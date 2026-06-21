#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "plugin-dock.h"
#include "srt-broker.h"
#include "srt-source.h"

#include <string>
#include <cstring>

#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QSpinBox>
#include <QHBoxLayout>

std::string s_target_scene_name;
QListWidget *s_participants_list{nullptr};
QDockWidget *s_dock_widget{nullptr};
QComboBox *s_scene_selector{nullptr};

static obs_source_t *s_scene_source{nullptr};

struct SourceTaskData {
	std::string name;
	std::shared_ptr<PacketBuffer> buffer;
};

static obs_scene_t *find_or_create_target_scene()
{
	if (s_scene_source) {
		obs_source_release(s_scene_source);
		s_scene_source = nullptr;
	}

	obs_source_t *source =
		obs_get_source_by_name(s_target_scene_name.c_str());
	if (source) {
		obs_scene_t *scene = obs_scene_from_source(source);
		if (scene) {
			s_scene_source = source;
			return scene;
		}
		obs_source_release(source);
	}

	obs_scene_t *new_scene =
		obs_scene_create(s_target_scene_name.c_str());
	if (new_scene) {
		s_scene_source = obs_source_get_ref(
			obs_scene_get_source(new_scene));
		obs_log(LOG_INFO, "Created target scene '%s'",
			s_target_scene_name.c_str());
		return new_scene;
	}

	const char *locale = obs_get_locale();
	std::string fallback_name = "Meeting";
	if (locale && (strncmp(locale, "es", 2) == 0))
		fallback_name = "Reunion";

	new_scene = obs_scene_create(fallback_name.c_str());
	if (new_scene) {
		s_target_scene_name = fallback_name;
		s_scene_source = obs_source_get_ref(
			obs_scene_get_source(new_scene));
		obs_log(LOG_INFO,
			"Created fallback scene '%s' based on locale '%s'",
			fallback_name.c_str(), locale ? locale : "unknown");
		return new_scene;
	}

	obs_log(LOG_ERROR, "Failed to find or create target scene");
	return nullptr;
}

static void create_obs_srt_source_task(void *param)
{
	auto *data = static_cast<SourceTaskData *>(param);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "participant_name", data->name.c_str());

	obs_scene_t *scene = find_or_create_target_scene();
	if (scene) {
		std::string source_name = "SRT - " + data->name;

		obs_source_t *existing_source =
			obs_get_source_by_name(source_name.c_str());
		if (existing_source) {
			obs_log(LOG_INFO,
				"Source '%s' already exists in OBS, updating settings",
				source_name.c_str());
			obs_source_update(existing_source, settings);
			obs_source_release(existing_source);
		} else {
			obs_source_t *srt_source = obs_source_create(
				"srt_participant", source_name.c_str(), settings,
				nullptr);
			if (srt_source) {
				// Prevent OBS from deactivating this source on scene switch
				obs_source_inc_active(srt_source);

				obs_sceneitem_t *scene_item =
					obs_scene_add(scene, srt_source);
				if (scene_item) {
					obs_log(LOG_INFO,
						"Added SRT source '%s' to scene '%s'",
						source_name.c_str(),
						s_target_scene_name.c_str());
				} else {
					obs_log(LOG_ERROR,
						"Failed to add SRT source '%s' to scene",
						source_name.c_str());
				}
				obs_source_release(srt_source);
			} else {
				obs_log(LOG_ERROR,
					"Failed to create OBS source '%s'",
					source_name.c_str());
			}
		}
	} else {
		obs_log(LOG_WARNING, "No target scene found to add source");
	}

	obs_data_release(settings);
	delete data;
}

static void remove_obs_srt_source_task(void *param)
{
	auto *name = static_cast<std::string *>(param);
	std::string source_name = "SRT - " + *name;

	obs_source_t *source =
		obs_get_source_by_name(source_name.c_str());
	if (source) {
		obs_source_dec_active(source); // Match the inc_active
		obs_source_remove(source);
		obs_source_release(source);
		obs_log(LOG_INFO, "Removed SRT source '%s' from OBS",
			source_name.c_str());
	} else {
		obs_log(LOG_WARNING, "Source '%s' not found for removal",
			source_name.c_str());
	}

	delete name;
}

static void update_participants_list_ui_task(void *param)
{
	if (!s_participants_list)
		return;
	s_participants_list->clear();
	std::vector<std::string> names = s_broker.get_active_participant_names();
	for (const auto &name : names) {
		s_participants_list->addItem(QString::fromStdString(name));
	}
	(void)param;
}

void update_participants_list_ui()
{
	obs_queue_task(OBS_TASK_UI, update_participants_list_ui_task, nullptr,
		       false);
}

void populate_scene_selector()
{
	if (!s_scene_selector)
		return;

	s_scene_selector->blockSignals(true);
	s_scene_selector->clear();

	obs_enum_scenes(
		[](void *param, obs_source_t *scene) -> bool {
			auto *combo = static_cast<QComboBox *>(param);
			combo->addItem(obs_source_get_name(scene));
			return true;
		},
		s_scene_selector);

	int idx = s_scene_selector->findText(
		QString::fromStdString(s_target_scene_name));
	if (idx >= 0)
		s_scene_selector->setCurrentIndex(idx);

	s_scene_selector->blockSignals(false);
}

