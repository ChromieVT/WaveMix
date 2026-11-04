// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/apps.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "core/names.hpp"

namespace wavemix::core {

namespace {

// A property's value, or "" if it isn't set. Empty and missing are treated the same:
// an app that sets application.name to "" hasn't really named itself.
std::string_view get(const Props& props, std::string_view key)
{
    const auto it = props.find(key);
    return it == props.end() ? std::string_view() : std::string_view(it->second);
}

}  // namespace

// The key an app's route is remembered by: "binary:..." or "name:...", if either is set.
std::optional<std::string> app_key(const Props& props)
{
    // The binary is the better key: it's the same for every window and tab of an app,
    // while some apps change application.name between versions or languages.
    if (auto binary = get(props, "application.process.binary"); !binary.empty())
        return "binary:" + std::string(binary);
    if (auto name = get(props, "application.name"); !name.empty())
        return "name:" + std::string(name);
    return std::nullopt;
}

// The app's display name, from the first of its naming properties that's set.
std::string app_name(const Props& props, unsigned id)
{
    for (auto key : {"application.name", "application.process.binary", "media.name"}) {
        if (auto value = get(props, key); !value.empty())
            return std::string(value);
    }
    return "Stream " + std::to_string(id);
}

// Recognises WaveMix's own nodes and loopbacks so they're never listed as apps.
bool is_internal(const Props& props)
{
    const auto node = get(props, "node.name");
    return node.starts_with(kPrefix)               // one of ours
           || props.contains("wavemix.link")       // our loopbacks are tagged
           || node.find("loopback") != std::string_view::npos
           || get(props, "media.name").starts_with("loopback");
}

// Averages the cube roots of the channel gains (PulseAudio's cubic scale).
double slider_from_gains(std::span<const float> channel_gains)
{
    if (channel_gains.empty())
        return 0.0;
    double sum = 0.0;
    for (float gain : channel_gains)
        sum += std::cbrt(static_cast<double>(std::max(gain, 0.0F)));
    return sum / static_cast<double>(channel_gains.size());
}

// Clamps the slider and cubes it to get the linear gain.
float gain_from_slider(double slider)
{
    const double s = clamp_volume(slider);
    return static_cast<float>(s * s * s);
}

// Limits a slider value to 0..kMaxVolume, treating NaN as 0.
double clamp_volume(double slider)
{
    if (std::isnan(slider))
        return 0.0;
    return std::clamp(slider, 0.0, kMaxVolume);
}

// Rounds to 4 decimal places.
double round4(double value)
{
    return std::round(value * 10000.0) / 10000.0;
}

}  // namespace wavemix::core
