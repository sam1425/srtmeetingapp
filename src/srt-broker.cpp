#include <obs-module.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "scene-manager.h"

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
	return srt_setsockopt(sock, 0, SRTO_REUSEADDR, &yes, sizeof(yes)) !=
	       SRT_ERROR;
}

static std::string extract_username(const std::string &stream_id,
				    int &out_latency_ms)
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
		obs_log(LOG_ERROR, "srt_create_socket: %s",
			srt_getlasterror_str());
		return false;
	}

	if (!set_reuse_addr(listener_socket_)) {
		obs_log(LOG_WARNING, "SRTO_REUSEADDR failed (non-fatal)");
	}

	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;

	if (srt_bind(listener_socket_, reinterpret_cast<sockaddr *>(&sa),
		     sizeof(sa)) == SRT_ERROR) {
		obs_log(LOG_ERROR, "srt_bind(%d): %s", port,
			srt_getlasterror_str());
		srt_close(listener_socket_);
		return false;
	}

	if (srt_listen(listener_socket_, 5) == SRT_ERROR) {
		obs_log(LOG_ERROR, "srt_listen: %s",
			srt_getlasterror_str());
		srt_close(listener_socket_);
		return false;
	}

	running_ = true;
	listener_thread_ =
		std::make_unique<std::thread>(&SRTBroker::listen_loop, this);

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

	{
		std::lock_guard<std::mutex> lock(handler_futures_mutex_);
		for (auto &f : handler_futures_)
			f.get();
		handler_futures_.clear();
	}

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
	while (running_) {
		sockaddr_in addr{};
		int addr_len = sizeof(addr);

		SRTSOCKET ls = listener_socket_.load();
		if (ls == SRT_INVALID_SOCK)
			break;

		SRTSOCKET client =
			srt_accept(ls, reinterpret_cast<sockaddr *>(&addr),
				   &addr_len);
		if (client == SRT_INVALID_SOCK) {
			if (running_)
				obs_log(LOG_WARNING, "srt_accept: %s",
					srt_getlasterror_str());
			continue;
		}

		char raw_stream_id[512] = {};
		int opt_len = sizeof(raw_stream_id) - 1;
		if (srt_getsockopt(client, 0, SRTO_STREAMID, raw_stream_id,
				   &opt_len) == SRT_ERROR) {
			obs_log(LOG_WARNING, "SRTO_STREAMID: %s",
				srt_getlasterror_str());
			srt_close(client);
			continue;
		}

		int latency_ms = 0;
		std::string username =
			extract_username(raw_stream_id, latency_ms);

		if (username.empty()) {
			obs_log(LOG_WARNING,
				"Rejecting connection with StreamID: %s",
				raw_stream_id);
			srt_close(client);
			continue;
		}

		obs_log(LOG_INFO, "Publisher '%s' connected (latency=%dms)",
			username.c_str(), latency_ms);

		{
			std::lock_guard<std::mutex> lock(
				handler_futures_mutex_);
			for (auto it = handler_futures_.begin();
			     it != handler_futures_.end();) {
				if (it->wait_for(std::chrono::seconds(0)) ==
				    std::future_status::ready)
					it = handler_futures_.erase(it);
				else
					++it;
			}
			handler_futures_.push_back(std::async(
				std::launch::async,
				&SRTBroker::handle_new_publisher, this,
				username, client, latency_ms));
		}
	}
}

/* ---------------------------------------------------------------------------
 * handle_new_publisher – set up relay port, notify SceneManager, spawn
 *                        relay thread.
 * -----------------------------------------------------------------------*/

void SRTBroker::handle_new_publisher(const std::string &username,
				     SRTSOCKET client_sock, int latency_ms)
{
	try {
		/* Clean up any existing session for this name. */
		std::shared_ptr<Participant> old;
		{
			std::lock_guard<std::mutex> lock(participants_mutex_);
			auto it = participants_.find(username);
			if (it != participants_.end()) {
				old = it->second;
				participants_.erase(it);
			}
		}
		if (old) {
			obs_log(LOG_INFO, "Reconnect: cleaning up '%s'",
				username.c_str());
			old->active = false;
			close_socket_once(old->client_socket);
			close_socket_once(old->relay_socket);
			if (old->relay_port >= 0)
				port_pool_.release(old->relay_port);
			s_scene_manager.remove_participant(username);
			if (old->relay_thread && old->relay_thread->joinable()) {
				if (old->relay_thread->get_id() !=
				    std::this_thread::get_id())
					old->relay_thread->join();
				else
					old->relay_thread->detach();
			}
		}

		/* Allocate relay port. */
		int port = port_pool_.allocate();
		if (port < 0) {
			obs_log(LOG_ERROR,
				"No relay ports available for '%s'",
				username.c_str());
			srt_close(client_sock);
			return;
		}

		auto p = std::make_shared<Participant>();
		p->name = username;
		p->client_socket.store(client_sock);
		p->relay_port = port;
		{
			std::lock_guard<std::mutex> lock(participants_mutex_);
			participants_[username] = p;
		}

		/* Apply SRT latency. */
		if (latency_ms > 0)
			srt_setsockopt(client_sock, 0, SRTO_RCVLATENCY,
				       &latency_ms, sizeof(latency_ms));

		s_scene_manager.add_participant(username, port);

		p->relay_thread = std::make_unique<std::thread>(
			&SRTBroker::relay_loop, this, p);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "handle_new_publisher: %s", e.what());
		srt_close(client_sock);
	} catch (...) {
		obs_log(LOG_ERROR, "handle_new_publisher: unknown error");
		srt_close(client_sock);
	}
}

