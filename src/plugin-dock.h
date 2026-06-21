#pragma once

#include <string>

#include <QListWidget>
#include <QDockWidget>
#include <QComboBox>

extern QListWidget *s_participants_list;
extern QDockWidget *s_dock_widget;
extern QComboBox *s_scene_selector;

void setup_plugin_dock(int broker_port);
void populate_scene_selector();
void update_participants_list_ui();
void release_scene_source();
