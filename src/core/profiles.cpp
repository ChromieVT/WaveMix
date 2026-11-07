// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/profiles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

#include "core/apps.hpp"
#include "core/config.hpp"
#include "core/ids.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"

namespace wavemix::core {

namespace {

// The settings a profile carries: the devices. The rest are the PC's, not the setup's.
constexpr std::array<std::string_view, 3> kDeviceSettings{"personal_output", "fallback_output", "fallback_input"};

// object[key] if it's a string, else `fallback`.
std::string text(const json& object, std::string_view key, std::string fallback = "")
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

// Convert to lowercase ASCII letters.
std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// The IDs of config's channels, in order.
std::vector<std::string> channel_ids(const json& config)
{
    std::vector<std::string> ids;
    if (const auto it = config.find("channels"); it != config.end() && it->is_array())
        for (const auto& ch : *it)
            ids.push_back(text(ch, "id"));
    return ids;
}

}  // namespace

// Reads a channel's "enabled" flag; missing or non-boolean counts as enabled.
bool channel_enabled(const json& channel)
{
    const auto it = channel.find("enabled");
    return it == channel.end() || !it->is_boolean() || it->get<bool>();
}

// A copy of the config with the disabled channels taken out of "channels".
json running_config(const json& config)
{
    json out = config;
    if (out.contains("channels") && out["channels"].is_array()) {
        auto& channels = out["channels"];
        channels.erase(std::remove_if(channels.begin(), channels.end(),
                                      [](const json& ch) { return !channel_enabled(ch); }),
                       channels.end());
    }
    return out;
}

// Copies each channel (with its saved levels), the app routes, the mic and the device
// settings into a new profile.
json snapshot_profile(const json& config, const json& state, std::string_view name)
{
    json channels = json::array();
    json levels = json::object();
    const json& saved = state.contains("levels") && state["levels"].is_object() ? state["levels"] : json::object();
    for (const auto& ch : config.value("channels", json::array())) {
        const std::string id = text(ch, "id");
        channels.push_back({{"id", id},
                            {"label", text(ch, "label", id)},
                            {"icon", text(ch, "icon")},
                            {"in_vod", in_vod(ch)},
                            {"enabled", channel_enabled(ch)}});
        for (Mix mix : kMixes) {
            const std::string tag = link_tag(id, mix);
            if (saved.contains(tag) && saved[tag].is_object())
                levels[tag] = saved[tag];
        }
    }
    json devices = json::object();
    const json& settings = config.value("settings", json::object());
    for (const auto key : kDeviceSettings)
        devices[std::string(key)] = text(settings, key, key == "personal_output" ? "default" : "");
    return {
        {"name", std::string(name)},
        {"channels", channels},
        {"levels", levels},
        {"assignments", config.value("assignments", json::array())},
        {"mic", config.value("mic", json::object())},
        {"devices", devices},
    };
}

// Swaps the current channels' saved levels for the profile's, then replaces config's
// channels, routes, mic and device settings with the profile's.
void apply_profile(json& config, json& state, const json& profile)
{
    if (!state.contains("levels") || !state["levels"].is_object())
        state["levels"] = json::object();
    auto& levels = state["levels"];
    for (const auto& id : channel_ids(config))
        for (Mix mix : kMixes)
            levels.erase(link_tag(id, mix));
    const json new_levels = profile.value("levels", json::object());
    for (const auto& [tag, level] : new_levels.items())
        levels[tag] = level;

    config["channels"] = profile.value("channels", json::array());
    config["assignments"] = profile.value("assignments", json::array());
    config["mic"] = profile.value("mic", json::object());
    const json& devices = profile.value("devices", json::object());
    for (const auto key : kDeviceSettings)
        config["settings"][std::string(key)] = text(devices, key, key == "personal_output" ? "default" : "");
}


// Rebuilds the profile from what's valid in it: up to kMaxChannels channels with unique,
// valid IDs, their levels clamped, one route per app into a channel it has, the mic
// chain capped at kMaxMicChain, and the device settings.
std::optional<json> clean_profile(const json& raw)
{
    if (!raw.is_object())
        return std::nullopt;
    if (const auto it = raw.find("wavemix_profile"); it != raw.end()) {
        if (!it->is_number_integer() || it->get<int>() > kProfileFormat || it->get<int>() < 1)
            return std::nullopt;
    }

    json channels = json::array();
    std::set<std::string> ids;
    for (const auto& ch : raw.value("channels", json::array())) {
        if (!ch.is_object() || channels.size() >= static_cast<size_t>(kMaxChannels))
            continue;
        const std::string id = text(ch, "id");
        if (!valid_id(id) || ids.contains(id))
            continue;
        ids.insert(id);
        std::string label = text(ch, "label");
        if (label.empty())
            label = id;
        channels.push_back({{"id", id},
                            {"label", label},
                            {"icon", text(ch, "icon")},
                            {"in_vod", in_vod(ch)},
                            {"enabled", channel_enabled(ch)}});
    }

    json levels = json::object();
    const json raw_levels = raw.value("levels", json::object());
    for (const auto& id : ids) {
        for (Mix mix : kMixes) {
            const std::string tag = link_tag(id, mix);
            if (!raw_levels.is_object() || !raw_levels.contains(tag) || !raw_levels[tag].is_object())
                continue;
            const json& level = raw_levels[tag];
            const double volume = level.contains("volume") && level["volume"].is_number()
                                      ? level["volume"].get<double>()
                                      : 1.0;
            levels[tag] = {{"volume", round4(clamp_volume(volume))},
                           {"muted", level.contains("muted") && level["muted"].is_boolean() && level["muted"].get<bool>()}};
        }
    }

    json assignments = json::array();
    std::set<std::string> apps;
    for (const auto& rule : raw.value("assignments", json::array())) {
        if (!rule.is_object())
            continue;
        const std::string app = text(rule, "app");
        const std::string channel = text(rule, "channel");
        if (app.empty() || !ids.contains(channel) || apps.contains(app))
            continue;
        apps.insert(app);
        assignments.push_back({{"app", app}, {"label", text(rule, "label", app)}, {"channel", channel}});
    }

    json mic = json::object();
    if (const auto it = raw.find("mic"); it != raw.end() && it->is_object()) {
        if (it->contains("enabled") && (*it)["enabled"].is_boolean())
            mic["enabled"] = (*it)["enabled"];
        mic["input"] = text(*it, "input", "default");
        json chain = json::array();
        for (const auto& entry : it->value("chain", json::array()))
            if (entry.is_object() && entry.contains("filter") && entry["filter"].is_string() &&
                chain.size() < static_cast<size_t>(kMaxMicChain))
                chain.push_back(entry);
        mic["chain"] = chain;
    }

    json devices = json::object();
    const json raw_devices = raw.value("devices", json::object());
    for (const auto key : kDeviceSettings) {
        std::string value = raw_devices.is_object() ? text(raw_devices, key) : "";
        if (key == "personal_output" && value.empty())
            value = "default";
        devices[std::string(key)] = value;
    }

    return json{
        {"name", clean_profile_name(text(raw, "name"))},
        {"channels", channels},
        {"levels", levels},
        {"assignments", assignments},
        {"mic", mic},
        {"devices", devices},
    };
}

// Trims whitespace from both ends, then refuses names that are empty, longer than
// kMaxProfileName or contain control characters.
std::string clean_profile_name(std::string_view name)
{
    const auto first = name.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos)
        return "";
    const auto last = name.find_last_not_of(" \t\n\r");
    std::string out(name.substr(first, last - first + 1));
    if (out.size() > kMaxProfileName)
        return "";
    if (std::any_of(out.begin(), out.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return "";
    return out;
}

// Adds " (2)", " (3)"... until no profile has the name, shortening `wanted` so the
// result stays within kMaxProfileName.
std::string unique_profile_name(const json& config, std::string_view wanted)
{
    std::string name(wanted);
    for (int n = 2; find_profile(config, name); ++n) {
        const std::string suffix = " (" + std::to_string(n) + ")";
        std::string base(wanted.substr(0, kMaxProfileName - std::min(kMaxProfileName, suffix.size())));
        name = base + suffix;
    }
    return name;
}

// Searches config "profiles" for the name, ignoring ASCII case.
json* find_profile(json& config, std::string_view name)
{
    if (!config.contains("profiles") || !config["profiles"].is_array())
        return nullptr;
    const std::string wanted = lower(name);
    for (auto& profile : config["profiles"])
        if (lower(text(profile, "name")) == wanted)
            return &profile;
    return nullptr;
}

// The read-only version of the above.
const json* find_profile(const json& config, std::string_view name)
{
    return find_profile(const_cast<json&>(config), name);
}

// Reads config "profile", falling back to "Default" when it's missing or unusable.
std::string active_profile(const json& config)
{
    const std::string name = clean_profile_name(text(config, "profile"));
    return name.empty() ? std::string(kDefaultProfile) : name;
}

// Cleans the stored profiles (dropping broken, unnamed and repeated ones), then makes
// sure the profile in use is among them, adding it from the live setup if it isn't.
void ensure_profiles(json& config, const json& state)
{
    json profiles = json::array();
    if (config.contains("profiles") && config["profiles"].is_array()) {
        json kept = json::object();  // {"profiles": [...]} so far, for the name checks
        kept["profiles"] = json::array();
        for (const auto& raw : config["profiles"]) {
            auto profile = clean_profile(raw);
            if (!profile || (*profile)["name"] == "" || find_profile(kept, (*profile)["name"].get<std::string>()))
                continue;
            kept["profiles"].push_back(*profile);
        }
        profiles = std::move(kept["profiles"]);
    }
    config["profiles"] = std::move(profiles);
    const std::string active = active_profile(config);
    if (const json* found = find_profile(config, active))
        config["profile"] = (*found)["name"];  // as it's spelled there
    else {
        config["profile"] = active;
        config["profiles"].push_back(snapshot_profile(config, state, active));
    }
}

// Resets each device the profile names that isn't in `outputs` or `inputs`: the Personal
// output and mic input to "default", the fallbacks to "".
void match_devices(json& profile, const std::vector<std::string>& outputs, const std::vector<std::string>& inputs)
{
    const auto has = [](const std::vector<std::string>& list, const std::string& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    auto& devices = profile["devices"];
    if (const std::string out = text(devices, "personal_output"); out != "default" && !has(outputs, out))
        devices["personal_output"] = "default";
    if (!has(outputs, text(devices, "fallback_output")))
        devices["fallback_output"] = "";
    if (!has(inputs, text(devices, "fallback_input")))
        devices["fallback_input"] = "";
    if (profile.contains("mic") && profile["mic"].is_object()) {
        const std::string input = text(profile["mic"], "input", "default");
        if (input != "default" && !has(inputs, input))
            profile["mic"]["input"] = "default";
    }
}

}  // namespace wavemix::core
