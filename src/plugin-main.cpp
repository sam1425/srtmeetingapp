#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "srt-broker.h"
#include "plugin-dock.h"
#include "scene-manager.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static constexpr int BROKER_PORT = 9000;

static void obssrt_frontend_event(enum obs_frontend_event event,
				  void *private_data)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		setup_plugin_dock(BROKER_PORT);
	}
	(void)private_data;
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "SRT Meeting plugin loaded successfully (version %s)",
		PLUGIN_VERSION);

	obs_frontend_add_event_callback(obssrt_frontend_event, nullptr);

	if (!s_broker.start(BROKER_PORT)) {
		obs_log(LOG_ERROR, "Failed to start SRT broker");
		return false;
	}

	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(obssrt_frontend_event, nullptr);

	s_broker.stop();

	release_scene_source();

	if (s_dock_widget) {
		obs_frontend_remove_dock("srtMeetingControlDock");
		delete s_dock_widget;
		s_dock_widget = nullptr;
	}
	s_participants_list = nullptr;

	obs_log(LOG_INFO, "SRT Meeting plugin unloaded");
}
