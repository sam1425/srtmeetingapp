#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include "plugin-dock.h"
#include "scene-manager.h"
#include "srt-broker.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <QMainWindow>

QListWidget *s_participants_list{nullptr};
QDockWidget *s_dock_widget{nullptr};
QComboBox *s_scene_selector{nullptr};

/* ---------------------------------------------------------------------------
 * Participant list refresh (queued to UI thread).
 * -----------------------------------------------------------------------*/

static void refresh_participants_task(void *)
{
	if (!s_participants_list)
		return;
	s_participants_list->clear();
	for (const auto &name : s_broker.get_active_participant_names())
		s_participants_list->addItem(QString::fromStdString(name));
}

void update_participants_list_ui()
{
	obs_queue_task(OBS_TASK_UI, refresh_participants_task, nullptr, false);
}

/* ---------------------------------------------------------------------------
 * Scene selector helpers.
 * -----------------------------------------------------------------------*/

void populate_scene_selector()
{
	if (!s_scene_selector)
		return;

	s_scene_selector->blockSignals(true);
	s_scene_selector->clear();

	obs_enum_scenes(
		[](void *param, obs_source_t *scene) -> bool {
			static_cast<QComboBox *>(param)->addItem(
				obs_source_get_name(scene));
			return true;
		},
		s_scene_selector);

	int idx = s_scene_selector->findText(
		QString::fromStdString(s_scene_manager.get_target_scene()));
	if (idx >= 0)
		s_scene_selector->setCurrentIndex(idx);

	s_scene_selector->blockSignals(false);
}

/* ---------------------------------------------------------------------------
 * Dock setup.
 * -----------------------------------------------------------------------*/

void setup_plugin_dock(int broker_port)
{
	auto *main_window = static_cast<QMainWindow *>(
		obs_frontend_get_main_window());
	if (!main_window) {
		obs_log(LOG_WARNING, "No OBS main window");
		return;
	}

	/* Determine default scene from locale. */
	const char *locale = obs_get_locale();
	std::string default_scene = "Meeting";
	if (locale && std::string(locale).rfind("es", 0) == 0)
		default_scene = "Reunion";

	s_scene_manager.init(default_scene);

	/* Build dock widget. */
	s_dock_widget = new QDockWidget("SRT Meeting Control", main_window);
	s_dock_widget->setObjectName("srtMeetingControlDock");

	auto *content = new QWidget(s_dock_widget);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	/* -- Mode selector -- */
	auto *mode_label = new QLabel("<b>Orchestration Mode</b>", content);
	mode_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(mode_label);

	auto *auto_btn =
		new QRadioButton("Automatic Mode (Dynamic Sources)", content);
	auto_btn->setToolTip("Create source on connect, remove on disconnect.");
	auto_btn->setChecked(s_scene_manager.is_automatic_mode());
	auto_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(auto_btn);

	auto *static_btn =
		new QRadioButton("Static Mode (Persistent Sources)", content);
	static_btn->setToolTip("Keep sources after disconnect.");
	static_btn->setChecked(!s_scene_manager.is_automatic_mode());
	static_btn->setStyleSheet("color: #ebdbb2;");
	layout->addWidget(static_btn);

	auto *mode_group = new QButtonGroup(content);
	mode_group->addButton(auto_btn, 0);
	mode_group->addButton(static_btn, 1);
	QObject::connect(mode_group, &QButtonGroup::idClicked, [](int id) {
		s_scene_manager.set_automatic_mode(id == 0);
	});

	layout->addSpacing(8);

	/* -- Scene selector -- */
	auto *scene_label = new QLabel("<b>Target Scene</b>", content);
	scene_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(scene_label);

	s_scene_selector = new QComboBox(content);
	s_scene_selector->setToolTip(
		"Scene where participant sources are added.");
	s_scene_selector->setStyleSheet(
		"background-color: #3c3836; color: #ebdbb2;"
		"border: 1px solid #504945; border-radius: 4px; padding: 4px;");
	populate_scene_selector();

	QObject::connect(s_scene_selector, &QComboBox::currentTextChanged,
			 [](const QString &text) {
				 s_scene_manager.set_target_scene(
					 text.toStdString());
			 });
	layout->addWidget(s_scene_selector);

	layout->addSpacing(8);

	/* -- Latency spinner -- */
	auto *latency_label = new QLabel(
		"<b>Default Latency (auto clients)</b>", content);
	latency_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(latency_label);

	auto *latency_row = new QWidget(content);
	auto *latency_layout = new QHBoxLayout(latency_row);
	latency_layout->setContentsMargins(0, 0, 0, 0);

	auto *latency_spin = new QSpinBox(latency_row);
	latency_spin->setRange(20, 1000);
	latency_spin->setValue(s_scene_manager.get_default_latency_ms());
	latency_spin->setSuffix(" ms");
	latency_spin->setStyleSheet(
		"background-color: #3c3836; color: #b8bb26;"
		"border: 1px solid #504945; border-radius: 4px; padding: 4px;");
	latency_layout->addWidget(latency_spin);

	auto *hint = new QLabel("Lower = faster, higher = more stable",
				latency_row);
	hint->setStyleSheet("color: #a89984; font-size: 10px;");
	latency_layout->addWidget(hint);
	layout->addWidget(latency_row);

	QObject::connect(latency_spin, &QSpinBox::valueChanged, [](int val) {
		s_scene_manager.set_default_latency_ms(val);
	});

	layout->addSpacing(8);

	/* -- Participant list -- */
	auto *list_label = new QLabel("<b>Active Participants</b>", content);
	list_label->setStyleSheet("font-size: 13px; color: #fabd2f;");
	layout->addWidget(list_label);

	s_participants_list = new QListWidget(content);
	s_participants_list->setStyleSheet(
		"background-color: #3c3836; color: #ebdbb2;"
		"border: 1px solid #504945; border-radius: 6px; padding: 5px;");
	layout->addWidget(s_participants_list);

	/* -- Port info -- */
	auto *info = new QLabel(
		"SRT Broker on UDP <b>" + QString::number(broker_port) +
		"</b>",
		content);
	info->setAlignment(Qt::AlignCenter);
	info->setStyleSheet(
		"color: #83a598; font-size: 11px; margin-top: 4px;");
	layout->addWidget(info);

	content->setLayout(layout);
	s_dock_widget->setWidget(content);

	obs_frontend_add_custom_qdock("srtMeetingControlDock", s_dock_widget);
	s_dock_widget->show();
	obs_log(LOG_INFO, "Dock registered");
}
