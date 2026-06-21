#pragma once

#include <atomic>
#include <map>
#include <string>

#include <obs-module.h>

/* ---------------------------------------------------------------------------
 * SceneManager – manages OBS scenes and participant sources.
 *
 * When a participant connects, the broker calls add_participant() which
 * queues an OBS task to create an ffmpeg_source with an SRT URL pointing
 * at the relay port.  When they disconnect, remove_participant() tears it
 * down.  All OBS calls are queued to the UI thread for thread safety.
 * -----------------------------------------------------------------------*/
class SceneManager {
public:
	SceneManager();
	~SceneManager();

	void init(const std::string &target_scene);
	void shutdown();

	void add_participant(const std::string &name, int port);
	void remove_participant(const std::string &name);

	void set_target_scene(const std::string &name);
	std::string get_target_scene() const;

	void set_automatic_mode(bool automatic);
	bool is_automatic_mode() const;

	void set_default_latency_ms(int ms);
	int get_default_latency_ms() const;

	obs_source_t *get_scene_source() const;
	obs_scene_t *get_scene() const;

private:
	std::string target_scene_name_;
	std::atomic<bool> automatic_mode_{true};
	std::atomic<int> default_latency_ms_{120};

	std::map<std::string, std::string> participant_sources_;
	obs_source_t *scene_source_{nullptr};
};

extern SceneManager s_scene_manager;
