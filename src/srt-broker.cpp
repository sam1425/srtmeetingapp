#include <obs-module.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "scene-manager.h"

#include <sstream>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

SRTBroker s_broker;

static void close_srt_socket_once(std::atomic<SRTSOCKET> &sock)
{
	SRTSOCKET s = sock.exchange(SRT_INVALID_SOCK);
	if (s != SRT_INVALID_SOCK) {
		srt_close(s);
	}
}

/* ---------------------------------------------------------------------------
 * PortPool Implementation
 * -----------------------------------------------------------------------*/

PortPool::PortPool(int base, int count)
{
	for (int i = 0; i < count; ++i) {
		available_.insert(base + i);
	}
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

bool PortPool::is_available(int port) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return available_.count(port) > 0;
}

/* ---------------------------------------------------------------------------
 * SRTBroker Implementation
 * -----------------------------------------------------------------------*/

SRTBroker::SRTBroker()
	: running(false), listener_socket(SRT_INVALID_SOCK), port_pool_(10001, 100)
{
}

SRTBroker::~SRTBroker()
{
	stop();
}

std::vector<std::string> SRTBroker::get_active_participant_names()
{
	std::lock_guard<std::mutex> lock(participants_mutex);
	std::vector<std::string> names;
	for (const auto &pair : participants) {
		names.push_back(pair.first);
	}
	return names;
}

bool SRTBroker::start(int port)
{
	if (running)
		return true;

	if (srt_startup() != 0) {
		obs_log(LOG_ERROR, "Failed to startup SRT library");
		return false;
	}

	listener_socket = srt_create_socket();
	if (listener_socket == SRT_INVALID_SOCK) {
		obs_log(LOG_ERROR, "Failed to create SRT socket: %s",
			srt_getlasterror_str());
		return false;
	}

	int yes = 1;
	srt_setsockopt(listener_socket, 0, SRTO_REUSEADDR, &yes, sizeof(yes));

	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;

	if (srt_bind(listener_socket, (struct sockaddr *)&sa, sizeof(sa)) ==
	    SRT_ERROR) {
		obs_log(LOG_ERROR, "Failed to bind SRT socket to port %d: %s",
			port, srt_getlasterror_str());
		srt_close(listener_socket);
		return false;
	}

	if (srt_listen(listener_socket, 5) == SRT_ERROR) {
		obs_log(LOG_ERROR, "Failed to listen on SRT socket: %s",
			srt_getlasterror_str());
		srt_close(listener_socket);
		return false;
	}

	running = true;
	listener_thread =
		std::make_unique<std::thread>(&SRTBroker::listen_loop, this);
	obs_log(LOG_INFO, "SRT Broker started listening on port %d", port);
	return true;
}

void SRTBroker::stop()
{
	if (!running)
		return;
	running = false;

	close_srt_socket_once(listener_socket);

	if (listener_thread && listener_thread->joinable()) {
		listener_thread->join();
		listener_thread.reset();
	}

	{
		std::lock_guard<std::mutex> lock(handler_futures_mutex);
		for (auto &f : handler_futures) {
			f.wait();
		}
		handler_futures.clear();
	}

	std::vector<std::shared_ptr<Participant>> active_list;
	{
		std::lock_guard<std::mutex> lock(participants_mutex);
		for (auto &pair : participants) {
			active_list.push_back(pair.second);
		}
		participants.clear();
	}

	for (auto &p : active_list) {
		cleanup_participant(p);
	}

	srt_cleanup();
	obs_log(LOG_INFO, "SRT Broker stopped");
}

