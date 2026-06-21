#include <obs-module.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "plugin-dock.h"
#include "srt-source.h"

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
std::atomic<bool> s_automatic_mode{true};
std::atomic<int> s_default_latency_ms{120};

static void close_srt_socket_once(std::atomic<SRTSOCKET> &sock)
{
	SRTSOCKET s = sock.exchange(SRT_INVALID_SOCK);
	if (s != SRT_INVALID_SOCK) {
		srt_close(s);
	}
}

/* ---------------------------------------------------------------------------
 * PacketBuffer Implementation
 * -----------------------------------------------------------------------*/

void PacketBuffer::push(const uint8_t *data, int size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (packets_.size() >= MAX_PACKETS) {
		packets_.pop_front();
	}
	packets_.push_back(std::vector<uint8_t>(data, data + size));
	cv_.notify_one();
}

std::vector<uint8_t> PacketBuffer::pop(int timeout_ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	if (packets_.empty() && !eof_) {
		cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
	}
	if (!packets_.empty()) {
		std::vector<uint8_t> pkt = std::move(packets_.front());
		packets_.pop_front();
		return pkt;
	}
	return std::vector<uint8_t>();
}

void PacketBuffer::signal_eof()
{
	eof_ = true;
	cv_.notify_all();
}

bool PacketBuffer::is_eof() const
{
	return eof_.load();
}

void PacketBuffer::reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	packets_.clear();
	eof_ = false;
}

/* ---------------------------------------------------------------------------
 * SRTBroker Implementation
 * -----------------------------------------------------------------------*/

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
			old_p->buffer->signal_eof();
			unregister_participant_buffer(username);
		}

		auto participant = std::make_shared<Participant>();
		participant->name = username;
		participant->client_socket.store(client_sock);
		participant->buffer = std::make_shared<PacketBuffer>();
		participant->buffer->reset();

		{
			std::lock_guard<std::mutex> lock(participants_mutex);
			participants[username] = participant;
		}

		obs_log(LOG_INFO, "Registered participant: %s, setting up PacketBuffer",
			username.c_str());

		register_participant_buffer(username, participant->buffer);

		// Set SRT latency
		int final_latency_ms =
			latency_ms > 0 ? latency_ms : s_default_latency_ms.load();
		srt_setsockopt(client_sock, 0, SRTO_RCVLATENCY, &final_latency_ms,
			       sizeof(final_latency_ms));

		update_participants_list_ui();

		create_obs_srt_source(username, participant->buffer);

		participant->receive_thread = std::make_unique<std::thread>(
			&SRTBroker::receive_loop, this, participant);

	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception in handle_new_publisher: %s",
			e.what());
		srt_close(client_sock);
	} catch (...) {
		obs_log(LOG_ERROR, "Unknown exception in handle_new_publisher");
		srt_close(client_sock);
	}
}

	void SRTBroker::receive_loop(std::shared_ptr<Participant> p)
{
	try {
		constexpr int RECV_BUFFER_SIZE = 1316 * 16;
		std::vector<uint8_t> buffer(RECV_BUFFER_SIZE);
		obs_log(LOG_INFO, "Starting SRT receive loop for %s (buffer=%d bytes)",
			p->name.c_str(), RECV_BUFFER_SIZE);

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
					"Client socket gone for %s, exiting receive loop",
					p->name.c_str());
				break;
			}

			int bytes_read =
				srt_recvmsg(recv_sock, reinterpret_cast<char*>(buffer.data()), buffer.size());
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

			if (bytes_read > 0) {
				p->buffer->push(buffer.data(), bytes_read);
				obs_log(LOG_DEBUG, "Pushed %d bytes to buffer for %s", bytes_read, p->name.c_str());
			}
		}

		obs_log(LOG_INFO, "Stopping SRT receive loop for %s",
			p->name.c_str());
		cleanup_participant(p);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception in receive_loop for %s: %s",
			p->name.c_str(), e.what());
		cleanup_participant(p);
	} catch (...) {
		obs_log(LOG_ERROR, "Unknown exception in receive_loop for %s",
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
	p->buffer->signal_eof();
	unregister_participant_buffer(p->name);

	if (p->receive_thread) {
		if (p->receive_thread->get_id() == std::this_thread::get_id()) {
			p->receive_thread->detach();
		} else if (p->receive_thread->joinable()) {
			p->receive_thread->join();
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
