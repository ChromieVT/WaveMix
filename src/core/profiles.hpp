// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Profiles: whole setups the user switches between, exports and imports (another PC,
// a template). A profile holds the channels (which are enabled, their names, Personal
// and Stream levels and VOD status), the app routes, the mic (on or off, its input and
// its filters with their order and settings) and the devices picked (the Personal
// output, the mic, and their fallbacks).
//
// config.json keeps them in "profiles", with the name of the one in use in "profile".
// The one in use is the live setup: its entry is brought up to date whenever it's read
// (switching away from it, exporting it), so changes made meanwhile are never lost.
// An exported file is the same object plus "wavemix_profile": the format version.
// See docs/contract/files.md.

#pragma once

#include <optional>
#include <vector>
#include <string>
#include <string_view>

#include "core/json_file.hpp"

namespace wavemix::core {

// The export file format this code writes and reads.
inline constexpr int kProfileFormat = 1;

// The profile a first run is in.
inline constexpr std::string_view kDefaultProfile = "Default";

// Longest profile name, in characters (UTF-8 bytes, to keep it simple).
inline constexpr size_t kMaxProfileName = 40;

// Whether a channel (an entry of config "channels") is enabled: it has its sink and
// loopbacks. Missing means true. A disabled channel keeps its place, name, levels and
// app routes.
bool channel_enabled(const json& channel);

// The config with only the enabled channels: what the plumbing, the loopbacks and the
// login file are made from.
json running_config(const json& config);

// The live setup as a profile called `name`.
json snapshot_profile(const json& config, const json& state, std::string_view name);

// Puts `profile` in place of the live setup: config's channels, assignments, mic and
// device settings, and state's channel levels. Other settings stay as they are.
void apply_profile(json& config, json& state, const json& profile);

// A profile as stored or imported, cleaned up: channels with bad or repeated IDs
// dropped, levels clamped, unknown parts left out. nullopt if it isn't a profile at all
// (not an object, or an export from a newer format).
std::optional<json> clean_profile(const json& raw);

// The name trimmed, or "" if it can't be used: empty, too long or with control
// characters.
std::string clean_profile_name(std::string_view name);

// `wanted`, or "wanted (2)", "wanted (3)"... if config already has a profile by that
// name (compared without case).
std::string unique_profile_name(const json& config, std::string_view wanted);

// The stored profile called `name` (compared without case), or nullptr.
json* find_profile(json& config, std::string_view name);
const json* find_profile(const json& config, std::string_view name);

// The name of the profile in use. Every config has one: "Default" until another is
// made.
std::string active_profile(const json& config);

// Makes sure config has a "profiles" list with the profile in use in it (a first run,
// or a config from before profiles).
void ensure_profiles(json& config, const json& state);

// For an import on another PC: devices this one doesn't have are dropped, so they
// follow the system default (the output and mic) or are left unset (the fallbacks).
void match_devices(json& profile, const std::vector<std::string>& outputs, const std::vector<std::string>& inputs);

}  // namespace wavemix::core
