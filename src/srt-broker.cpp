#include <obs-module.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "plugin-dock.h"

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

static std::mutex s_ports_mutex;
static std::set<int> s_allocated_ports;

SRTBroker s_broker;
std::atomic<bool> s_automatic_mode{true};
std::atomic<int> s_default_latency_ms{120};

static void release_loopback_port(int port)
{
	std::lock_guard<std::mutex> lock(s_ports_mutex);
	s_allocated_ports.erase(port);
}

static void close_srt_socket_once(std::atomic<SRTSOCKET> &sock)
{
	SRTSOCKET s = sock.exchange(SRT_INVALID_SOCK);
	if (s != SRT_INVALID_SOCK) {
		srt_close(s);
	}
}

SRTBroker::SRTBroker() : running(false), listener_socket(SRT_INVALID_SOCK) {}

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
			close_srt_socket_once(old_p->obs_socket);
			close_srt_socket_once(old_p->loopback_listener);

			if (old_p->relay_thread &&
			    old_p->relay_thread->joinable()) {
				old_p->relay_thread->join();
			}
		}

		int loopback_port = find_free_loopback_port();
		if (loopback_port < 0) {
			obs_log(LOG_ERROR,
				"No free loopback ports available for participant %s",
				username.c_str());
			srt_close(client_sock);
			return;
		}

		SRTSOCKET loopback_listener = srt_create_socket();
		if (loopback_listener == SRT_INVALID_SOCK) {
			obs_log(LOG_ERROR,
				"Failed to create loopback listener socket: %s",
				srt_getlasterror_str());
			srt_close(client_sock);
			release_loopback_port(loopback_port);
			return;
		}

		int yes = 1;
		srt_setsockopt(loopback_listener, 0, SRTO_REUSEADDR, &yes,
			       sizeof(yes));

		sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(loopback_port);
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (srt_bind(loopback_listener, (struct sockaddr *)&sa,
			     sizeof(sa)) == SRT_ERROR) {
			obs_log(LOG_ERROR,
				"Failed to bind loopback listener to port %d: %s",
				loopback_port, srt_getlasterror_str());
			srt_close(loopback_listener);
			srt_close(client_sock);
			release_loopback_port(loopback_port);
			return;
		}

		if (srt_listen(loopback_listener, 1) == SRT_ERROR) {
			obs_log(LOG_ERROR,
				"Failed to listen on loopback port %d: %s",
				loopback_port, srt_getlasterror_str());
			srt_close(loopback_listener);
			srt_close(client_sock);
			release_loopback_port(loopback_port);
			return;
		}

		auto participant = std::make_shared<Participant>();
		participant->name = username;
		participant->client_socket.store(client_sock);
		participant->loopback_port = loopback_port;
		participant->loopback_listener.store(loopback_listener);

		{
			std::lock_guard<std::mutex> lock(participants_mutex);
			participants[username] = participant;
		}

		obs_log(LOG_INFO,
			"Registered participant: %s, assigned local loopback port %d",
			username.c_str(), loopback_port);
		update_participants_list_ui();

		int final_latency_ms =
			latency_ms > 0 ? latency_ms : s_default_latency_ms.load();
		create_obs_srt_source(username, loopback_port, final_latency_ms);

		sockaddr_in obs_addr;
		int addr_len = sizeof(obs_addr);

		SRTSOCKET obs_sock = SRT_INVALID_SOCK;
		SRTSOCKET lb_sock = participant->loopback_listener.load();
		if (lb_sock != SRT_INVALID_SOCK) {
			int epoll_id = srt_epoll_create();
			if (epoll_id < 0) {
				obs_log(LOG_ERROR,
					"srt_epoll_create failed: %s",
					srt_getlasterror_str());
			} else {
				int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
				srt_epoll_add_usock(epoll_id, lb_sock,
						    &events);

				SRTSOCKET ready[1];
				int nready = 1;
				int wait_result = srt_epoll_wait(
					epoll_id, ready, &nready, nullptr,
					nullptr, 10000, nullptr, nullptr,
					nullptr, nullptr);
				srt_epoll_release(epoll_id);

				if (wait_result > 0 && nready > 0) {
					lb_sock =
						participant->loopback_listener
							.load();
					if (lb_sock != SRT_INVALID_SOCK) {
						obs_sock = srt_accept(
							lb_sock,
							(struct sockaddr *)
								&obs_addr,
							&addr_len);
					}
				} else {
					obs_log(LOG_ERROR,
						"Timed out waiting 10s for OBS to connect on loopback port %d",
						loopback_port);
				}
			}
		}
		if (obs_sock == SRT_INVALID_SOCK) {
			obs_log(LOG_ERROR,
				"OBS failed to connect to loopback listener on port %d: %s",
				loopback_port, srt_getlasterror_str());
			cleanup_participant(participant);
			return;
		}

		participant->obs_socket.store(obs_sock);

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
		char buffer[1316];
		obs_log(LOG_INFO, "Starting SRT relay loop for %s",
			p->name.c_str());

		int timeout_ms = 500;
		SRTSOCKET cs = p->client_socket.load();
		if (cs != SRT_INVALID_SOCK) {
			srt_setsockopt(cs, 0, SRTO_RCVTIMEO, &timeout_ms,
				       sizeof(timeout_ms));
		}

		while (running && p->active) {
			SRTSOCKET recv_sock = p->client_socket.load();
			if (recv_sock == SRT_INVALID_SOCK) {
				obs_log(LOG_INFO,
					"Client socket gone for %s, exiting relay loop",
					p->name.c_str());
				break;
			}

			SRTSOCKET send_sock = p->obs_socket.load();
			if (send_sock == SRT_INVALID_SOCK) {
				obs_log(LOG_INFO,
					"OBS disconnected from %s, waiting for reconnection...",
					p->name.c_str());
				update_participants_list_ui();
				SRTSOCKET new_obs =
					wait_for_obs_connection(p);
				if (new_obs == SRT_INVALID_SOCK) {
					obs_log(LOG_INFO,
						"OBS reconnection failed or broker stopped for %s",
						p->name.c_str());
					break;
				}
				p->obs_socket.store(new_obs);
				obs_log(LOG_INFO,
					"OBS reconnected to %s on loopback",
					p->name.c_str());
				update_participants_list_ui();
				continue;
			}

			int bytes_read =
				srt_recvmsg(recv_sock, buffer, sizeof(buffer));
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

			int bytes_written = srt_sendmsg(send_sock, buffer,
							bytes_read, -1, 0);
			if (bytes_written == SRT_ERROR) {
				obs_log(LOG_INFO,
					"OBS write error for %s: %s, waiting for reconnection...",
					p->name.c_str(),
					srt_getlasterror_str());
				close_srt_socket_once(p->obs_socket);
				continue;
			}
		}

		obs_log(LOG_INFO, "Stopping SRT relay loop for %s",
			p->name.c_str());
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

