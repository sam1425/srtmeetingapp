/*
OBS SRT Meeting App Plugin
Copyright (C) 2026 Antigravity Team
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <srt/srt.h>

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <atomic>
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

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Forward declaration of OBS API helper functions
void create_obs_srt_source(const std::string& name, int port);
void remove_obs_srt_source(const std::string& name);

// Simple struct to track dynamic participant streams
struct Participant {
    std::string name;
    SRTSOCKET client_socket;
    SRTSOCKET obs_socket;
    int loopback_port;
    SRTSOCKET loopback_listener;
    std::unique_ptr<std::thread> relay_thread;
    std::atomic<bool> active{true};
};

class SRTBroker {
public:
    SRTBroker() : running(false), listener_socket(SRT_INVALID_SOCK) {}
    ~SRTBroker() { stop(); }

    bool start(int port) {
        if (running) return true;

        if (srt_startup() != 0) {
            obs_log(LOG_ERROR, "Failed to startup SRT library");
            return false;
        }

        listener_socket = srt_create_socket();
        if (listener_socket == SRT_INVALID_SOCK) {
            obs_log(LOG_ERROR, "Failed to create SRT socket: %s", srt_getlasterror_str());
            return false;
        }

        // Enable reuse address
        int yes = 1;
        srt_setsockopt(listener_socket, 0, SRTO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = INADDR_ANY;

        if (srt_bind(listener_socket, (struct sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
            obs_log(LOG_ERROR, "Failed to bind SRT socket to port %d: %s", port, srt_getlasterror_str());
            srt_close(listener_socket);
            return false;
        }

        if (srt_listen(listener_socket, 5) == SRT_ERROR) {
            obs_log(LOG_ERROR, "Failed to listen on SRT socket: %s", srt_getlasterror_str());
            srt_close(listener_socket);
            return false;
        }

        running = true;
        listener_thread = std::make_unique<std::thread>(&SRTBroker::listen_loop, this);
        obs_log(LOG_INFO, "SRT Broker started listening on port %d", port);
        return true;
    }

    void stop() {
        if (!running) return;
        running = false;

        if (listener_socket != SRT_INVALID_SOCK) {
            srt_close(listener_socket);
            listener_socket = SRT_INVALID_SOCK;
        }

        if (listener_thread && listener_thread->joinable()) {
            listener_thread->join();
            listener_thread.reset();
        }

        // Clean up all active participants
        std::vector<std::string> participant_names;
        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            for (auto& pair : participants) {
                participant_names.push_back(pair.first);
            }
        }

        for (const auto& name : participant_names) {
            cleanup_participant(name);
        }

        srt_cleanup();
        obs_log(LOG_INFO, "SRT Broker stopped");
    }

private:
    void listen_loop() {
        while (running) {
            sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            SRTSOCKET client_sock = srt_accept(listener_socket, (struct sockaddr*)&client_addr, &addr_len);
            if (client_sock == SRT_INVALID_SOCK) {
                if (running) {
                    obs_log(LOG_WARNING, "Accept failed: %s", srt_getlasterror_str());
                }
                continue;
            }

            // Read stream ID to identify participant
            char stream_id[512] = {0};
            int opt_len = sizeof(stream_id) - 1;
            if (srt_getsockopt(client_sock, 0, SRTO_STREAMID, stream_id, &opt_len) == SRT_ERROR) {
                obs_log(LOG_WARNING, "Failed to get stream ID: %s", srt_getlasterror_str());
                srt_close(client_sock);
                continue;
            }

            std::string stream_id_str(stream_id);
            obs_log(LOG_INFO, "Incoming connection with StreamID: %s", stream_id);

            // Parse StreamID. Expected format: publish:<username> (MediaMTX way)
            const std::string prefix = "publish:";
            if (stream_id_str.rfind(prefix, 0) == 0) {
                std::string username = stream_id_str.substr(prefix.length());
                
                // Sanitize username (alphanumeric and underscores only)
                for (char& c : username) {
                    if (!isalnum(static_cast<unsigned char>(c)) && c != '_') {
                        c = '_';
                    }
                }

                if (username.empty()) {
                    obs_log(LOG_WARNING, "Empty username, rejecting stream");
                    srt_close(client_sock);
                    continue;
                }

                // Launch handling on a detached thread to not block incoming connections
                std::thread(&SRTBroker::handle_new_publisher, this, username, client_sock).detach();
            } else {
                obs_log(LOG_WARNING, "Invalid StreamID. Expected 'publish:<name>', got '%s'", stream_id);
                srt_close(client_sock);
            }
        }
    }

    void handle_new_publisher(std::string username, SRTSOCKET client_sock) {
        // Find a dynamic loopback port
        int loopback_port = find_free_loopback_port();
        if (loopback_port < 0) {
            obs_log(LOG_ERROR, "No free loopback ports available for participant %s", username.c_str());
            srt_close(client_sock);
            return;
        }

        // Set up the local loopback listener
        SRTSOCKET loopback_listener = srt_create_socket();
        if (loopback_listener == SRT_INVALID_SOCK) {
            obs_log(LOG_ERROR, "Failed to create loopback listener socket: %s", srt_getlasterror_str());
            srt_close(client_sock);
            return;
        }

        int yes = 1;
        srt_setsockopt(loopback_listener, 0, SRTO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(loopback_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

        if (srt_bind(loopback_listener, (struct sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
            obs_log(LOG_ERROR, "Failed to bind loopback listener to port %d: %s", loopback_port, srt_getlasterror_str());
            srt_close(loopback_listener);
            srt_close(client_sock);
            return;
        }

        if (srt_listen(loopback_listener, 1) == SRT_ERROR) {
            obs_log(LOG_ERROR, "Failed to listen on loopback port %d: %s", loopback_port, srt_getlasterror_str());
            srt_close(loopback_listener);
            srt_close(client_sock);
            return;
        }

        // Register participant
        auto participant = std::make_shared<Participant>();
        participant->name = username;
        participant->client_socket = client_sock;
        participant->loopback_port = loopback_port;
        participant->loopback_listener = loopback_listener;
        participant->obs_socket = SRT_INVALID_SOCK;

        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            // If participant already exists, clean them up first
            auto it = participants.find(username);
            if (it != participants.end()) {
                obs_log(LOG_INFO, "Participant %s reconnected. Cleaning up old session.", username.c_str());
                it->second->active = false;
                srt_close(it->second->client_socket);
                srt_close(it->second->obs_socket);
                srt_close(it->second->loopback_listener);
                participants.erase(it);
            }
            participants[username] = participant;
        }

        obs_log(LOG_INFO, "Registered participant: %s, assigned local loopback port %d", username.c_str(), loopback_port);

        // Tell OBS to create the source pointing to our loopback listener
        create_obs_srt_source(username, loopback_port);

        // Accept connection from OBS Media Source
        sockaddr_in obs_addr;
        int addr_len = sizeof(obs_addr);
        
        // Wait for OBS to connect (with a 5s timeout)
        int timeout_ms = 5000;
        srt_setsockopt(loopback_listener, 0, SRTO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        
        SRTSOCKET obs_sock = srt_accept(loopback_listener, (struct sockaddr*)&obs_addr, &addr_len);
        if (obs_sock == SRT_INVALID_SOCK) {
            obs_log(LOG_ERROR, "OBS failed to connect to loopback listener on port %d: %s", loopback_port, srt_getlasterror_str());
            cleanup_participant(username);
            return;
        }

        participant->obs_socket = obs_sock;

        // Start relay thread
        participant->relay_thread = std::make_unique<std::thread>(&SRTBroker::relay_loop, this, username);
    }

    void relay_loop(std::string username) {
        std::shared_ptr<Participant> p;
        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            auto it = participants.find(username);
            if (it == participants.end()) return;
            p = it->second;
        }

        char buffer[1316]; // SRT standard packet size (MPEG-TS payloads fit in 1316 bytes)
        obs_log(LOG_INFO, "Starting SRT relay loop for %s", username.c_str());

        // Set non-blocking/recv timeout to check for active state periodically
        int timeout_ms = 500;
        srt_setsockopt(p->client_socket, 0, SRTO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

        while (running && p->active) {
            // Read from client
            int bytes_read = srt_recvmsg(p->client_socket, buffer, sizeof(buffer));
            if (bytes_read == SRT_ERROR) {
                int err = srt_getlasterror(NULL);
                if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) { // Timeout, loop and check active state
                    continue;
                }
                obs_log(LOG_INFO, "Client socket read error for %s: %s", username.c_str(), srt_getlasterror_str());
                break;
            }

            if (bytes_read == 0) {
                obs_log(LOG_INFO, "Client disconnected (EOF) for %s", username.c_str());
                break;
            }

            // Write to OBS loopback
            int bytes_written = srt_sendmsg(p->obs_socket, buffer, bytes_read, -1, 0);
            if (bytes_written == SRT_ERROR) {
                obs_log(LOG_WARNING, "Failed to write to OBS loopback for %s: %s", username.c_str(), srt_getlasterror_str());
                break;
            }
        }

        obs_log(LOG_INFO, "Stopping SRT relay loop for %s", username.c_str());
        cleanup_participant(username);
    }

    void cleanup_participant(const std::string& name) {
        std::shared_ptr<Participant> p;
        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            auto it = participants.find(name);
            if (it == participants.end()) return;
            p = it->second;
            p->active = false;
            participants.erase(it);
        }

        // Close sockets
        if (p->client_socket != SRT_INVALID_SOCK) {
            srt_close(p->client_socket);
        }
        if (p->obs_socket != SRT_INVALID_SOCK) {
            srt_close(p->obs_socket);
        }
        if (p->loopback_listener != SRT_INVALID_SOCK) {
            srt_close(p->loopback_listener);
        }

        // Join or detach the relay thread
        if (p->relay_thread) {
            if (p->relay_thread->get_id() == std::this_thread::get_id()) {
                p->relay_thread->detach();
            } else if (p->relay_thread->joinable()) {
                p->relay_thread->join();
            }
        }

        // Remove source from OBS
        remove_obs_srt_source(name);
        obs_log(LOG_INFO, "Cleaned up participant %s", name.c_str());
    }

    int find_free_loopback_port() {
        static std::atomic<int> next_port(10000);
        for (int i = 0; i < 100; ++i) {
            int port = next_port++;
            if (next_port >= 10100) next_port = 10000;

            // Check if already in use
            bool in_use = false;
            {
                std::lock_guard<std::mutex> lock(participants_mutex);
                for (const auto& pair : participants) {
                    if (pair.second->loopback_port == port) {
                        in_use = true;
                        break;
                    }
                }
            }
            if (!in_use) return port;
        }
        return -1;
    }

    std::atomic<bool> running;
    SRTSOCKET listener_socket;
    std::unique_ptr<std::thread> listener_thread;

    std::mutex participants_mutex;
    std::map<std::string, std::shared_ptr<Participant>> participants;
};

// Global broker instance
static SRTBroker s_broker;

bool obs_module_load(void)
{
    obs_log(LOG_INFO, "SRT Meeting plugin loaded successfully (version %s)", PLUGIN_VERSION);
    
    // Start SRT broker on port 9000
    if (!s_broker.start(9000)) {
        obs_log(LOG_ERROR, "Failed to start SRT broker");
        return false;
    }
    
    return true;
}

void obs_module_unload(void)
{
    s_broker.stop();
    obs_log(LOG_INFO, "SRT Meeting plugin unloaded");
}

// Struct to pass to OBS UI queue tasks
struct TaskData {
    std::string name;
    int port;
};

void create_obs_srt_source_task(void *param) {
    auto* data = static_cast<TaskData*>(param);
    
    // 1. Create Media Source settings
    obs_data_t *settings = obs_data_create();
    
    // Construct the loopback SRT URL
    // Low latency is configured via latency parameter (value in microseconds)
    std::string srt_url = "srt://127.0.0.1:" + std::to_string(data->port) + "?mode=caller&latency=120000";
    obs_data_set_string(settings, "input", srt_url.c_str());
    obs_data_set_bool(settings, "is_local_file", false);
    obs_data_set_string(settings, "input_format", "mpegts");
    obs_data_set_bool(settings, "looping", false);
    obs_data_set_bool(settings, "hw_decode", true);
    obs_data_set_int(settings, "buffering_mb", 1);
    
    // 2. Get current scene
    obs_source_t *current_scene_source = obs_frontend_get_current_scene();
    if (current_scene_source) {
        obs_scene_t *scene = obs_scene_from_source(current_scene_source);
        if (scene) {
            std::string source_name = "SRT - " + data->name;
            
            // Check if source already exists
            obs_source_t *existing_source = obs_get_source_by_name(source_name.c_str());
            if (existing_source) {
                obs_log(LOG_INFO, "Source '%s' already exists in OBS, skipping creation", source_name.c_str());
                obs_source_release(existing_source);
            } else {
                // Create native Media Source
                obs_source_t *srt_source = obs_source_create("ffmpeg_source", source_name.c_str(), settings, nullptr);
                if (srt_source) {
                    obs_scene_add(scene, srt_source);
                    obs_source_release(srt_source);
                    obs_log(LOG_INFO, "Added SRT source '%s' to current scene", source_name.c_str());
                } else {
                    obs_log(LOG_ERROR, "Failed to create OBS source '%s'", source_name.c_str());
                }
            }
        }
        obs_source_release(current_scene_source);
    } else {
        obs_log(LOG_WARNING, "No active scene found to add source");
    }
    
    obs_data_release(settings);
    delete data;
}

void remove_obs_srt_source_task(void *param) {
    auto* name = static_cast<std::string*>(param);
    std::string source_name = "SRT - " + *name;
    
    obs_source_t *source = obs_get_source_by_name(source_name.c_str());
    if (source) {
        obs_source_remove(source);
        obs_source_release(source);
        obs_log(LOG_INFO, "Removed SRT source '%s' from OBS", source_name.c_str());
    } else {
        obs_log(LOG_WARNING, "Source '%s' not found for removal", source_name.c_str());
    }
    
    delete name;
}

void create_obs_srt_source(const std::string& name, int port) {
    auto* data = new TaskData{name, port};
    obs_queue_task(OBS_TASK_UI, create_obs_srt_source_task, data, false);
}

void remove_obs_srt_source(const std::string& name) {
    auto* data = new std::string(name);
    obs_queue_task(OBS_TASK_UI, remove_obs_srt_source_task, data, false);
}
