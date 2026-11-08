// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// WaveMix's devices, hosted by pipewire-pulse: the channel sinks, the Personal, Stream
// and VOD Mixes, and WaveMix Mic with the feed sink the service's mic chain plays into. They're created in two ways that produce identical
// objects:
//   - at login, by pipewire-pulse itself, from the login file (core/login_conf);
//   - live, by this class, when the user adds a channel or something is missing.
// Because pipewire-pulse owns them either way, apps and OBS always find them, whether
// or not the WaveMix service is running. The loopbacks between them are the service's
// (plumbing/service_links.hpp).
//
// This class only creates and removes devices. It reads the current state from the
// PipeWire registry, and uses libpulse (PulseControl) to load and unload modules.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json_file.hpp"
#include "core/links.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"
#include "plumbing/pulse_control.hpp"
#include "pw/connection.hpp"
#include "pw/registry.hpp"

namespace wavemix::plumbing {

class PulsePlumbing {
public:
    // Keeps references to the connection, the registry and the libpulse client; owns nothing.
    PulsePlumbing(pw::Connection& connection, pw::Registry& registry, PulseControl& pulse);

    // Creates the sinks `config` needs that don't exist yet: the Stream and VOD Mixes
    // and each channel's. Never touches anything that already exists, so it's safe to
    // call any time, for example after every PipeWire restart. Returns a line per thing
    // it created.
    std::vector<std::string> ensure(const core::json& config);

    // The Personal Mix (what you hear of the channels and the mic's Monitor), if it's
    // missing. Returns a line if it created it.
    std::vector<std::string> ensure_personal();

    // Removes the Personal Mix: nothing plays into it any more.
    void remove_personal();

    // Removes a channel's sink. The Stream and VOD Mixes stay, even after the last
    // channel: OBS records them.
    void remove_channel(std::string_view channel_id);

    // WaveMix Mic (core/mic.hpp): the feed sink the service's chain plays into, and
    // WaveMix Mic reading its monitor, if they're missing. Returns a line per thing it
    // created.
    std::vector<std::string> ensure_mic();

    // Removes WaveMix Mic (the mic was turned off).
    void remove_mic();

    // Removes the VOD Mix sink: the VOD Mix was turned off.
    void remove_vod();

    // Unloads every loopback of ours that pipewire-pulse runs: left from a WaveMix that
    // could run the channels' loopbacks there, so they'd play twice next to the
    // service's. Returns a line per loopback removed.
    std::vector<std::string> remove_loopbacks();

private:
    // Re-reads the graph until `ready` is true for it, or ~2 seconds pass. Needed after
    // loading a module: pipewire-pulse creates the node, and the registry hears about
    // it a moment later.
    template <typename Predicate>
    pw::Graph wait_for(Predicate ready);

    // Unloads every module whose argument string contains one of `needles`.
    void unload_matching(const std::vector<std::string>& needles);

    pw::Connection& connection_;
    pw::Registry& registry_;
    PulseControl& pulse_;
};

}  // namespace wavemix::plumbing
