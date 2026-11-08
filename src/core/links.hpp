// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The loopbacks between WaveMix's devices, as the service hosts them.
//
// The devices (the channel sinks, the Personal, Stream and VOD Mixes, WaveMix Mic) are
// pipewire-pulse modules from the login file, always there for apps and OBS. The
// loopbacks connecting them run in the WaveMix service: PipeWire's loopback module,
// loaded into the service's own connection. If the service stops, they go with it, and
// everything stays where it is, quiet, until it's back.
//
//   wavemix_<id> ─┬─ "<id>:personal" ─► wavemix_personal_mix ─ "personal:out" ─► output
//                 ├─ "<id>:stream"   ─► wavemix_stream_mix
//                 └─ "<id>:vod"      ─► wavemix_vod_mix
//   wavemix_mic ──── "mic:monitor"   ─► wavemix_personal_mix
//   mic input ────── "mic:chain"     ─► wavemix_mic_feed (whose monitor is wavemix_mic)
//
// The mic chain is hosted the same way (core/mic.hpp): a filter-chain module instead of
// a loopback, reloaded whenever the chain changes.
//
// Each keeps the names, media.name and wavemix.link tag it had as a pipewire-pulse
// module, so saved levels and anything that looks for them carry on unchanged.
//
// Plain C++ on plain data, so it's unit-tested.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json_file.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"

namespace wavemix::core {

// One module the service hosts: its tag (for a loopback, the wavemix.link property and
// the key for its saved level), the module and its arguments, the two devices it
// connects (node names), which have to exist before it's loaded, and its capture
// node's name, which shows it's running.
struct LinkSpec {
    std::string tag;
    std::string args;
    std::string from;  // a sink (read through its monitor) or a source
    std::string to;    // a sink
    std::string node;  // its capture side
    std::string module = "libpipewire-module-loopback";
    // What to load instead if `module` can't be loaded (none if empty): for the mic chain,
    // a plain loopback, so WaveMix Mic still carries the mic, unfiltered.
    std::string fallback_args;
};

// A channel's loopback from its sink to `destination`.
std::string link_module_args(std::string_view channel_id, Mix mix, std::string_view destination);

// The Personal Mix to the output device.
std::string personal_out_module_args(std::string_view output);

// The mic's Monitor: WaveMix Mic into the Personal Mix.
std::string mic_monitor_module_args();

// Everything the service hosts for `config`, in the order to load it: each channel's
// loopbacks (the VOD one only for channels in the VOD), the Personal Mix's to
// `personal_output` (none without one, or without channels or a mic), and while the mic
// is on, its Monitor and, with a plan (an input to read), its chain. 
std::vector<LinkSpec> wanted_links(const json& config, const std::optional<std::string>& personal_output,
                                   const std::optional<MicPlan>& mic = std::nullopt);

}  // namespace wavemix::core