void SRTBroker::listen_loop()
{
	try {
		while (running) {
			sockaddr_in client_addr;
			int addr_len = sizeof(client_addr);
			SRTSOCKET ls = listener_socket.load();
			if (ls == SRT_INVALID_SOCK)
				break;
			SRTSOCKET client_sock =
				srt_accept(ls, (struct sockaddr *)&client_addr,
					   &addr_len);
			if (client_sock == SRT_INVALID_SOCK) {
				if (running) {
					obs_log(LOG_WARNING,
						"Accept failed: %s",
						srt_getlasterror_str());
				}
				continue;
			}

			char stream_id[512] = {0};
			int opt_len = sizeof(stream_id) - 1;
			if (srt_getsockopt(client_sock, 0, SRTO_STREAMID,
					   stream_id,
					   &opt_len) == SRT_ERROR) {
				obs_log(LOG_WARNING,
					"Failed to get stream ID: %s",
					srt_getlasterror_str());
				srt_close(client_sock);
				continue;
			}

			std::string stream_id_str(stream_id);
			obs_log(LOG_INFO,
				"Incoming connection with StreamID: %s",
				stream_id);

			const std::string prefix = "publish:";
			if (stream_id_str.rfind(prefix, 0) == 0) {
				std::string after_prefix =
					stream_id_str.substr(prefix.length());

				int client_latency_ms = 0;
				size_t query_pos =
					after_prefix.find_first_of("?;");
				if (query_pos != std::string::npos) {
					std::string query =
						after_prefix.substr(
							query_pos + 1);
					size_t lat_pos =
						query.find("latency=");
					if (lat_pos != std::string::npos) {
						std::string val = query.substr(
							lat_pos + 8);
						size_t end = val.find_first_of(
							"&;");
						if (end != std::string::npos)
							val = val.substr(0, end);
						if (val != "auto") {
							try {
								client_latency_ms =
									std::stoi(
										val) /
									1000;
							} catch (...) {
							}
						}
					}
				}

				std::string username =
					after_prefix.substr(0, query_pos);

				for (char &c : username) {
					if (!isalnum(static_cast<unsigned char>(
						     c)) &&
					    c != '_') {
						c = '_';
					}
				}

				if (username.empty()) {
					obs_log(LOG_WARNING,
						"Empty username, rejecting stream");
					srt_close(client_sock);
					continue;
				}

				{
					std::lock_guard<std::mutex> lock(
						handler_futures_mutex);
					for (auto it =
						     handler_futures.begin();
					     it != handler_futures.end();) {
						if (it->wait_for(
							    std::chrono::
								    seconds(0)) ==
						    std::future_status::
							    ready) {
							it = handler_futures
								     .erase(it);
						} else {
							++it;
						}
					}
					handler_futures.push_back(
						std::async(std::launch::async,
							   &SRTBroker::handle_new_publisher,
							   this, username,
							   client_sock,
							   client_latency_ms));
				}
			} else {
				obs_log(LOG_WARNING,
					"Invalid StreamID. Expected 'publish:<name>', got '%s'",
					stream_id);
				srt_close(client_sock);
			}
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception in listen_loop: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "Unknown exception in listen_loop");
	}
}

void SRTBroker::handle_new_publisher(std::string username,
				     SRTSOCKET client_sock, int latency_ms)
{
	try {
		std::shared_ptr<Participant> old_p;
		{
			std::lock_guard<std::mutex> lock(participants_mutex);
			auto it = participants.find(username);
			if (it != participants.end()) {
				old_p = it->second;
				participants.erase(it);
			}
		}

		if (old_p) {
			obs_log(LOG_INFO,
				"Participant %s reconnected. Cleaning up old session.",
				username.c_str());
			old_p->active = false;
			close_srt_socket_once(old_p->client_socket);
			close_srt_socket_once(old_p->relay_socket);
			if (old_p->relay_port >= 0) {
				port_pool_.release(old_p->relay_port);
			}
			s_scene_manager.remove_participant(username);
			if (old_p->relay_thread &&
			    old_p->relay_thread->joinable()) {
				if (old_p->relay_thread->get_id() !=
				    std::this_thread::get_id()) {
					old_p->relay_thread->join();
				} else {
					old_p->relay_thread->detach();
				}
			}
		}

		int relay_port = port_pool_.allocate();
		if (relay_port < 0) {
			obs_log(LOG_ERROR,
				"No available relay ports for participant %s",
				username.c_str());
			srt_close(client_sock);
			return;
		}

		auto participant = std::make_shared<Participant>();
		participant->name = username;
		participant->client_socket.store(client_sock);
		participant->relay_port = relay_port;
		participant->active = true;

		{
			std::lock_guard<std::mutex> lock(participants_mutex);
			participants[username] = participant;
		}

		obs_log(LOG_INFO,
			"Registered participant: %s, assigned relay port %d",
			username.c_str(), relay_port);

		int final_latency_ms =
			latency_ms > 0 ? latency_ms : 120;
		srt_setsockopt(client_sock, 0, SRTO_RCVLATENCY,
			       &final_latency_ms, sizeof(final_latency_ms));

		s_scene_manager.add_participant(username, relay_port);

		participant->relay_thread = std::make_unique<std::thread>(
			&SRTBroker::relay_loop, this, participant);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception in handle_new_publisher: %s",
			e.what());
		srt_close(client_sock);
	} catch (...) {
		obs_log(LOG_ERROR, "Unknown exception in handle_new_publisher");
		srt_close(client_sock);
	}
}

