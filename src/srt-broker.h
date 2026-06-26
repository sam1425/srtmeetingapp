#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <srt/srt.h>

/* ---------------------------------------------------------------------------
 * PortPool – thread-safe allocator for relay ports.
 * -----------------------------------------------------------------------*/
class PortPool {
public:
	PortPool(int base_port, int count);

	int allocate();
	void release(int port);

private:
	mutable std::mutex mutex_;
	std::set<int> available_;
};

/* ---------------------------------------------------------------------------
 * Participant – tracks one connected SRT publisher and its relay socket.
 * -----------------------------------------------------------------------*/
struct Participant {
	std::string name;
	std::atomic<SRTSOCKET> client_socket{SRT_INVALID_SOCK};
	std::atomic<SRTSOCKET> relay_listener{SRT_INVALID_SOCK};
	std::atomic<SRTSOCKET> obs_socket{SRT_INVALID_SOCK};
	
	int relay_port{-1};
	int latency_ms{0};
	
	std::unique_ptr<std::thread> relay_thread;
	std::atomic<bool> active{true};
	
	std::atomic<int> epoll_id{-1};
};

/* ---------------------------------------------------------------------------
 * SRTBroker – listens on a single UDP port, demuxes incoming publishers by
 *             their SRT StreamID, and relays each stream to a unique local
 *             port where OBS can consume it natively.
 * -----------------------------------------------------------------------*/
class SRTBroker {
public:
	SRTBroker();
	~SRTBroker();

	bool start(int port);
	void stop();

	std::vector<std::string> get_active_participant_names();

private:
	void listen_loop();
	void handle_new_publisher(std::string username, SRTSOCKET client_sock, int latency_ms);
	void relay_loop(std::shared_ptr<Participant> p);
	void cleanup_participant(std::shared_ptr<Participant> p);

	std::atomic<bool> running_{false};
	std::atomic<SRTSOCKET> listener_socket_{SRT_INVALID_SOCK};
	std::unique_ptr<std::thread> listener_thread_;
	std::atomic<int> listener_epoll_id_{-1};

	std::mutex participants_mutex_;
	std::map<std::string, std::shared_ptr<Participant>> participants_;

	PortPool port_pool_;
};

extern SRTBroker s_broker;
