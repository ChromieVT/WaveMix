// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The login file: ~/.config/pipewire/pipewire-pulse.conf.d/60-wavemix.conf
//
// This file is what keeps WaveMix's devices there without WaveMix. When pipewire-pulse
// starts (at login), it runs the "pulse.cmd" list in this file and creates every
// channel sink, the mixes and WaveMix Mic by itself, so apps and OBS always find
// them. The loopbacks between them run in the WaveMix service: while it isn't running,
// everything stays where it is, quiet.
//
// The service creates the same modules live, with the same arguments, so an object
// created either way looks identical and can be removed the same way.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json_file.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"

namespace wavemix::core {

// Ordered key/value pairs: the order is kept in the generated arguments.
using PropList = std::vector<std::pair<std::string, std::string>>;

// Formats properties for a pulse module argument: key=value pairs separated by spaces.
// Values with a space, and empty values, are single-quoted. The format has no escaping,
// so quote characters in values are removed.
std::string pa_props(const PropList& props);

// Arguments for module-null-sink: a channel's sink, or the Stream Mix (channel_id "").
std::string sink_args(std::string_view sink_name, std::string_view description,
                      std::string_view channel_id);

// The complete file for `config`: the devices, WaveMix Mic last while the mic is on.
// The loopbacks between them and the mic chain are the service's (core/links.hpp).
std::string render_login_conf(const json& config);

// Writes the login file (atomically), if it's changed. Throws std::system_error on
// failure.
void write_login_conf(const Paths& paths, const json& config);

}  // namespace wavemix::core
