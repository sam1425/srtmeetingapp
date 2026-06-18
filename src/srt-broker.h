#pragma once

#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <future>
#include <vector>
#include <set>

#include <srt/srt.h>

struct Participant {
	std::string name;
	std::atomic<SRTSOCKET> client_socket{SRT_INVALID_SOCK};
	std::atomic<SRTSOCKET> obs_socket{SRT_INVALID_SOCK};
	int loopback_port{-1};
	std::atomic<SRTSOCKET> loopback_listener{SRT_INVALID_SOCK};
	std::unique_ptr<std::thread> relay_thread;
	std::atomic<bool> active{true};
};

class SRTBroker {
public:
	SRTBroker();
	~SRTBroker();

	std::vector<std::string> get_active_participant_names();
	bool start(int port);
	void stop();

private:
	void listen_loop();
	void handle_new_publisher(std::string username, SRTSOCKET client_sock,
				  int latency_ms);
	void relay_loop(std::shared_ptr<Participant> p);
	void cleanup_participant(std::shared_ptr<Participant> p);
	SRTSOCKET wait_for_obs_connection(std::shared_ptr<Participant> p);
	int find_free_loopback_port();

	std::atomic<bool> running;
	std::atomic<SRTSOCKET> listener_socket;
	std::unique_ptr<std::thread> listener_thread;

	std::mutex handler_futures_mutex;
	std::vector<std::future<void>> handler_futures;

	std::mutex participants_mutex;
	std::map<std::string, std::shared_ptr<Participant>> participants;
};

extern SRTBroker s_broker;
extern std::atomic<bool> s_automatic_mode;
extern std::atomic<int> s_default_latency_ms;
