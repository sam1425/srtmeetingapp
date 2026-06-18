#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <srt/srt.h>

#include <thread>
#include <mutex>
#include <future>
#include <vector>
#include <string>
#include <map>
#include <set>
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

// Qt Headers
#include <QMainWindow>
#include <QDockWidget>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QListWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Forward declaration of OBS API helper functions
void create_obs_srt_source(const std::string& name, int port);
void remove_obs_srt_source(const std::string& name);
void update_participants_list_ui();

// Static UI Globals
static std::atomic<bool> s_automatic_mode{true};
static QListWidget *s_participants_list{nullptr};
static QDockWidget *s_dock_widget{nullptr};

// The UDP port the SRT broker listens on
static constexpr int BROKER_PORT = 9000;

// Static Port Allocation Globals
static std::mutex s_ports_mutex;
static std::set<int> s_allocated_ports;

static void release_loopback_port(int port) {
    std::lock_guard<std::mutex> lock(s_ports_mutex);
    s_allocated_ports.erase(port);
}

// Simple struct to track dynamic participant streams
struct Participant {
    std::string name;
    std::atomic<SRTSOCKET> client_socket{SRT_INVALID_SOCK};
    std::atomic<SRTSOCKET> obs_socket{SRT_INVALID_SOCK};
    int loopback_port{-1};
    std::atomic<SRTSOCKET> loopback_listener{SRT_INVALID_SOCK};
    std::unique_ptr<std::thread> relay_thread;
    std::atomic<bool> active{true};
};

// Atomically close an SRT socket exactly once, preventing double-close races
static void close_srt_socket_once(std::atomic<SRTSOCKET> &sock) {
    SRTSOCKET s = sock.exchange(SRT_INVALID_SOCK);
    if (s != SRT_INVALID_SOCK) {
        srt_close(s);
    }
}

class SRTBroker {
public:
    SRTBroker() : running(false), listener_socket(SRT_INVALID_SOCK) {}
    ~SRTBroker() { stop(); }

    std::vector<std::string> get_active_participant_names() {
        std::lock_guard<std::mutex> lock(participants_mutex);
        std::vector<std::string> names;
        for (const auto& pair : participants) {
            names.push_back(pair.first);
        }
        return names;
    }

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

        // Bug fix: listener_socket is now atomic — use close_srt_socket_once
        // so stop() and listen_loop() can never double-close it.
        close_srt_socket_once(listener_socket);

        if (listener_thread && listener_thread->joinable()) {
            listener_thread->join();
            listener_thread.reset();
        }

        // Wait for all handle_new_publisher tasks to complete.
        // Bug fix: replaced std::thread vector with std::future vector because
        // a finished std::thread is still joinable() — the old pruning logic
        // (if (!t.joinable()) erase) never removed anything, growing forever.
        {
            std::lock_guard<std::mutex> lock(handler_futures_mutex);
            for (auto &f : handler_futures) {
                f.wait();
            }
            handler_futures.clear();
        }

        // Clean up all active participants
        std::vector<std::shared_ptr<Participant>> active_list;
        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            for (auto& pair : participants) {
                active_list.push_back(pair.second);
            }
            participants.clear();
        }

        for (auto& p : active_list) {
            cleanup_participant(p);
        }

        srt_cleanup();
        obs_log(LOG_INFO, "SRT Broker stopped");
    }