void setup_plugin_dock(int broker_port)
{
	QMainWindow *main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window) {
		obs_log(LOG_WARNING,
			"OBS main window not found, cannot create dock");
		return;
	}

	{
		const char *locale = obs_get_locale();
		if (locale && strncmp(locale, "es", 2) == 0)
			s_target_scene_name = "Reunion";
		else
			s_target_scene_name = "Meeting";
		obs_log(LOG_INFO,
			"Default target scene set to '%s' (locale: %s)",
			s_target_scene_name.c_str(),
			locale ? locale : "unknown");
	}

	s_dock_widget = new QDockWidget("SRT Meeting Control", main_window);
	s_dock_widget->setObjectName("srtMeetingControlDock");

	QWidget *content = new QWidget(s_dock_widget);
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	QLabel *mode_label = new QLabel("<b>Orchestration Mode</b>", content);
	mode_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(mode_label);

	QRadioButton *auto_btn =
		new QRadioButton("Automatic Mode (Dynamic Sources)", content);
	auto_btn->setToolTip(
		"Automatically creates OBS source when client connects, and deletes it when they disconnect.");
	auto_btn->setChecked(s_automatic_mode.load());
	auto_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(auto_btn);

	QRadioButton *static_btn =
		new QRadioButton("Static Mode (Persistent Sources)", content);
	static_btn->setToolTip(
		"Creates OBS source on first connection, but keeps it when they disconnect. Updates source settings if they reconnect.");
	static_btn->setChecked(!s_automatic_mode.load());
	static_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(static_btn);

	QButtonGroup *group = new QButtonGroup(content);
	group->addButton(auto_btn, 0);
	group->addButton(static_btn, 1);

	QObject::connect(group, &QButtonGroup::idClicked, [](int id) {
		s_automatic_mode.store(id == 0);
		obs_log(LOG_INFO, "Connection Mode changed to: %s",
			(id == 0) ? "Automatic" : "Static");
	});

	layout->addSpacing(8);

	QLabel *scene_label =
		new QLabel("<b>Target Scene</b>", content);
	scene_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(scene_label);

	s_scene_selector = new QComboBox(content);
	s_scene_selector->setEditable(false);
	s_scene_selector->setToolTip(
		"Scene where participant sources are added. Created automatically if it doesn't exist.");
	s_scene_selector->setStyleSheet(
		"background-color: #3c3836;"
		"color: #ebdbb2;"
		"border: 1px solid #504945;"
		"border-radius: 4px;"
		"padding: 4px;");
	populate_scene_selector();

	QObject::connect(s_scene_selector, &QComboBox::currentTextChanged,
			 [](const QString &text) {
				 s_target_scene_name = text.toStdString();
				 obs_log(LOG_INFO,
					 "Target scene changed to '%s'",
					 s_target_scene_name.c_str());
			 });

	layout->addWidget(s_scene_selector);

	layout->addSpacing(8);

	QLabel *latency_label =
		new QLabel("<b>Default Latency (auto clients)</b>", content);
	latency_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(latency_label);

	QWidget *latency_row = new QWidget(content);
	QHBoxLayout *latency_layout = new QHBoxLayout(latency_row);
	latency_layout->setContentsMargins(0, 0, 0, 0);

	QSpinBox *latency_spin = new QSpinBox(latency_row);
	latency_spin->setRange(20, 1000);
	latency_spin->setValue(s_default_latency_ms.load());
	latency_spin->setSuffix(" ms");
	latency_spin->setToolTip(
		"Latency used when client sends 'auto' (ignored when client specifies a value).");
	latency_spin->setStyleSheet(
		"background-color: #3c3836;"
		"color: #b8bb26;"
		"border: 1px solid #504945;"
		"border-radius: 4px;"
		"padding: 4px;");
	latency_layout->addWidget(latency_spin);

	QLabel *latency_hint =
		new QLabel("Lower = faster, higher = more stable", latency_row);
	latency_hint->setStyleSheet("color: #a89984; font-size: 10px;");
	latency_layout->addWidget(latency_hint);

	layout->addWidget(latency_row);

	QObject::connect(latency_spin, &QSpinBox::valueChanged, [](int val) {
		s_default_latency_ms.store(val);
		obs_log(LOG_INFO, "Default latency changed to %d ms", val);
	});

	layout->addSpacing(8);

	QLabel *list_label =
		new QLabel("<b>Active Participants</b>", content);
	list_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(list_label);

	s_participants_list = new QListWidget(content);
	s_participants_list->setToolTip(
		"List of currently streaming participants.");
	s_participants_list->setStyleSheet(
		"background-color: #3c3836;"
		"color: #ebdbb2;"
		"border: 1px solid #504945;"
		"border-radius: 6px;"
		"padding: 5px;");
	layout->addWidget(s_participants_list);

	QString portStr = QString::number(broker_port);
	QLabel *info_label = new QLabel(
		"SRT Broker Listening on UDP port <b>" + portStr + "</b>",
		content);
	info_label->setAlignment(Qt::AlignCenter);
	info_label->setStyleSheet("color: #83a598; font-size: 11px; margin-top: 4px;");
	layout->addWidget(info_label);

	content->setLayout(layout);
	s_dock_widget->setWidget(content);

	find_or_create_target_scene();
	obs_log(LOG_INFO, "Startup: target scene '%s' ready",
		s_target_scene_name.c_str());

	obs_frontend_add_custom_qdock("srtMeetingControlDock", s_dock_widget);
	s_dock_widget->show();
	obs_log(LOG_INFO, "SRT Meeting UI dock registered successfully");
}

void create_obs_srt_source(const std::string &name, std::shared_ptr<PacketBuffer> buffer)
{
	auto *data = new SourceTaskData{name, buffer};
	obs_queue_task(OBS_TASK_UI, create_obs_srt_source_task, data, false);
}

void remove_obs_srt_source(const std::string &name)
{
	auto *data = new std::string(name);
	obs_queue_task(OBS_TASK_UI, remove_obs_srt_source_task, data, false);
}

void release_scene_source()
{
	if (s_scene_source) {
		obs_source_release(s_scene_source);
		s_scene_source = nullptr;
	}
}
