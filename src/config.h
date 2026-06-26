#pragma once
#include <string>

bool load_config(const std::string& explicit_path = "");
extern volatile bool config_changed;
extern std::string config_path;
int watch_config();
void mark_cli(const char* name);