private:
    void listen_loop() {
      try {
        while (running) {
            sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
            // Bug fix: listener_socket is atomic — load before passing to srt_accept
            // so stop()'s close_srt_socket_once() never races a live read.
            SRTSOCKET ls = listener_socket.load();
            if (ls == SRT_INVALID_SOCK) break;
            SRTSOCKET client_sock = srt_accept(ls, (struct sockaddr*)&client_addr, &addr_len);
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
                
                // Strip query parameters if present (e.g., ?latency=120000)
                size_t query_pos = username.find_first_of("?;");
                if (query_pos != std::string::npos) {
                    username = username.substr(0, query_pos);
                }

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

                // Launch handling without blocking the accept loop.
                // Bug fix: use std::future instead of std::thread — a finished
                // std::thread is still joinable(), so the old pruning condition
                // `!it->joinable()` was always false and never removed anything.
                // std::future::wait_for(0s) returns ready only when truly done.
                {
                    std::lock_guard<std::mutex> lock(handler_futures_mutex);
                    // Prune completed tasks (non-blocking zero-wait check)
                    for (auto it = handler_futures.begin(); it != handler_futures.end(); ) {
                        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                            it = handler_futures.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    handler_futures.push_back(std::async(std::launch::async,
                        &SRTBroker::handle_new_publisher, this, username, client_sock));
                }
            } else {
                obs_log(LOG_WARNING, "Invalid StreamID. Expected 'publish:<name>', got '%s'", stream_id);
                srt_close(client_sock);
            }
        }
      } catch (const std::exception &e) {
          obs_log(LOG_ERROR, "Exception in listen_loop: %s", e.what());
      } catch (...) {
          obs_log(LOG_ERROR, "Unknown exception in listen_loop");
      }
    }

    void handle_new_publisher(std::string username, SRTSOCKET client_sock) {
      try {
        std::shared_ptr<Participant> old_p;
        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            auto it = participants.find(username);
            if (it != participants.end()) {
                old_p = it->second;
                participants.erase(it); // Erase old one from map so cleanup_participant knows it was replaced
            }
        }

        if (old_p) {
            obs_log(LOG_INFO, "Participant %s reconnected. Cleaning up old session.", username.c_str());
            old_p->active = false;

            // Close sockets so the old relay_loop unblocks and exits
            close_srt_socket_once(old_p->client_socket);
            close_srt_socket_once(old_p->obs_socket);
            close_srt_socket_once(old_p->loopback_listener);

            if (old_p->relay_thread && old_p->relay_thread->joinable()) {
                old_p->relay_thread->join();
            }
        }

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
            release_loopback_port(loopback_port);
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
            release_loopback_port(loopback_port);
            return;
        }

        if (srt_listen(loopback_listener, 1) == SRT_ERROR) {
            obs_log(LOG_ERROR, "Failed to listen on loopback port %d: %s", loopback_port, srt_getlasterror_str());
            srt_close(loopback_listener);
            srt_close(client_sock);
            release_loopback_port(loopback_port);
            return;
        }

        // Register participant
        auto participant = std::make_shared<Participant>();
        participant->name = username;
        participant->client_socket.store(client_sock);
        participant->loopback_port = loopback_port;
        participant->loopback_listener.store(loopback_listener);
        // obs_socket is already SRT_INVALID_SOCK from default initialization

        {
            std::lock_guard<std::mutex> lock(participants_mutex);
            participants[username] = participant;
        }

        obs_log(LOG_INFO, "Registered participant: %s, assigned local loopback port %d", username.c_str(), loopback_port);
        update_participants_list_ui();

        // Tell OBS to create the source pointing to our loopback listener
        create_obs_srt_source(username, loopback_port);

        // Accept connection from OBS Media Source
        sockaddr_in obs_addr;
        int addr_len = sizeof(obs_addr);

        // Bug fix: SRTO_RCVTIMEO only applies to srt_recv*, NOT srt_accept().
        // The old code set a receive timeout that had zero effect on the accept
        // call, so if OBS never connected this thread would block forever and
        // stop() would deadlock trying to join it.
        // Fix: use srt_epoll_wait which correctly timeouts the accept readiness.
        SRTSOCKET obs_sock = SRT_INVALID_SOCK;
        SRTSOCKET lb_sock = participant->loopback_listener.load();
        if (lb_sock != SRT_INVALID_SOCK) {
            int epoll_id = srt_epoll_create();
            if (epoll_id < 0) {
                obs_log(LOG_ERROR, "srt_epoll_create failed: %s", srt_getlasterror_str());
            } else {
                int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
                srt_epoll_add_usock(epoll_id, lb_sock, &events);

                SRTSOCKET ready[1];
                int nready = 1;
                // Wait up to 10 s for OBS media source to connect
                int wait_result = srt_epoll_wait(epoll_id,
                    ready, &nready,          // read-ready set
                    nullptr, nullptr,        // write-ready set (unused)
                    10000,                   // timeout ms
                    nullptr, nullptr,        // system sockets (unused)
                    nullptr, nullptr);
                srt_epoll_release(epoll_id);

                if (wait_result > 0 && nready > 0) {
                    lb_sock = participant->loopback_listener.load();
                    if (lb_sock != SRT_INVALID_SOCK) {
                        obs_sock = srt_accept(lb_sock, (struct sockaddr*)&obs_addr, &addr_len);
                    }
                } else {
                    obs_log(LOG_ERROR,
                        "Timed out waiting 10s for OBS to connect on loopback port %d",
                        loopback_port);
                }
            }
        }
        if (obs_sock == SRT_INVALID_SOCK) {
            obs_log(LOG_ERROR, "OBS failed to connect to loopback listener on port %d: %s",
                loopback_port, srt_getlasterror_str());
            cleanup_participant(participant);
            return;
        }

        participant->obs_socket.store(obs_sock);

        // Start relay thread
        participant->relay_thread = std::make_unique<std::thread>(&SRTBroker::relay_loop, this, participant);
      } catch (const std::exception &e) {
          obs_log(LOG_ERROR, "Exception in handle_new_publisher: %s", e.what());
          srt_close(client_sock);
      } catch (...) {
          obs_log(LOG_ERROR, "Unknown exception in handle_new_publisher");
          srt_close(client_sock);
      }
    }

    void relay_loop(std::shared_ptr<Participant> p) {
      try {
        char buffer[1316]; // SRT standard packet size (MPEG-TS payloads fit in 1316 bytes)
        obs_log(LOG_INFO, "Starting SRT relay loop for %s", p->name.c_str());

        // Set non-blocking/recv timeout to check for active state periodically
        int timeout_ms = 500;
        SRTSOCKET cs = p->client_socket.load();
        if (cs != SRT_INVALID_SOCK) {
            srt_setsockopt(cs, 0, SRTO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        }

        while (running && p->active) {
            // Validate sockets before SRT operations
            SRTSOCKET recv_sock = p->client_socket.load();
            SRTSOCKET send_sock = p->obs_socket.load();
            if (recv_sock == SRT_INVALID_SOCK || send_sock == SRT_INVALID_SOCK) {
                obs_log(LOG_INFO, "Socket invalidated for %s, exiting relay loop", p->name.c_str());
                break;
            }

            // Read from client
            int bytes_read = srt_recvmsg(recv_sock, buffer, sizeof(buffer));
            if (bytes_read == SRT_ERROR) {
                int err = srt_getlasterror(NULL);
                if (err == SRT_EASYNCRCV || err == SRT_ETIMEOUT) { // Timeout, loop and check active state
                    continue;
                }
                obs_log(LOG_INFO, "Client socket read error for %s: %s", p->name.c_str(), srt_getlasterror_str());
                break;
            }

            if (bytes_read == 0) {
                obs_log(LOG_INFO, "Client disconnected (EOF) for %s", p->name.c_str());
                break;
            }

            // Write to OBS loopback
            send_sock = p->obs_socket.load();
            if (send_sock == SRT_INVALID_SOCK) {
                obs_log(LOG_INFO, "OBS socket invalidated for %s, exiting relay loop", p->name.c_str());
                break;
            }
            int bytes_written = srt_sendmsg(send_sock, buffer, bytes_read, -1, 0);
            if (bytes_written == SRT_ERROR) {
                obs_log(LOG_WARNING, "Failed to write to OBS loopback for %s: %s", p->name.c_str(), srt_getlasterror_str());
                break;
            }
        }

        obs_log(LOG_INFO, "Stopping SRT relay loop for %s", p->name.c_str());
        cleanup_participant(p);
      } catch (const std::exception &e) {
          obs_log(LOG_ERROR, "Exception in relay_loop for %s: %s", p->name.c_str(), e.what());
          cleanup_participant(p);
      } catch (...) {
          obs_log(LOG_ERROR, "Unknown exception in relay_loop for %s", p->name.c_str());
          cleanup_participant(p);
      }
    }

    void cleanup_participant(std::shared_ptr<Participant> p) {
        if (!p) return;

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

        // Close sockets (atomically, prevents double-close).
        // Close loopback_listener FIRST so OBS Media Source can't reconnect
        // when obs_socket drops (which would cause a black frame moment).
        close_srt_socket_once(p->loopback_listener);
        close_srt_socket_once(p->obs_socket);
        close_srt_socket_once(p->client_socket);

        // Join or detach the relay thread
        if (p->relay_thread) {
            if (p->relay_thread->get_id() == std::this_thread::get_id()) {
                p->relay_thread->detach();
            } else if (p->relay_thread->joinable()) {
                p->relay_thread->join();
            }
        }

        if (was_in_map) {
            update_participants_list_ui();
            // Remove source from OBS only if automatic mode is enabled
            if (s_automatic_mode.load()) {
                remove_obs_srt_source(p->name);
            } else {
                obs_log(LOG_INFO, "Static mode enabled: Keeping source 'SRT - %s' on disconnect", p->name.c_str());
            }
        }
        obs_log(LOG_INFO, "Cleaned up participant session %s", p->name.c_str());
    }

    int find_free_loopback_port() {
        std::lock_guard<std::mutex> lock(s_ports_mutex);
        static constexpr int PORT_MIN = 10000;
        static constexpr int PORT_MAX = 10100;
        // Scan the full range every call so freed ports are immediately reusable
        for (int port = PORT_MIN; port < PORT_MAX; ++port) {
            if (s_allocated_ports.find(port) == s_allocated_ports.end()) {
                s_allocated_ports.insert(port);
                return port;
            }
        }
        return -1;
    }

    std::atomic<bool> running;
    // Bug fix: was plain SRTSOCKET (int) — written from stop() and read from
    // listen_loop() on different threads simultaneously (data race / UB).
    std::atomic<SRTSOCKET> listener_socket;
    std::unique_ptr<std::thread> listener_thread;

    std::mutex handler_futures_mutex;
    std::vector<std::future<void>> handler_futures;

    std::mutex participants_mutex;
    std::map<std::string, std::shared_ptr<Participant>> participants;
};

