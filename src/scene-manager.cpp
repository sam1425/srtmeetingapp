#include <obs-module.h>
#include <plugin-support.h>

#include "scene-manager.h"

#include <string>

SceneManager s_scene_manager;

/* ---------------------------------------------------------------------------
 * Helper: task data for queued OBS operations.
 * -----------------------------------------------------------------------*/

struct AddParticipantData {
	std::string name;
	int port;
};

struct RemoveParticipantData {
	std::string name;
};

/* ---------------------------------------------------------------------------
 * SceneManager Implementation
 * -----------------------------------------------------------------------*/

SceneManager::SceneManager() {}

SceneManager::~SceneManager()
{
	shutdown();
}

void SceneManager::init(const std::string &target_scene)
{
	shutdown();

	target_scene_name_ = target_scene;

	obs_source_t *source =
		obs_get_source_by_name(target_scene_name_.c_str());
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		if (scene) {
			scene_source_ = source;
			obs_log(LOG_INFO, "Using existing scene '%s'",
				target_scene_name_.c_str());
			return;
		}
		obs_source_release(source);
	}

	obs_scene_t *new_scene =
		obs_scene_create(target_scene_name_.c_str());
	if (new_scene) {
		scene_source_ = obs_source_get_ref(
			obs_scene_get_source(new_scene));
		obs_log(LOG_INFO, "Created target scene '%s'",
			target_scene_name_.c_str());
		return;
	}

	const char *locale = obs_get_locale();
	std::string fallback_name = "Meeting";
	if (locale && (strncmp(locale, "es", 2) == 0))
		fallback_name = "Reunion";

	new_scene = obs_scene_create(fallback_name.c_str());
	if (new_scene) {
		target_scene_name_ = fallback_name;
		scene_source_ = obs_source_get_ref(
			obs_scene_get_source(new_scene));
		obs_log(LOG_INFO,
			"Created fallback scene '%s' (locale: %s)",
			fallback_name.c_str(),
			locale ? locale : "unknown");
		return;
	}

	obs_log(LOG_ERROR, "Failed to find or create target scene");
}

void SceneManager::shutdown()
{
	for (auto &pair : participant_sources_) {
		std::string source_name = pair.second;

		obs_source_t *source =
			obs_get_source_by_name(source_name.c_str());
		if (source) {
			obs_source_dec_active(source);
			obs_source_remove(source);
			obs_source_release(source);
			obs_log(LOG_DEBUG, "Released source '%s'",
				source_name.c_str());
		}
	}
	participant_sources_.clear();

	if (scene_source_) {
		obs_source_release(scene_source_);
		scene_source_ = nullptr;
	}
}

/* ---------------------------------------------------------------------------
 * add_participant – queued task: create an ffmpeg_source pointing at the
 * relay port and add it to the target scene.
 * -----------------------------------------------------------------------*/

static void add_participant_task(void *param)
{
	auto *data = static_cast<AddParticipantData *>(param);

	obs_scene_t *scene = s_scene_manager.get_scene();
	if (!scene) {
		obs_log(LOG_WARNING,
			"No target scene available for participant '%s'",
			data->name.c_str());
		delete data;
		return;
	}

	std::string source_name = "SRT - " + data->name;

	obs_source_t *existing =
		obs_get_source_by_name(source_name.c_str());
	if (existing) {
		obs_log(LOG_INFO,
			"Source '%s' already exists, updating settings",
			source_name.c_str());
		obs_data_t *settings = obs_data_create();
		std::string url = "srt://127.0.0.1:" + std::to_string(data->port);
		obs_data_set_string(settings, "input", url.c_str());
		obs_data_set_string(settings, "input_format", "mpegts");
		obs_data_set_bool(settings, "is_local_file", false);
		obs_source_update(existing, settings);
		obs_data_release(settings);
		obs_source_release(existing);
		delete data;
		return;
	}

	obs_data_t *settings = obs_data_create();
	std::string url = "srt://127.0.0.1:" + std::to_string(data->port);
	obs_data_set_string(settings, "input", url.c_str());
	obs_data_set_string(settings, "input_format", "mpegts");
	obs_data_set_bool(settings, "is_local_file", false);

	obs_source_t *source = obs_source_create("ffmpeg_source",
						source_name.c_str(),
						settings, nullptr);
	obs_data_release(settings);

	if (!source) {
		obs_log(LOG_ERROR, "Failed to create source '%s'",
			source_name.c_str());
		delete data;
		return;
	}

	obs_source_inc_active(source);

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		obs_log(LOG_INFO,
			"Added source '%s' to scene '%s' (port %d)",
			source_name.c_str(),
			s_scene_manager.get_target_scene().c_str(),
			data->port);
	} else {
		obs_log(LOG_ERROR,
			"Failed to add source '%s' to scene",
			source_name.c_str());
	}

	obs_source_release(source);
	delete data;
}

