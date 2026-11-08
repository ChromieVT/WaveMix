// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// config.json (what the user set up) and state.json (what WaveMix remembers).
//
// Both are kept as JSON documents rather than C++ structs on purpose: unknown keys
// must survive a load/save round trip (a newer version may have written them), and
// "settings" is passed to clients exactly as stored.

#pragma once

#include "core/json_file.hpp"
#include "core/paths.hpp"

namespace wavemix::core {

// The config schema version this code writes, stored as "version" in config.json so a
// future format change can recognise and convert older files.
inline constexpr int kConfigVersion = 2;

// The config a first run starts with: no channels, no routes, default settings.
json default_config();

// The state a first run starts with: no saved levels.
json default_state();

// Loads config.json, falling back to defaults for anything missing or broken.
// Channels with a missing or invalid ID are dropped.
json load_config(const Paths& paths);

// Writes config.json atomically.
void save_config(const Paths& paths, const json& config);

// Whether a channel (an entry of config "channels") is in the VOD Mix. Stored as
// "in_vod"; channels without it, including every channel from before the VOD Mix
// existed, are in it.
bool in_vod(const json& channel);

// Whether the VOD Mix is used at all (settings "vod_enabled", off by default). Off,
// there's no VOD Mix sink and no VOD loopbacks; channels keep their in_vod for when
// it's turned on again.
bool vod_enabled(const json& config);

// Whether this channel should have a VOD loopback: the VOD Mix is on and the channel
// is in it.
bool wants_vod_link(const json& config, const json& channel);

// Loads state.json. A "levels" value that isn't an object is reset to {}.
json load_state(const Paths& paths);

// Writes state.json atomically.
void save_state(const Paths& paths, const json& state);

}  // namespace wavemix::core