// Global broker instance
static SRTBroker s_broker;

static void update_participants_list_ui_task(void *param) {
    if (!s_participants_list) return;
    s_participants_list->clear();
    std::vector<std::string> names = s_broker.get_active_participant_names();
    for (const auto& name : names) {
        s_participants_list->addItem(QString::fromStdString(name));
    }
    (void)param;
}

void update_participants_list_ui() {
    obs_queue_task(OBS_TASK_UI, update_participants_list_ui_task, nullptr, false);
}

static void setup_plugin_dock(int broker_port) {
    QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main_window) {
        obs_log(LOG_WARNING, "OBS main window not found, cannot create dock");
        return;
    }

    s_dock_widget = new QDockWidget("SRT Meeting Control", main_window);
    s_dock_widget->setObjectName("srtMeetingControlDock");

    QWidget *content = new QWidget(s_dock_widget);
    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    // Section 1: Mode Selection
    QLabel *mode_label = new QLabel("<b>Orchestration Mode</b>", content);
    mode_label->setStyleSheet("font-size: 13px; color: #bac2de;");
    layout->addWidget(mode_label);

    QRadioButton *auto_btn = new QRadioButton("Automatic Mode (Dynamic Sources)", content);
    auto_btn->setToolTip("Automatically creates OBS source when client connects, and deletes it when they disconnect.");
    auto_btn->setChecked(s_automatic_mode.load());
    layout->addWidget(auto_btn);

    QRadioButton *static_btn = new QRadioButton("Static Mode (Persistent Sources)", content);
    static_btn->setToolTip("Creates OBS source on first connection, but keeps it when they disconnect. Updates source settings if they reconnect.");
    static_btn->setChecked(!s_automatic_mode.load());
    layout->addWidget(static_btn);

    QButtonGroup *group = new QButtonGroup(content);
    group->addButton(auto_btn, 0);
    group->addButton(static_btn, 1);

    QObject::connect(group, &QButtonGroup::idClicked, [](int id) {
        s_automatic_mode.store(id == 0);
        obs_log(LOG_INFO, "Connection Mode changed to: %s", (id == 0) ? "Automatic" : "Static");
    });

    // Spacer
    layout->addSpacing(8);

    // Section 2: Active Participants
    QLabel *list_label = new QLabel("<b>Active Participants</b>", content);
    list_label->setStyleSheet("font-size: 13px; color: #bac2de;");
    layout->addWidget(list_label);

    s_participants_list = new QListWidget(content);
    s_participants_list->setToolTip("List of currently streaming participants.");
    s_participants_list->setStyleSheet(
        "background-color: rgb(30, 30, 46);"
        "color: rgb(166, 227, 161);"
        "border: 1px solid rgb(49, 50, 68);"
        "border-radius: 6px;"
        "padding: 5px;"
    );
    layout->addWidget(s_participants_list);

    // Section 3: Connection Info Helper
    QString portStr = QString::number(broker_port);
    QLabel *info_label = new QLabel("SRT Broker Listening on UDP port <b>" + portStr + "</b>", content);
    info_label->setAlignment(Qt::AlignCenter);
    info_label->setStyleSheet("color: rgb(180, 190, 254); font-size: 11px; margin-top: 4px;");
    layout->addWidget(info_label);

    content->setLayout(layout);
    s_dock_widget->setWidget(content);

    obs_frontend_add_custom_qdock("srtMeetingControlDock", s_dock_widget);
    // Note: obs_frontend_add_custom_qdock already adds the dock to the main
    // window internally; calling addDockWidget a second time would double-add it.
    s_dock_widget->show();
    obs_log(LOG_INFO, "SRT Meeting UI dock registered successfully");
}

