#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <srt/srt.h>

/* ---------------------------------------------------------------------------
 * PortPool – simple allocator for relay ports (10001-10100).
 * -----------------------------------------------------------------------*/
class PortPool {
public:
	PortPool(int base, int count);

	int allocate();
	void release(int port);
	bool is_available(int port) const;

private:
	mutable std::mutex mutex_;
	std::set<int> available_;
};

/* ---------------------------------------------------------------------------
 * Participant – one connected SRT publisher.
 * -----------------------------------------------------------------------*/
struct Participant {
	std::string name;
	std::atomic<SRTSOCKET> client_socket{SRT_INVALID_SOCK};
	std::atomic<SRTSOCKET> relay_socket{SRT_INVALID_SOCK};
	int relay_port{-1};
	std::unique_ptr<std::thread> relay_thread;
	std::atomic<bool> active{true};
};

/* ---------------------------------------------------------------------------
 * SRTBroker – listens on a single port, demuxes publishers by StreamID,
 *             relays each stream to a unique local port, and notifies the
 *             SceneManager to create OBS sources.
 * -----------------------------------------------------------------------*/
class SRTBroker {
public:
	SRTBroker();
	~SRTBroker();

	std::vector<std::string> get_active_participant_names();
	bool start(int port);
	void stop();

private:
	void listen_loop();
	void handle_new_publisher(std::string username,
				  SRTSOCKET client_sock, int latency_ms);
	void relay_loop(std::shared_ptr<Participant> p);
	void cleanup_participant(std::shared_ptr<Participant> p);

	std::atomic<bool> running;
	std::atomic<SRTSOCKET> listener_socket;
	std::unique_ptr<std::thread> listener_thread;

	std::mutex handler_futures_mutex;
	std::vector<std::future<void>> handler_futures;

	std::mutex participants_mutex;
	std::map<std::string, std::shared_ptr<Participant>> participants;

	PortPool port_pool_;
};

extern SRTBroker s_broker;