void SceneManager::add_participant(const std::string &name, int port)
{
	obs_scene_t *scene = get_scene();
	if (!scene) {
		obs_log(LOG_WARNING,
			"No target scene for participant '%s'", name.c_str());
		return;
	}

	std::string source_name = "SRT - " + name;
	participant_sources_[name] = source_name;

	auto *data = new AddParticipantData{name, port};
	obs_queue_task(OBS_TASK_UI, add_participant_task, data, false);
}

/* ---------------------------------------------------------------------------
 * remove_participant – queued task: find the scene item, remove it,
 * dec_active, release the source, and erase from the map.
 * -----------------------------------------------------------------------*/

static void remove_participant_task(void *param)
{
	auto *data = static_cast<RemoveParticipantData *>(param);

	obs_scene_t *scene = s_scene_manager.get_scene();
	if (!scene) {
		obs_log(LOG_WARNING,
			"No scene available to remove participant '%s'",
			data->name.c_str());
		delete data;
		return;
	}

	std::string source_name = "SRT - " + data->name;

	obs_source_t *source =
		obs_get_source_by_name(source_name.c_str());
	if (source) {
		obs_sceneitem_t *item =
			obs_scene_find_source(scene, source_name.c_str());
		if (item) {
			obs_sceneitem_remove(item);
		}
		obs_source_dec_active(source);
		obs_source_release(source);
		obs_log(LOG_INFO,
			"Removed source '%s' from scene",
			source_name.c_str());
	}

	delete data;
}

void SceneManager::remove_participant(const std::string &name)
{
	auto it = participant_sources_.find(name);
	if (it == participant_sources_.end()) {
		obs_log(LOG_WARNING,
			"No source mapping for participant '%s'",
			name.c_str());
		return;
	}

	participant_sources_.erase(it);

	auto *data = new RemoveParticipantData{name};
	obs_queue_task(OBS_TASK_UI, remove_participant_task, data, false);
}

/* ---------------------------------------------------------------------------
 * Accessors
 * -----------------------------------------------------------------------*/

void SceneManager::set_target_scene(const std::string &name)
{
	target_scene_name_ = name;
	obs_log(LOG_INFO, "Target scene changed to '%s'",
		target_scene_name_.c_str());
}

std::string SceneManager::get_target_scene() const
{
	return target_scene_name_;
}

void SceneManager::set_automatic_mode(bool automatic)
{
	automatic_mode_.store(automatic);
	obs_log(LOG_INFO, "Mode changed to: %s",
		automatic ? "Automatic" : "Static");
}

bool SceneManager::is_automatic_mode() const
{
	return automatic_mode_.load();
}

void SceneManager::set_default_latency_ms(int ms)
{
	default_latency_ms_.store(ms);
	obs_log(LOG_INFO, "Default latency changed to %d ms", ms);
}

int SceneManager::get_default_latency_ms() const
{
	return default_latency_ms_.load();
}

obs_source_t *SceneManager::get_scene_source() const
{
	return scene_source_;
}

obs_scene_t *SceneManager::get_scene() const
{
	if (!scene_source_)
		return nullptr;
	return obs_scene_from_source(scene_source_);
}
