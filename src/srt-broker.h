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
 * PacketBuffer – thread-safe ring buffer for MPEG-TS packets.
 *
 * Written by the broker's receive thread, read by the custom OBS source's
 * decode thread.  When full the oldest packets are silently dropped so a
 * slow decoder never blocks the network path.
 * -----------------------------------------------------------------------*/
class PacketBuffer {
public:
	static constexpr size_t MAX_PACKETS = 2000; // ~2.6 MB at 1316 B each

	void push(const uint8_t *data, int size);
	std::vector<uint8_t> pop(int timeout_ms = 100);
	void signal_eof();
	bool is_eof() const;
	void reset(); // clear buffer + reset EOF for reconnection

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::vector<uint8_t>> packets_;
	std::atomic<bool> eof_{false};
};

/* ---------------------------------------------------------------------------
 * Participant – one connected SRT publisher.
 * -----------------------------------------------------------------------*/
struct Participant {
	std::string name;
	std::atomic<SRTSOCKET> client_socket{SRT_INVALID_SOCK};
	std::shared_ptr<PacketBuffer> buffer;
	std::unique_ptr<std::thread> receive_thread;
	std::atomic<bool> active{true};
};

/* ---------------------------------------------------------------------------
 * SRTBroker – listens on a single port, demuxes publishers by StreamID,
 *             and pushes their MPEG-TS data into per-participant ring
 *             buffers.  No loopback sockets, no relay threads.
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
	void receive_loop(std::shared_ptr<Participant> p);
	void cleanup_participant(std::shared_ptr<Participant> p);

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
