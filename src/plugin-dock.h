#pragma once

#include <memory>
#include <string>

#include <QListWidget>
#include <QDockWidget>
#include <QComboBox>

class PacketBuffer;

extern std::string s_target_scene_name;
extern QListWidget *s_participants_list;
extern QDockWidget *s_dock_widget;
extern QComboBox *s_scene_selector;

void setup_plugin_dock(int broker_port);
void populate_scene_selector();
void update_participants_list_ui();
void create_obs_srt_source(const std::string &name,
			   std::shared_ptr<PacketBuffer> buffer);
void remove_obs_srt_source(const std::string &name);
void release_scene_source();