/* ---------------------------------------------------------------------------
 * relay_loop – bind a local port, wait for OBS to connect, then pipe data
 *              from the client socket to the OBS socket.
 * -----------------------------------------------------------------------*/

void SRTBroker::relay_loop(std::shared_ptr<Participant> p)
{
	SRTSOCKET relay_listener = SRT_INVALID_SOCK;
	SRTSOCKET obs_sock = SRT_INVALID_SOCK;

	try {
		/* Create relay listener. */
		relay_listener = srt_create_socket();
		if (relay_listener == SRT_INVALID_SOCK) {
			obs_log(LOG_ERROR, "relay socket for '%s': %s",
				p->name.c_str(), srt_getlasterror_str());
			cleanup_participant(p);
			return;
		}

		set_reuse_addr(relay_listener);

		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_port = htons(p->relay_port);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (srt_bind(relay_listener, reinterpret_cast<sockaddr *>(&sa),
			     sizeof(sa)) == SRT_ERROR) {
			obs_log(LOG_ERROR, "relay bind(:%d) for '%s': %s",
				p->relay_port, p->name.c_str(),
				srt_getlasterror_str());
			srt_close(relay_listener);
			cleanup_participant(p);
			return;
		}

		if (srt_listen(relay_listener, 1) == SRT_ERROR) {
			obs_log(LOG_ERROR, "relay listen(:%d) for '%s': %s",
				p->relay_port, p->name.c_str(),
				srt_getlasterror_str());
			srt_close(relay_listener);
			cleanup_participant(p);
			return;
		}

		p->relay_socket.store(relay_listener);

		obs_log(LOG_INFO, "Relay :%d ready for '%s', waiting for OBS",
			p->relay_port, p->name.c_str());

		/* Wait for OBS to connect (10s timeout). */
		int timeout_ms = 10000;
		srt_setsockopt(relay_listener, 0, SRTO_RCVTIMEO, &timeout_ms,
			       sizeof(timeout_ms));

		sockaddr_in obs_addr{};
		int obs_addr_len = sizeof(obs_addr);
		obs_sock = srt_accept(relay_listener,
				      reinterpret_cast<sockaddr *>(&obs_addr),
				      &obs_addr_len);

		if (obs_sock == SRT_INVALID_SOCK) {
			if (running_ && p->active)
				obs_log(LOG_WARNING,
					"OBS did not connect to :%d for '%s'",
					p->relay_port, p->name.c_str());
			srt_close(relay_listener);
			p->relay_socket.store(SRT_INVALID_SOCK);
			cleanup_participant(p);
			return;
		}

		obs_log(LOG_INFO, "OBS connected to relay :%d for '%s'",
			p->relay_port, p->name.c_str());

		/* Set client receive timeout. */
		int recv_timeout = 500;
		SRTSOCKET cs = p->client_socket.load();
		if (cs != SRT_INVALID_SOCK)
			srt_setsockopt(cs, 0, SRTO_RCVTIMEO, &recv_timeout,
				       sizeof(recv_timeout));

		/* Relay data: client -> OBS. */
		constexpr int BUF_SIZE = 1316 * 16;
		std::vector<uint8_t> buf(BUF_SIZE);

		while (running_ && p->active) {
			SRTSOCKET cs = p->client_socket.load();
			if (cs == SRT_INVALID_SOCK)
				break;

			int n = srt_recvmsg(cs, reinterpret_cast<char *>(buf.data()),
					    static_cast<int>(buf.size()));
			if (n == SRT_ERROR) {
				int err = srt_getlasterror(NULL);
				if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT)
					continue;
				obs_log(LOG_DEBUG, "recv error for '%s': %s",
					p->name.c_str(),
					srt_getlasterror_str());
				break;
			}
			if (n == 0)
				break;

			int sent = srt_sendmsg(obs_sock,
					       reinterpret_cast<const char *>(
						       buf.data()),
					       n, -1, true);
			if (sent == SRT_ERROR) {
				obs_log(LOG_DEBUG, "relay send error for '%s': %s",
					p->name.c_str(),
					srt_getlasterror_str());
				break;
			}
		}

		srt_close(obs_sock);
		obs_sock = SRT_INVALID_SOCK;
		srt_close(relay_listener);
		p->relay_socket.store(SRT_INVALID_SOCK);

		obs_log(LOG_INFO, "Relay stopped for '%s'", p->name.c_str());
		cleanup_participant(p);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "relay_loop('%s'): %s", p->name.c_str(),
			e.what());
		if (obs_sock != SRT_INVALID_SOCK)
			srt_close(obs_sock);
		if (relay_listener != SRT_INVALID_SOCK)
			srt_close(relay_listener);
		p->relay_socket.store(SRT_INVALID_SOCK);
		cleanup_participant(p);
	} catch (...) {
		obs_log(LOG_ERROR, "relay_loop('%s'): unknown error",
			p->name.c_str());
		if (obs_sock != SRT_INVALID_SOCK)
			srt_close(obs_sock);
		if (relay_listener != SRT_INVALID_SOCK)
			srt_close(relay_listener);
		p->relay_socket.store(SRT_INVALID_SOCK);
		cleanup_participant(p);
	}
}

/* ---------------------------------------------------------------------------
 * cleanup_participant – tear down sockets, free port, remove from scene.
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
	close_socket_once(p->relay_socket);

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
		if (s_scene_manager.is_automatic_mode()) {
			s_scene_manager.remove_participant(p->name);
		} else {
			obs_log(LOG_INFO, "Static mode: keeping '%s'",
				p->name.c_str());
		}
	}

	obs_log(LOG_DEBUG, "Cleaned up participant '%s'", p->name.c_str());
}
