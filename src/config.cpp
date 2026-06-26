#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

volatile bool config_changed = false;
std::string config_path;

// CLI-set flags: true if the option was explicitly given on command line
static bool cli_source = false;
static bool cli_name = false;
static bool cli_rotate = false;
static bool cli_sharpness = false;
static bool cli_backlight = false;
static bool cli_focus = false;
static bool cli_frame_size = false;
static bool cli_input_fmt = false;
static bool cli_aspect = false;
static bool cli_brightness = false;
static bool cli_contrast = false;
static bool cli_saturation = false;
static bool cli_white_balance = false;
static bool cli_exposure_comp = false;

// These are declared in main.cpp — we declare them extern here
extern std::string opt_source;
extern std::string opt_name;
extern int opt_rotate;
extern int opt_sharpness;
extern int opt_backlight;
extern int opt_focus_abs;
extern bool opt_focus_auto;
extern std::string opt_frame_size;
extern std::string opt_input_fmt;
extern std::string opt_aspect_ratio;
extern std::string opt_brightness;
extern std::string opt_contrast;
extern std::string opt_saturation;
extern std::string opt_white_balance;
extern std::string opt_exposure_comp;

// Mark an option as set from CLI
void mark_cli(const char* name) {
    if (strcmp(name, "source") == 0) cli_source = true;
    else if (strcmp(name, "name") == 0) cli_name = true;
    else if (strcmp(name, "rotate") == 0) cli_rotate = true;
    else if (strcmp(name, "sharpness") == 0) cli_sharpness = true;
    else if (strcmp(name, "backlight_compensation") == 0) cli_backlight = true;
    else if (strcmp(name, "focus") == 0) cli_focus = true;
    else if (strcmp(name, "frame_size") == 0) cli_frame_size = true;
    else if (strcmp(name, "input_format") == 0) cli_input_fmt = true;
    else if (strcmp(name, "aspect_ratio") == 0) cli_aspect = true;
    else if (strcmp(name, "brightness") == 0) cli_brightness = true;
    else if (strcmp(name, "contrast") == 0) cli_contrast = true;
    else if (strcmp(name, "saturation") == 0) cli_saturation = true;
    else if (strcmp(name, "white_balance") == 0) cli_white_balance = true;
    else if (strcmp(name, "exposure_compensation") == 0) cli_exposure_comp = true;
}

static std::string find_config(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        struct stat st;
        if (stat(explicit_path.c_str(), &st) == 0)
            return explicit_path;
        fprintf(stderr, "Config file not found: %s\n",
                explicit_path.c_str());
        return "";
    }
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        std::string p = std::string(xdg) + "/WebCamProxy/config.yml";
        struct stat st;
        if (stat(p.c_str(), &st) == 0) return p;
    }
    const char* home = getenv("HOME");
    if (home) {
        std::string p = std::string(home) +
                        "/.config/WebCamProxy/config.yml";
        struct stat st;
        if (stat(p.c_str(), &st) == 0) return p;
    }
    {
        struct stat st;
        if (stat("/etc/WebCamProxy/config.yml", &st) == 0)
            return "/etc/WebCamProxy/config.yml";
    }
    return "";
}

template<typename T>
static void set_if_not_cli(T& var, const YAML::Node& node,
                           const char* key, bool& cli_flag) {
    if (!cli_flag && node[key]) {
        if constexpr (std::is_same_v<T, int>)
            var = node[key].as<int>();
        else if constexpr (std::is_same_v<T, bool>)
            var = node[key].as<bool>();
        else
            var = node[key].as<std::string>();
    }
}

bool load_config(const std::string& explicit_path) {
    std::string path = find_config(explicit_path);
    if (path.empty()) return false;

    config_path = path;
    fprintf(stderr, "Loading config: %s\n", path.c_str());

    try {
        YAML::Node root = YAML::LoadFile(path);
        if (!root.IsMap()) return false;

        set_if_not_cli(opt_source, root, "source", cli_source);
        set_if_not_cli(opt_name, root, "name", cli_name);
        set_if_not_cli(opt_rotate, root, "rotate", cli_rotate);
        set_if_not_cli(opt_sharpness, root, "sharpness", cli_sharpness);
        set_if_not_cli(opt_backlight, root, "backlight_compensation",
                       cli_backlight);
        set_if_not_cli(opt_frame_size, root, "input_frame_size",
                       cli_frame_size);
        set_if_not_cli(opt_input_fmt, root, "input_format", cli_input_fmt);
        set_if_not_cli(opt_aspect_ratio, root, "output_aspect_ratio",
                       cli_aspect);
        set_if_not_cli(opt_brightness, root, "brightness", cli_brightness);
        set_if_not_cli(opt_contrast, root, "contrast", cli_contrast);
        set_if_not_cli(opt_saturation, root, "saturation", cli_saturation);
        set_if_not_cli(opt_white_balance, root, "white_balance",
                       cli_white_balance);
        set_if_not_cli(opt_exposure_comp, root, "exposure_compensation",
                       cli_exposure_comp);

        // focus: special handling
        if (!cli_focus && root["focus"]) {
            std::string f = root["focus"].as<std::string>();
            if (f == "auto") {
                opt_focus_auto = true;
                opt_focus_abs = -1;
            } else {
                opt_focus_auto = false;
                opt_focus_abs = root["focus"].as<int>();
            }
        }
    } catch (const YAML::Exception& e) {
        fprintf(stderr, "Config parse error: %s\n", e.what());
        return false;
    }
    return true;
}

int watch_config() {
    if (config_path.empty()) return -1;
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return -1;
    int wd = inotify_add_watch(fd, config_path.c_str(),
                               IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) { close(fd); return -1; }
    return fd;
}
