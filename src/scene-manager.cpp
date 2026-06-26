#include <obs-module.h>
#include <plugin-support.h>

#include "scene-manager.h"

SceneManager s_scene_manager;

/* ---------------------------------------------------------------------------
 * Task data structs (passed to obs_queue_task).
 * -----------------------------------------------------------------------*/

struct AddParticipantData {
	std::string name;
	int port;
};

struct RemoveParticipantData {
	std::string name;
};

/* ---------------------------------------------------------------------------
 * Queued OBS tasks (run on the UI thread).
 * -----------------------------------------------------------------------*/

static void add_participant_task(void *param)
{
	auto *data = static_cast<AddParticipantData *>(param);

	obs_scene_t *scene = s_scene_manager.get_scene();
	if (!scene) {
		obs_log(LOG_WARNING, "No scene for '%s'", data->name.c_str());
		delete data;
		return;
	}

	std::string source_name = "SRT - " + data->name;

	/* If the source already exists, just update its URL. */
	obs_source_t *existing = obs_get_source_by_name(source_name.c_str());
	if (existing) {
		obs_data_t *settings = obs_data_create();
		std::string url =
			"srt://127.0.0.1:" + std::to_string(data->port);
		obs_data_set_string(settings, "input", url.c_str());
		obs_data_set_string(settings, "input_format", "mpegts");
		obs_data_set_bool(settings, "is_local_file", false);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		obs_source_update(existing, settings);
		obs_data_release(settings);
		obs_source_release(existing);
		delete data;
		return;
	}

	obs_data_t *settings = obs_data_create();
	std::string url =
		"srt://127.0.0.1:" + std::to_string(data->port);
	obs_data_set_string(settings, "input", url.c_str());
	obs_data_set_string(settings, "input_format", "mpegts");
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);

	obs_source_t *source = obs_source_create("ffmpeg_source",
						 source_name.c_str(), settings,
						 nullptr);
	obs_data_release(settings);

	if (!source) {
		obs_log(LOG_ERROR, "Failed to create '%s'",
			source_name.c_str());
		delete data;
		return;
	}

	obs_source_inc_active(source);

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		obs_log(LOG_INFO, "Added '%s' to scene '%s' (:%d)",
			source_name.c_str(),
			s_scene_manager.get_target_scene().c_str(),
			data->port);
	} else {
		obs_log(LOG_ERROR, "Failed to add '%s' to scene",
			source_name.c_str());
	}

	obs_source_release(source);
	delete data;
}

static void remove_participant_task(void *param)
{
	auto *data = static_cast<RemoveParticipantData *>(param);

	obs_scene_t *scene = s_scene_manager.get_scene();
	if (!scene) {
		delete data;
		return;
	}

	std::string source_name = "SRT - " + data->name;

	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	if (source) {
		obs_sceneitem_t *item =
			obs_scene_find_source(scene, source_name.c_str());
		if (item)
			obs_sceneitem_remove(item);
		obs_source_dec_active(source);
		obs_source_release(source);
		obs_log(LOG_INFO, "Removed '%s'", source_name.c_str());
	}

	delete data;
}

/* ---------------------------------------------------------------------------
 * SceneManager implementation
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

	/* Try to find an existing scene. */
	obs_source_t *src = obs_get_source_by_name(target_scene_name_.c_str());
	if (src && obs_scene_from_source(src)) {
		scene_source_ = src;
		obs_log(LOG_INFO, "Using scene '%s'",
			target_scene_name_.c_str());
		return;
	}
	if (src)
		obs_source_release(src);

	/* Create a new scene. */
	obs_scene_t *scene = obs_scene_create(target_scene_name_.c_str());
	if (scene) {
		scene_source_ =
			obs_source_get_ref(obs_scene_get_source(scene));
		obs_log(LOG_INFO, "Created scene '%s'",
			target_scene_name_.c_str());
		return;
	}

	/* Fallback based on locale. */
	const char *locale = obs_get_locale();
	std::string fallback = "Meeting";
	if (locale && std::string(locale).rfind("es", 0) == 0)
		fallback = "Reunion";

	scene = obs_scene_create(fallback.c_str());
	if (scene) {
		target_scene_name_ = fallback;
		scene_source_ =
			obs_source_get_ref(obs_scene_get_source(scene));
		obs_log(LOG_INFO, "Created fallback scene '%s'", fallback.c_str());
		return;
	}

	obs_log(LOG_ERROR, "Failed to create any scene");
}

void SceneManager::shutdown()
{
	for (auto &[name, source_name] : participant_sources_) {
		obs_source_t *src = obs_get_source_by_name(source_name.c_str());
		if (src) {
			obs_source_dec_active(src);
			obs_source_remove(src);
			obs_source_release(src);
		}
	}
	participant_sources_.clear();

	if (scene_source_) {
		obs_source_release(scene_source_);
		scene_source_ = nullptr;
	}
}

void SceneManager::add_participant(const std::string &name, int port)
{
	if (!get_scene()) {
		obs_log(LOG_WARNING, "No scene for '%s'", name.c_str());
		return;
	}

	participant_sources_[name] = "SRT - " + name;

	auto *data = new AddParticipantData{name, port};
	obs_queue_task(OBS_TASK_UI, add_participant_task, data, false);
}

void SceneManager::remove_participant(const std::string &name)
{
	auto it = participant_sources_.find(name);
	if (it == participant_sources_.end())
		return;

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
	obs_log(LOG_INFO, "Target scene -> '%s'", name.c_str());
}

std::string SceneManager::get_target_scene() const
{
	return target_scene_name_;
}

void SceneManager::set_automatic_mode(bool automatic)
{
	automatic_mode_.store(automatic);
	obs_log(LOG_INFO, "Mode -> %s", automatic ? "Auto" : "Static");
}

bool SceneManager::is_automatic_mode() const
{
	return automatic_mode_.load();
}

void SceneManager::set_default_latency_ms(int ms)
{
	default_latency_ms_.store(ms);
	obs_log(LOG_INFO, "Default latency -> %dms", ms);
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