SRTSOCKET SRTBroker::wait_for_obs_connection(std::shared_ptr<Participant> p)
{
	SRTSOCKET lb_sock = p->loopback_listener.load();
	SRTSOCKET cl_sock = p->client_socket.load();
	if (lb_sock == SRT_INVALID_SOCK || cl_sock == SRT_INVALID_SOCK)
		return SRT_INVALID_SOCK;

	int epoll_id = srt_epoll_create();
	if (epoll_id < 0) {
		obs_log(LOG_ERROR,
			"srt_epoll_create failed in wait_for_obs_connection: %s",
			srt_getlasterror_str());
		return SRT_INVALID_SOCK;
	}

	int lb_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	srt_epoll_add_usock(epoll_id, lb_sock, &lb_events);

	int cl_events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
	srt_epoll_add_usock(epoll_id, cl_sock, &cl_events);

	SRTSOCKET obs_sock = SRT_INVALID_SOCK;

	while (running && p->active) {
		SRTSOCKET ready[2];
		int nready = 2;
		int wait_result =
			srt_epoll_wait(epoll_id, ready, &nready, nullptr,
				       nullptr, 1000, nullptr, nullptr,
				       nullptr, nullptr);

		if (wait_result > 0 && nready > 0) {
			for (int i = 0; i < nready; ++i) {
				if (ready[i] == lb_sock) {
					SRTSOCKET lb =
						p->loopback_listener.load();
					if (lb != SRT_INVALID_SOCK) {
						sockaddr_in obs_addr;
						int addr_len =
							sizeof(obs_addr);
						obs_sock = srt_accept(
							lb,
							(struct sockaddr *)
								&obs_addr,
							&addr_len);
					}
					goto done;
				}
				if (ready[i] == cl_sock) {
					obs_log(LOG_INFO,
						"Client disconnected while waiting for OBS reconnection for %s",
						p->name.c_str());
					goto done;
				}
			}
			continue;
		}

		if (wait_result < 0) {
			obs_log(LOG_WARNING,
				"srt_epoll_wait error in wait_for_obs_connection for %s: %s",
				p->name.c_str(), srt_getlasterror_str());
			break;
		}
	}

done:
	srt_epoll_release(epoll_id);
	return obs_sock;
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

	if (p->loopback_port != -1) {
		release_loopback_port(p->loopback_port);
		p->loopback_port = -1;
	}

	p->active = false;

	close_srt_socket_once(p->loopback_listener);
	close_srt_socket_once(p->obs_socket);
	close_srt_socket_once(p->client_socket);

	if (p->relay_thread) {
		if (p->relay_thread->get_id() == std::this_thread::get_id()) {
			p->relay_thread->detach();
		} else if (p->relay_thread->joinable()) {
			p->relay_thread->join();
		}
	}

	if (was_in_map) {
		update_participants_list_ui();
		if (s_automatic_mode.load()) {
			remove_obs_srt_source(p->name);
		} else {
			obs_log(LOG_INFO,
				"Static mode enabled: Keeping source 'SRT - %s' on disconnect",
				p->name.c_str());
		}
	}
	obs_log(LOG_INFO, "Cleaned up participant session %s",
		p->name.c_str());
}

int SRTBroker::find_free_loopback_port()
{
	std::lock_guard<std::mutex> lock(s_ports_mutex);
	static constexpr int PORT_MIN = 10000;
	static constexpr int PORT_MAX = 10100;
	for (int port = PORT_MIN; port < PORT_MAX; ++port) {
		if (s_allocated_ports.find(port) == s_allocated_ports.end()) {
			s_allocated_ports.insert(port);
			return port;
		}
	}
	return -1;
}