void SRTBroker::relay_loop(std::shared_ptr<Participant> p)
{
	try {
		SRTSOCKET relay_listener = srt_create_socket();
		if (relay_listener == SRT_INVALID_SOCK) {
			obs_log(LOG_ERROR,
				"Failed to create relay socket for %s: %s",
				p->name.c_str(), srt_getlasterror_str());
			cleanup_participant(p);
			return;
		}

		int yes = 1;
		srt_setsockopt(relay_listener, 0, SRTO_REUSEADDR, &yes,
			       sizeof(yes));

		sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(p->relay_port);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (srt_bind(relay_listener, (struct sockaddr *)&sa,
			     sizeof(sa)) == SRT_ERROR) {
			obs_log(LOG_ERROR,
				"Failed to bind relay socket to port %d for %s: %s",
				p->relay_port, p->name.c_str(),
				srt_getlasterror_str());
			srt_close(relay_listener);
			cleanup_participant(p);
			return;
		}

		if (srt_listen(relay_listener, 1) == SRT_ERROR) {
			obs_log(LOG_ERROR,
				"Failed to listen on relay port %d for %s: %s",
				p->relay_port, p->name.c_str(),
				srt_getlasterror_str());
			srt_close(relay_listener);
			cleanup_participant(p);
			return;
		}

		p->relay_socket.store(relay_listener);

		obs_log(LOG_INFO,
			"Relay listener ready on port %d for %s, waiting for OBS...",
			p->relay_port, p->name.c_str());

		sockaddr_in obs_addr;
		int obs_addr_len = sizeof(obs_addr);
		int accept_timeout_ms = 10000;
		srt_setsockopt(relay_listener, 0, SRTO_RCVTIMEO,
			       &accept_timeout_ms, sizeof(accept_timeout_ms));

		SRTSOCKET obs_sock =
			srt_accept(relay_listener,
				   (struct sockaddr *)&obs_addr,
				   &obs_addr_len);

		if (obs_sock == SRT_INVALID_SOCK) {
			if (running && p->active) {
				obs_log(LOG_WARNING,
					"OBS did not connect to relay port %d for %s within timeout",
					p->relay_port, p->name.c_str());
			}
			srt_close(relay_listener);
			p->relay_socket.store(SRT_INVALID_SOCK);
			cleanup_participant(p);
			return;
		}

		obs_log(LOG_INFO,
			"OBS connected to relay port %d for %s, starting relay",
			p->relay_port, p->name.c_str());

		constexpr int RECV_BUFFER_SIZE = 1316 * 16;
		std::vector<uint8_t> buffer(RECV_BUFFER_SIZE);

		int client_timeout_ms = 500;
		SRTSOCKET cs = p->client_socket.load();
		if (cs != SRT_INVALID_SOCK) {
			srt_setsockopt(cs, 0, SRTO_RCVTIMEO,
				       &client_timeout_ms,
				       sizeof(client_timeout_ms));
		}

		while (running && p->active) {
			SRTSOCKET recv_sock = p->client_socket.load();
			if (recv_sock == SRT_INVALID_SOCK) {
				obs_log(LOG_INFO,
					"Client socket gone for %s, stopping relay",
					p->name.c_str());
				break;
			}

			int bytes_read = srt_recvmsg(
				recv_sock,
				reinterpret_cast<char *>(buffer.data()),
				buffer.size());

			if (bytes_read == SRT_ERROR) {
				int err = srt_getlasterror(NULL);
				if (err == SRT_EASYNCRCV ||
				    err == SRT_ETIMEOUT) {
					continue;
				}
				obs_log(LOG_INFO,
					"Client socket read error for %s: %s",
					p->name.c_str(),
					srt_getlasterror_str());
				break;
			}

			if (bytes_read == 0) {
				obs_log(LOG_INFO,
					"Client disconnected (EOF) for %s",
					p->name.c_str());
				break;
			}

			int sent = srt_sendmsg(
				obs_sock,
				reinterpret_cast<const char *>(
					buffer.data()),
				bytes_read, -1, true);

			if (sent == SRT_ERROR) {
				obs_log(LOG_INFO,
					"Relay send error for %s (OBS disconnected): %s",
					p->name.c_str(),
					srt_getlasterror_str());
				break;
			}
		}

		srt_close(obs_sock);
		srt_close(relay_listener);
		p->relay_socket.store(SRT_INVALID_SOCK);

		obs_log(LOG_INFO, "Relay stopped for %s", p->name.c_str());
		cleanup_participant(p);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception in relay_loop for %s: %s",
			p->name.c_str(), e.what());
		cleanup_participant(p);
	} catch (...) {
		obs_log(LOG_ERROR, "Unknown exception in relay_loop for %s",
			p->name.c_str());
		cleanup_participant(p);
	}
}

void SRTBroker::cleanup_participant(std::shared_ptr<Participant> p)
{
	if (!p)
		return;

	bool was_in_map = false;
	{
		std::lock_guard<std::mutex> lock(participants_mutex);
		auto it = participants.find(p->name);
		if (it != participants.end() && it->second == p) {
			participants.erase(it);
			was_in_map = true;
		}
	}

	p->active = false;
	close_srt_socket_once(p->client_socket);
	close_srt_socket_once(p->relay_socket);

	if (p->relay_port >= 0) {
		port_pool_.release(p->relay_port);
		p->relay_port = -1;
	}

	if (p->relay_thread) {
		if (p->relay_thread->get_id() == std::this_thread::get_id()) {
			p->relay_thread->detach();
		} else if (p->relay_thread->joinable()) {
			p->relay_thread->join();
		}
	}

	if (was_in_map) {
		if (s_scene_manager.is_automatic_mode()) {
			s_scene_manager.remove_participant(p->name);
		} else {
			obs_log(LOG_INFO,
				"Static mode: Keeping source 'SRT - %s' on disconnect",
				p->name.c_str());
		}
	}
	obs_log(LOG_INFO, "Cleaned up participant session %s",
		p->name.c_str());
}
