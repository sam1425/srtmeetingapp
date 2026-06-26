#include <obs-module.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "scene-manager.h"
#include "plugin-dock.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

SRTBroker s_broker;

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static void close_socket_once(std::atomic<SRTSOCKET> &sock)
{
	SRTSOCKET s = sock.exchange(SRT_INVALID_SOCK);
	if (s != SRT_INVALID_SOCK)
		srt_close(s);
}

static bool set_reuse_addr(SRTSOCKET sock)
{
	int yes = 1;
	return srt_setsockopt(sock, 0, SRTO_REUSEADDR, &yes, sizeof(yes)) != SRT_ERROR;
}

static std::string extract_username(const std::string &stream_id, int &out_latency_ms)
{
	out_latency_ms = 0;

	const std::string prefix = "publish:";
	if (stream_id.rfind(prefix, 0) != 0)
		return {};

	std::string rest = stream_id.substr(prefix.size());

	size_t query_pos = rest.find_first_of("?;");
	if (query_pos != std::string::npos) {
		std::string query = rest.substr(query_pos + 1);
		size_t lat_pos = query.find("latency=");
		if (lat_pos != std::string::npos) {
			std::string val = query.substr(lat_pos + 8);
			size_t end = val.find_first_of("&;");
			if (end != std::string::npos)
				val = val.substr(0, end);
			if (val != "auto") {
				try {
					out_latency_ms = std::stoi(val) / 1000;
				} catch (...) {
				}
			}
		}
		rest = rest.substr(0, query_pos);
	}

	std::string name = rest;
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
		return std::isalnum(c) || c == '_' ? c : '_';
	});

	return name.empty() ? std::string{} : name;
}

/* ---------------------------------------------------------------------------
 * PortPool
 * -----------------------------------------------------------------------*/

PortPool::PortPool(int base_port, int count)
{
	for (int i = 0; i < count; ++i)
		available_.insert(base_port + i);
}

int PortPool::allocate()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (available_.empty())
		return -1;
	int port = *available_.begin();
	available_.erase(available_.begin());
	return port;
}

void PortPool::release(int port)
{
	std::lock_guard<std::mutex> lock(mutex_);
	available_.insert(port);
}

/* ---------------------------------------------------------------------------
 * SRTBroker
 * -----------------------------------------------------------------------*/

SRTBroker::SRTBroker() : port_pool_(10001, 100) {}

SRTBroker::~SRTBroker()
{
	stop();
}