static void obssrt_frontend_event(enum obs_frontend_event event, void *private_data) {
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        setup_plugin_dock(BROKER_PORT);
    }
    (void)private_data;
}

bool obs_module_load(void)
{
    obs_log(LOG_INFO, "SRT Meeting plugin loaded successfully (version %s)", PLUGIN_VERSION);
    
    // Register frontend event callback to create dock once UI is loaded
    obs_frontend_add_event_callback(obssrt_frontend_event, nullptr);

    // Start SRT broker on the configured port
    if (!s_broker.start(BROKER_PORT)) {
        obs_log(LOG_ERROR, "Failed to start SRT broker");
        return false;
    }
    
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(obssrt_frontend_event, nullptr);

    // Stop broker first so no participant UI updates happen after dock is removed
    s_broker.stop();

    if (s_dock_widget) {
        obs_frontend_remove_dock("srtMeetingControlDock");
        delete s_dock_widget;
        s_dock_widget = nullptr;
    }
    s_participants_list = nullptr;

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
                obs_log(LOG_INFO, "Source '%s' already exists in OBS, updating settings with new port %d", source_name.c_str(), data->port);
                obs_source_update(existing_source, settings);
                obs_source_release(existing_source);
            } else {
                // Create native Media Source
                obs_source_t *srt_source = obs_source_create("ffmpeg_source", source_name.c_str(), settings, nullptr);
                if (srt_source) {
                    obs_sceneitem_t *scene_item = obs_scene_add(scene, srt_source);
                    if (scene_item) {
                        obs_log(LOG_INFO, "Added SRT source '%s' to current scene", source_name.c_str());
                    } else {
                        obs_log(LOG_ERROR, "Failed to add SRT source '%s' to scene", source_name.c_str());
                    }
                    obs_source_release(srt_source);
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
