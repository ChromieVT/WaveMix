// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The requests WaveMix makes to pipewire-pulse: loading and unloading modules, moving
// streams, and choosing the default output.
//
// Why libpulse at all, in an otherwise native PipeWire app? Two reasons:
//
// - The channels are pipewire-pulse modules (module-null-sink + module-loopback),
//   created at login from the login file so that audio never depends on WaveMix (see
//   docs/ROADMAP.md#design-rules). Adding or removing a channel *live* means loading
//   and unloading the very same modules, and only the PulseAudio protocol can ask
//   pipewire-pulse for that.
//
// - Moving a stream and changing the default output both write PipeWire's "default"
//   metadata. Inside a Flatpak, WirePlumber gives WaveMix's own PipeWire connection
//   read-only access to that metadata, and silently drops the writes. pipewire-pulse
//   makes the same changes with its own rights, after checking that the Flatpak may
//   (it has device access), exactly as for pavucontrol or `pactl move-sink-input`.
//
// Everything else (finding nodes, levels, mute) uses libpipewire.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_operation;

namespace wavemix::plumbing {

struct PulseModule {
    uint32_t index = 0;
    std::string name;      // "module-null-sink", "module-loopback", ...
    std::string argument;  // the argument string it was loaded with
};

class PulseControl {
public:
    // Connects to pipewire-pulse, and again later if it restarts. Throws
    // std::runtime_error if it isn't running.
    explicit PulseControl(const std::string& client_name);
    // Disconnects from pipewire-pulse and stops its loop thread.
    ~PulseControl();

    // Not copyable (copy constructor and assignment deleted): it owns the libpulse connection.
    PulseControl(const PulseControl&) = delete;
    PulseControl& operator=(const PulseControl&) = delete;

    // Loads a module and returns its index. Throws std::runtime_error if it fails
    // (for example a loopback whose target sink doesn't exist).
    uint32_t load(const std::string& name, const std::string& arguments);

    // Unloads a module. Returns false if it was already gone, which is normal: a
    // loopback disappears by itself when the sink it reads from is unloaded.
    bool unload(uint32_t index);

    // Every module currently loaded.
    std::vector<PulseModule> list();

    // Moves a playing stream to a sink. `stream` is the stream's PipeWire object.serial,
    // which is pipewire-pulse's sink-input index. The sink must exist: WirePlumber
    // destroys a stream with node.dont-fallback (our Personal loopbacks) whose target
    // can't be found. Returns false if pipewire-pulse refused (the stream is gone, say).
    bool move_stream(uint32_t stream, const std::string& sink_name);

private:
    struct Callbacks;  // the C callbacks; defined in pulse_control.cpp

    // Connects a new context, and waits until it's ready. Called with the loop lock
    // held. Throws std::runtime_error if pipewire-pulse isn't there.
    void connect();

    // Every request starts with this (with the lock held): if pipewire-pulse restarted
    // since, the old connection is dead, so it connects again.
    void reconnect_if_lost();

    // Waits until the operation finishes. Called with the loop lock held.
    void wait(pa_operation* operation);

    std::string client_name_;
    pa_threaded_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
};

}  // namespace wavemix::plumbing
