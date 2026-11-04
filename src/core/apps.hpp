// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Recognising apps from a playback stream's properties, and the volume scale.
//
// These work on a plain property map so they can be unit-tested without PipeWire.
// The properties are the standard ones apps set on their streams (application.name,
// application.process.binary, media.name, ...). See docs/contract/get-state.md#app.

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace wavemix::core {

// A node's properties. std::less<> allows lookups with string_view without a copy.
using Props = std::map<std::string, std::string, std::less<>>;

// What a route is remembered by: "binary:<application.process.binary>", else
// "name:<application.name>", else nothing (that app can be moved, but its route isn't
// remembered).
std::optional<std::string> app_key(const Props& props);

// The name shown to the user: application.name, application.process.binary or
// media.name, whichever comes first, else "Stream <id>".
std::string app_name(const Props& props, unsigned id);

// True for streams that are part of the plumbing (WaveMix's own loopbacks and other
// loopbacks) rather than apps. They're never listed or moved.
bool is_internal(const Props& props);

// ---- Volume ------------------------------------------------------------------------
//
// Slider values use PulseAudio's scale, which is cubic: 1.0 = 100% = unity gain, and
// the actual gain is value³. That's what users have saved in state.json and what the
// window shows, so it stays. PipeWire's channelVolumes property is linear gain, so we
// convert at the boundary:
//
//   slider = average over channels of cbrt(gain)    (what pulse reports)
//   gain   = slider³                                (set on every channel)

inline constexpr double kMaxVolume = 1.5;  // 150%, the most a slider allows

// Turns PipeWire's linear per-channel gains into one slider value (cube-root average).
double slider_from_gains(std::span<const float> channel_gains);
// Turns a slider value into the linear gain to set on every channel (slider³).
float gain_from_slider(double slider);

// Clamps to 0..kMaxVolume.
double clamp_volume(double slider);

// Rounds to 4 decimals, as levels are stored and reported.
double round4(double value);

}  // namespace wavemix::core
