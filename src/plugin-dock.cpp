#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "plugin-dock.h"
#include "srt-broker.h"
#include "scene-manager.h"

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

QListWidget *s_participants_list{nullptr};
QDockWidget *s_dock_widget{nullptr};
QComboBox *s_scene_selector{nullptr};

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

	std::string current = s_scene_manager.get_target_scene();
	int idx = s_scene_selector->findText(
		QString::fromStdString(current));
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

	const char *locale = obs_get_locale();
	std::string default_scene = "Meeting";
	if (locale && strncmp(locale, "es", 2) == 0)
		default_scene = "Reunion";

	s_scene_manager.init(default_scene);

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
	auto_btn->setChecked(s_scene_manager.is_automatic_mode());
	auto_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(auto_btn);

	QRadioButton *static_btn =
		new QRadioButton("Static Mode (Persistent Sources)", content);
	static_btn->setToolTip(
		"Creates OBS source on first connection, but keeps it when they disconnect. Updates source settings if they reconnect.");
	static_btn->setChecked(!s_scene_manager.is_automatic_mode());
	static_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(static_btn);

	QButtonGroup *group = new QButtonGroup(content);
	group->addButton(auto_btn, 0);
	group->addButton(static_btn, 1);

	QObject::connect(group, &QButtonGroup::idClicked, [](int id) {
		s_scene_manager.set_automatic_mode(id == 0);
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
				 s_scene_manager.set_target_scene(
					 text.toStdString());
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
	latency_spin->setValue(s_scene_manager.get_default_latency_ms());
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
		s_scene_manager.set_default_latency_ms(val);
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

	obs_frontend_add_custom_qdock("srtMeetingControlDock", s_dock_widget);
	s_dock_widget->show();
	obs_log(LOG_INFO, "SRT Meeting UI dock registered successfully");
}

void release_scene_source()
{
	s_scene_manager.shutdown();
}