bool SRTBroker::start(int port)
{
	if (running_)
		return true;

	if (srt_startup() != 0) {
		obs_log(LOG_ERROR, "srt_startup failed");
		return false;
	}

	listener_socket_ = srt_create_socket();
	if (listener_socket_ == SRT_INVALID_SOCK) {
		obs_log(LOG_ERROR, "srt_create_socket: %s", srt_getlasterror_str());
		return false;
	}

	if (!set_reuse_addr(listener_socket_)) {
		obs_log(LOG_WARNING, "SRTO_REUSEADDR failed (non-fatal)");
	}

	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;

	if (srt_bind(listener_socket_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == SRT_ERROR) {
		obs_log(LOG_ERROR, "srt_bind(%d): %s", port, srt_getlasterror_str());
		srt_close(listener_socket_);
		return false;
	}

	if (srt_listen(listener_socket_, 5) == SRT_ERROR) {
		obs_log(LOG_ERROR, "srt_listen: %s", srt_getlasterror_str());
		srt_close(listener_socket_);
		return false;
	}

	running_ = true;
	listener_thread_ = std::make_unique<std::thread>(&SRTBroker::listen_loop, this);

	obs_log(LOG_INFO, "Broker listening on port %d", port);
	return true;
}

void SRTBroker::stop()
{
	if (!running_)
		return;
	running_ = false;

	close_socket_once(listener_socket_);

	if (listener_thread_ && listener_thread_->joinable())
		listener_thread_->join();
	listener_thread_.reset();

	std::vector<std::shared_ptr<Participant>> snapshot;
	{
		std::lock_guard<std::mutex> lock(participants_mutex_);
		for (auto &[name, p] : participants_)
			snapshot.push_back(p);
		participants_.clear();
	}

	for (auto &p : snapshot)
		cleanup_participant(p);

	srt_cleanup();
	obs_log(LOG_INFO, "Broker stopped");
}

std::vector<std::string> SRTBroker::get_active_participant_names()
{
	std::lock_guard<std::mutex> lock(participants_mutex_);
	std::vector<std::string> names;
	names.reserve(participants_.size());
	for (auto &[name, _] : participants_)
		names.push_back(name);
	return names;
}

/* ---------------------------------------------------------------------------
 * listen_loop – accept incoming SRT connections, parse StreamID, dispatch.
 * -----------------------------------------------------------------------*/

void SRTBroker::listen_loop()
{
	int eid = srt_epoll_create();
	listener_epoll_id_.store(eid);

	SRTSOCKET ls = listener_socket_.load();
	if (ls != SRT_INVALID_SOCK) {
		int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
		srt_epoll_add_usock(eid, ls, &events);
	}

	while (running_) {
		SRT_EPOLL_EVENT ready[1];
		int nready = srt_epoll_uwait(eid, ready, 1, 500);

		if (!running_)
			break;

		if (nready > 0) {
			sockaddr_in addr{};
			int addr_len = sizeof(addr);

			SRTSOCKET ls_curr = listener_socket_.load();
			if (ls_curr == SRT_INVALID_SOCK)
				break;

			SRTSOCKET client = srt_accept(ls_curr, reinterpret_cast<sockaddr *>(&addr), &addr_len);
			if (client == SRT_INVALID_SOCK) {
				if (running_)
					obs_log(LOG_WARNING, "srt_accept: %s", srt_getlasterror_str());
				continue;
			}

			char raw_stream_id[512] = {};
			int opt_len = sizeof(raw_stream_id) - 1;
			if (srt_getsockopt(client, 0, SRTO_STREAMID, raw_stream_id, &opt_len) == SRT_ERROR) {
				obs_log(LOG_WARNING, "SRTO_STREAMID missing: %s", srt_getlasterror_str());
				srt_close(client);
				continue;
			}

			int latency_ms = 0;
			std::string username = extract_username(raw_stream_id, latency_ms);

			if (username.empty()) {
				obs_log(LOG_WARNING, "Rejecting connection with StreamID: %s", raw_stream_id);
				srt_close(client);
				continue;
			}

			obs_log(LOG_INFO, "Publisher '%s' connected (latency=%dms)", username.c_str(), latency_ms);

			// Dispatch asynchronously to avoid blocking the accept loop
			std::thread(&SRTBroker::handle_new_publisher, this, username, client, latency_ms).detach();
		}
	}

	srt_epoll_release(eid);
	listener_epoll_id_.store(-1);
}

/* ---------------------------------------------------------------------------
 * handle_new_publisher – cleanly stop old session, set up new relay.
 * -----------------------------------------------------------------------*/

void SRTBroker::handle_new_publisher(std::string username, SRTSOCKET client_sock, int latency_ms)
{
	try {
		std::shared_ptr<Participant> old;
		{
			std::lock_guard<std::mutex> lock(participants_mutex_);
			auto it = participants_.find(username);
			if (it != participants_.end()) {
				old = it->second;
			}
		}

		if (old) {
			obs_log(LOG_INFO, "Reconnect: cleaning up old session for '%s'", username.c_str());
			old->active = false;
			close_socket_once(old->client_socket);
			close_socket_once(old->obs_socket);
			close_socket_once(old->relay_listener);

			if (old->relay_thread && old->relay_thread->joinable()) {
				if (old->relay_thread->get_id() != std::this_thread::get_id())
					old->relay_thread->join();
				else
					old->relay_thread->detach();
			}

			if (old->relay_port >= 0) {
				port_pool_.release(old->relay_port);
			}
		}

		int port = port_pool_.allocate();
		if (port < 0) {
			obs_log(LOG_ERROR, "No relay ports available for '%s'", username.c_str());
			srt_close(client_sock);
			return;
		}

		auto p = std::make_shared<Participant>();
		p->name = username;
		p->client_socket.store(client_sock);
		p->relay_port = port;
		p->latency_ms = latency_ms;

		{
			std::lock_guard<std::mutex> lock(participants_mutex_);
			participants_[username] = p;
		}

		if (latency_ms > 0)
			srt_setsockopt(client_sock, 0, SRTO_RCVLATENCY, &latency_ms, sizeof(latency_ms));

		// Tell SceneManager to create/update the ffmpeg source
		s_scene_manager.add_participant(username, port);
		update_participants_list_ui();

		p->relay_thread = std::make_unique<std::thread>(&SRTBroker::relay_loop, this, p);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "handle_new_publisher: %s", e.what());
		srt_close(client_sock);
	} catch (...) {
		obs_log(LOG_ERROR, "handle_new_publisher: unknown error");
		srt_close(client_sock);
	}
}

