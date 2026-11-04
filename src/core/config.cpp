// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/config.hpp"

#include <iostream>
#include <system_error>

#include "core/ids.hpp"

namespace fs = std::filesystem;

namespace wavemix::core {

// The first-run config: schema version, no channels or assignments, default settings.
json default_config()
{
    return {
        {"version", kConfigVersion},
        {"channels", json::array()},     // [{"id", "label", "icon"}]
        {"assignments", json::array()},  // [{"app": key, "label": display name, "channel": id}]
        {"settings",
         {
             {"personal_output", "default"},  // "default" or a sink name
             {"compact_popup", true},          // tray icon opens compact sliders
             {"autostart", true},              // the service starts at login
             {"start_minimized", true},        // at login, only the tray icon (no window)
             {"vod_enabled", false},           // the VOD Mix for OBS's VOD track
             {"pulse_routing", false},         // the channels' loopbacks in pipewire-pulse
             {"theme", "dark"},                // the window's built-in theme
             {"custom_theme", ""},             // a CSS file on top of it, or ""
         }},
    };
}

// The first-run state: no saved levels and no remembered hardware default.
json default_state()
{
    return {
        {"levels", json::object()},  // link tag -> {"volume", "muted"}
        {"last_hardware_default", nullptr},
    };
}

namespace {

// True if a config channel entry is an object with a string ID that passes valid_id().
bool has_valid_id(const json& channel)
{
    if (!channel.is_object() || !channel.contains("id") || !channel["id"].is_string())
        return false;
    return valid_id(channel["id"].get<std::string>());
}

}  // namespace

// Reads a channel's "in_vod" flag; missing or non-boolean counts as in the VOD.
bool in_vod(const json& channel)
{
    const auto it = channel.find("in_vod");
    return it == channel.end() || !it->is_boolean() || it->get<bool>();
}

// Reads settings "vod_enabled"; anything but a true boolean counts as off.
bool vod_enabled(const json& config)
{
    const auto settings = config.find("settings");
    if (settings == config.end() || !settings->is_object())
        return false;
    const auto it = settings->find("vod_enabled");
    return it != settings->end() && it->is_boolean() && it->get<bool>();
}

// Reads settings "pulse_routing"; anything but a true boolean counts as off.
bool pulse_routing(const json& config)
{
    const auto settings = config.find("settings");
    if (settings == config.end() || !settings->is_object())
        return false;
    const auto it = settings->find("pulse_routing");
    return it != settings->end() && it->is_boolean() && it->get<bool>();
}

// A channel gets a VOD loopback only when the VOD Mix is on and the channel is in it.
bool wants_vod_link(const json& config, const json& channel)
{
    return vod_enabled(config) && in_vod(channel);
}

// Loads config.json over the defaults, then drops invalid channels and a broken "settings".
json load_config(const Paths& paths)
{
    json config = load_merged(paths.config_file, default_config());

    // Keep only channels with a usable ID: the ID is part of sink names, so a bad one
    // could produce names PipeWire rejects.
    json channels = json::array();
    if (config["channels"].is_array()) {
        for (const auto& channel : config["channels"]) {
            if (has_valid_id(channel))
                channels.push_back(channel);
        }
    }
    config["channels"] = std::move(channels);

    // A "settings" value that isn't an object is unusable: start over with the
    // defaults, so the daemon always comes up.
    if (!config["settings"].is_object())
        config["settings"] = default_config()["settings"];

    return config;
}

// Writes config.json atomically.
void save_config(const Paths& paths, const json& config)
{
    atomic_write_json(paths.config_file, config);
}

// Loads state.json over the defaults and makes sure "levels" is an object.
json load_state(const Paths& paths)
{
    json state = load_merged(paths.state_file, default_state());
    if (!state["levels"].is_object())
        state["levels"] = json::object();
    return state;
}

// Writes state.json atomically.
void save_state(const Paths& paths, const json& state)
{
    atomic_write_json(paths.state_file, state);
}

}  // namespace wavemix::core