/* ---------------------------------------------------------------------------
 * relay_loop – rock solid epoll loop waiting for OBS and shuffling data
 * -----------------------------------------------------------------------*/

void SRTBroker::relay_loop(std::shared_ptr<Participant> p)
{
	SRTSOCKET listener = srt_create_socket();
	if (listener == SRT_INVALID_SOCK) {
		obs_log(LOG_ERROR, "relay socket for '%s': %s", p->name.c_str(), srt_getlasterror_str());
		// Async dispatch to avoid deadlocks when joining this thread inside cleanup
		std::thread([this, p]() { cleanup_participant(p); }).detach();
		return;
	}

	set_reuse_addr(listener);

	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(p->relay_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (srt_bind(listener, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == SRT_ERROR) {
		obs_log(LOG_ERROR, "relay bind(:%d) for '%s': %s", p->relay_port, p->name.c_str(), srt_getlasterror_str());
		srt_close(listener);
		std::thread([this, p]() { cleanup_participant(p); }).detach();
		return;
	}

	if (srt_listen(listener, 1) == SRT_ERROR) {
		obs_log(LOG_ERROR, "relay listen(:%d) for '%s': %s", p->relay_port, p->name.c_str(), srt_getlasterror_str());
		srt_close(listener);
		std::thread([this, p]() { cleanup_participant(p); }).detach();
		return;
	}

	p->relay_listener.store(listener);

	int eid = srt_epoll_create();
	p->epoll_id.store(eid);

	int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	srt_epoll_add_usock(eid, listener, &events);

	SRTSOCKET cs = p->client_socket.load();
	if (cs != SRT_INVALID_SOCK) {
		int c_events = SRT_EPOLL_ERR; // Wait for OBS before reading from client
		srt_epoll_add_usock(eid, cs, &c_events);
	}

	obs_log(LOG_INFO, "Relay :%d ready for '%s', waiting for OBS", p->relay_port, p->name.c_str());

	constexpr int BUF_SIZE = 1316 * 16;
	std::vector<uint8_t> buf(BUF_SIZE);

	while (running_ && p->active) {
		SRT_EPOLL_EVENT ready[4];
		int nready = srt_epoll_uwait(eid, ready, 4, 500);

		if (!running_ || !p->active)
			break;

		if (nready > 0) {
			bool client_ready = false;
			bool client_err = false;
			bool obs_err = false;
			bool obs_connecting = false;

			SRTSOCKET c_sock = p->client_socket.load();
			SRTSOCKET o_sock = p->obs_socket.load();
			SRTSOCKET l_sock = p->relay_listener.load();

			for (int i = 0; i < nready; i++) {
				if (ready[i].fd == c_sock) {
					if (ready[i].events & SRT_EPOLL_ERR) client_err = true;
					if (ready[i].events & SRT_EPOLL_IN) client_ready = true;
				} else if (ready[i].fd == o_sock) {
					if (ready[i].events & SRT_EPOLL_ERR) obs_err = true;
				} else if (ready[i].fd == l_sock) {
					if (ready[i].events & SRT_EPOLL_IN) obs_connecting = true;
				}
			}

			if (client_err) {
				obs_log(LOG_INFO, "Client disconnected '%s'", p->name.c_str());
				break;
			}

			if (obs_err) {
				obs_log(LOG_INFO, "OBS disconnected from relay %d ('%s')", p->relay_port, p->name.c_str());
				srt_epoll_remove_usock(eid, o_sock);
				close_socket_once(p->obs_socket);
				
				// Stop reading from client to let SRT buffer handle it
				if (c_sock != SRT_INVALID_SOCK) {
					int c_events = SRT_EPOLL_ERR;
					srt_epoll_update_usock(eid, c_sock, &c_events);
				}
				continue;
			}

			if (obs_connecting) {
				sockaddr_in obs_addr{};
				int obs_len = sizeof(obs_addr);
				SRTSOCKET new_obs = srt_accept(l_sock, reinterpret_cast<sockaddr *>(&obs_addr), &obs_len);
				
				if (new_obs != SRT_INVALID_SOCK) {
					obs_log(LOG_INFO, "OBS connected to relay :%d for '%s'", p->relay_port, p->name.c_str());
					p->obs_socket.store(new_obs);
					
					int o_events = SRT_EPOLL_ERR;
					srt_epoll_add_usock(eid, new_obs, &o_events);
					
					if (c_sock != SRT_INVALID_SOCK) {
						int c_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
						srt_epoll_update_usock(eid, c_sock, &c_events);
					}
				}
			}

			if (client_ready && p->obs_socket.load() != SRT_INVALID_SOCK) {
				int n = srt_recvmsg(c_sock, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()));
				if (n == SRT_ERROR) {
					obs_log(LOG_DEBUG, "recv error for '%s': %s", p->name.c_str(), srt_getlasterror_str());
					break;
				}
				
				if (n > 0) {
					int sent = srt_sendmsg(p->obs_socket.load(), reinterpret_cast<const char *>(buf.data()), n, -1, true);
					if (sent == SRT_ERROR) {
						// Write error to OBS. Tear down OBS socket and wait for it to reconnect
						srt_epoll_remove_usock(eid, p->obs_socket.load());
						close_socket_once(p->obs_socket);
						
						if (c_sock != SRT_INVALID_SOCK) {
							int c_events = SRT_EPOLL_ERR;
							srt_epoll_update_usock(eid, c_sock, &c_events);
						}
					}
				}
			}
		}
	}

	srt_epoll_release(eid);
	p->epoll_id.store(-1);

	obs_log(LOG_INFO, "Relay stopped for '%s'", p->name.c_str());
	
	std::thread([this, p]() { cleanup_participant(p); }).detach();
}

/* ---------------------------------------------------------------------------
 * cleanup_participant
 * -----------------------------------------------------------------------*/

void SRTBroker::cleanup_participant(std::shared_ptr<Participant> p)
{
	if (!p)
		return;

	bool was_active = false;
	{
		std::lock_guard<std::mutex> lock(participants_mutex_);
		auto it = participants_.find(p->name);
		if (it != participants_.end() && it->second == p) {
			participants_.erase(it);
			was_active = true;
		}
	}

	p->active = false;
	close_socket_once(p->client_socket);
	close_socket_once(p->obs_socket);
	close_socket_once(p->relay_listener);

	if (p->relay_port >= 0) {
		port_pool_.release(p->relay_port);
		p->relay_port = -1;
	}

	if (p->relay_thread && p->relay_thread->joinable()) {
		if (p->relay_thread->get_id() == std::this_thread::get_id())
			p->relay_thread->detach();
		else
			p->relay_thread->join();
	}

	if (was_active) {
		update_participants_list_ui();
		if (s_scene_manager.is_automatic_mode()) {
			s_scene_manager.remove_participant(p->name);
		} else {
			obs_log(LOG_INFO, "Static mode: keeping '%s'", p->name.c_str());
		}
	}

	obs_log(LOG_DEBUG, "Cleaned up participant '%s'", p->name.c_str());
}
