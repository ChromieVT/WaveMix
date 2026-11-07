// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The mixer: everything the daemon does, minus D-Bus.
//
// It owns the channels (config.json), remembers levels (state.json), keeps PipeWire
// matching the config (reconcile), routes apps into channels, and answers GetState.
// The D-Bus service (next step) will be a thin layer that turns method calls into
// calls on this class; until then the wavemix-mixer development tool drives it.
//
// Threading: use it from one thread only. PipeWire events arrive on PipeWire's own
// thread; the registry just raises a flag, and the owner's loop calls poll() to act
// on it. That keeps every decision on one thread, with no locks in the logic.
//
// Per channel, as in docs/contract/files.md#names:
//
//   wavemix_<id>  (apps play here)
//     ├─ loopback "<id>:personal" ─► personal output (hardware)  = Personal slider
//     ├─ loopback "<id>:stream"   ─► wavemix_stream_mix ─► OBS   = Stream slider
//     └─ loopback "<id>:vod"      ─► wavemix_vod_mix    ─► OBS   = follows Stream,
//                                                                  if "in the VOD"

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/app_icons.hpp"
#include "core/json_file.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"
#include "pw/graph.hpp"

namespace wavemix::pw {
class Connection;
class Registry;
}  // namespace wavemix::pw

namespace wavemix::plumbing {
class PulseControl;
class PulsePlumbing;
}  // namespace wavemix::plumbing

namespace wavemix::mixer {

// What a client gets back when a request can't be done. The D-Bus layer turns the
// kind into the error name from docs/contract/dbus.md#errors.
class MixerError : public std::runtime_error {
public:
    enum class Kind { Failed, Disconnected };
    // An error of `kind` with a message for the user.
    MixerError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    // Which D-Bus error this becomes.
    Kind kind() const { return kind_; }

private:
    Kind kind_;
};

class Mixer {
public:
    // Loads config.json and state.json. Doesn't connect yet: call connect().
    explicit Mixer(core::Paths paths, std::string version);
    // Lets go of PipeWire (if connected) without saving; call shutdown() first to save.
    ~Mixer();

    // Not copyable (copy constructor and assignment deleted): it owns the PipeWire session.
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // ---- Connection ------------------------------------------------------------------

    // Connects to PipeWire and pipewire-pulse, then brings everything in line with the
    // config (startup sync). Returns false if they aren't reachable yet; call it again
    // later. While disconnected, GetState still answers and other requests fail.
    bool connect();
    // True while there's a PipeWire session.
    bool connected() const { return session_ != nullptr; }

    // Call regularly (every ~50 ms) and whenever the graph-changed flag is set. Reacts to
    // PipeWire events once they've settled, saves levels that are due, and notices a
    // lost connection. Returns true if anything a client shows may have changed.
    bool poll();

    // Saves levels and lets go of PipeWire, leaving every channel in place. This is
    // what Quit and Restart do first.
    void shutdown();

    // ---- The D-Bus methods (docs/contract/dbus.md) -----------------------------------

    // GetState: everything a client shows, as JSON.
    core::json state();
    // SetVolume: a channel's Personal or Stream slider, or a mix's master (channel "").
    void set_volume(std::string_view channel, std::string_view mix, double volume);
    // SetMute: mutes or unmutes a channel's slider, or a mix's master (channel "").
    void set_mute(std::string_view channel, std::string_view mix, bool muted);
    // AddChannel: creates a channel (sink and loopbacks) and returns its new ID.
    std::string add_channel(std::string_view label, std::string_view icon);
    // RemoveChannel: moves its apps out, then removes the channel and its saved levels.
    void remove_channel(std::string_view id);
    // RenameChannel: changes a channel's label and/or icon.
    void rename_channel(std::string_view id, std::string_view label, std::string_view icon);
    // MoveChannel: moves a channel to `position` in the list.
    void move_channel(std::string_view id, int position);
    // AssignApp: routes a stream's app into a channel and remembers it ("" un-assigns).
    void assign_app(uint64_t stream, std::string_view channel);
    // ForgetAssignment: drops the remembered route for an app key.
    void forget_assignment(std::string_view app_key);
    // SetInVod: puts a channel in the VOD Mix or leaves it out.
    void set_in_vod(std::string_view channel, bool in_vod);
    // SetChannelEnabled: turns a channel on or off; a disabled one keeps its place, name,
    // levels and app routes, but has no sink or loopbacks.
    void set_channel_enabled(std::string_view channel, bool enabled);
    // SetSetting: changes one of config.json's settings.
    void set_setting(std::string_view key, const core::json& value);
    // Back to a first run: no channels or routes, the default settings, an empty mic
    // chain, and every level that's left at 100% (the Monitor muted).
    void reset();

    // The mic (added in 0.1.6). Filter positions count from 0, in the user's list.

    // SetMicInput: which input the mic chain reads ("default" or a source name).
    void set_mic_input(std::string_view device);
    // SetMicEnabled: creates or removes WaveMix Mic and its chain.
    void set_mic_enabled(bool enabled);
    // SetMicVolume: the "mic" (WaveMix Mic) or "monitor" slider.
    void set_mic_volume(std::string_view slider, double volume);
    // SetMicMute: mutes or unmutes the "mic" or "monitor" slider.
    void set_mic_mute(std::string_view slider, bool muted);
    // AddMicFilter: inserts a filter (-1: at the end) and returns where it went.
    int add_mic_filter(std::string_view filter, int position);
    // RemoveMicFilter: removes the filter at `position`.
    void remove_mic_filter(int position);
    // MoveMicFilter: moves the filter at `from` to `to`.
    void move_mic_filter(int from, int to);
    // SetMicFilterOn: switches a filter on or off.
    void set_mic_filter_on(int position, bool on);
    // SetMicFilterValue: changes one setting of a filter, live.
    void set_mic_filter_value(int position, std::string_view key, double value);
    // GetMeters: the meters' peaks since the last call (starting them if needed).
    core::json meters();

    // Profiles (core/profiles.hpp). Names are matched without case.
    // SwitchProfile: saves the live setup into its profile, then switches to `name`.
    void switch_profile(std::string_view name);
    // SaveProfileAs: the live setup, as a new profile in use.
    void save_profile_as(std::string_view name);
    // RenameProfile: renames a stored profile.
    void rename_profile(std::string_view name, std::string_view new_name);
    // DeleteProfile: deletes a profile; for the one in use, switches to another first.
    void delete_profile(std::string_view name);
    // ExportProfile: the export file's text.
    std::string export_profile(std::string_view name);
    // ImportProfile: stores a profile from a file's text and returns the name it was given.
    std::string import_profile(std::string_view text);

private:
    struct Session;  // everything that only exists while connected

    // ---- Keeping PipeWire in line with the config -----------------------------------
    // Creates what the config needs, routes assigned apps and writes the login file.
    void startup_sync();
    // Creates missing devices and makes the loopbacks, mic chain and levels match the config.
    void reconcile();
    // Creates or removes WaveMix Mic's devices, following the "enabled" flag.
    void reconcile_mic();
    // Keeps pipewire-pulse's loopbacks in line with the "pulse_routing" setting.
    void sync_pulse_links();
    // Loads, reloads or unloads the service's loopbacks to match the config.
    void sync_links();
    // The input the mic chain should read, or nothing if there's none.
    std::optional<std::string> resolve_mic_input(const pw::Graph& graph) const;
    // The mic chain to build now, or nothing when the mic is off or has no input.
    std::optional<core::MicPlan> mic_plan(const pw::Graph& graph) const;
    // Applies an edit to the mic chain (turning bad input into MixerError), then rebuilds.
    void edit_mic_chain(const std::function<void()>& edit);
    // Sends the filters' settings to the mic chain's node when it's new or they changed.
    void apply_mic_controls(const pw::Graph& graph);
    // The "mic" part of GetState.
    core::json mic_state(const pw::Graph& graph) const;
    // Moves assigned apps' streams (all, or just one) into their channels.
    void route_apps(std::optional<uint64_t> only_stream = std::nullopt);
    // Sends apps out of channels they aren't routed to.
    void unroute_apps();
    // Moves a channel's apps to a real output, except the app keys in `keep`.
    void move_apps_out(std::string_view channel, const std::set<std::string>& keep = {});
    // Brings the profile in use up to date with the live setup.
    void refresh_profile();
    // The stored profile called `name`; throws if there's none.
    const core::json& profile_named(std::string_view name);
    // Rewrites the login file, logging rather than throwing on failure.
    void write_login_conf();

    // ---- Levels ------------------------------------------------------------------------
    // True while a loopback is in its grace period (see born_).
    bool is_fresh(const std::string& tag, const pw::Node& stream);
    // True if any loopback is still in its grace period.
    bool has_fresh_links() const;
    // Puts saved levels back on loopbacks still in their grace period.
    void apply_saved_levels();
    // The tags of every slider whose level is saved.
    std::vector<std::string> level_tags() const;
    // Gives each VOD loopback its channel's Stream level and mute.
    void sync_vod_levels();
    // Remembers a channel slider's new level (not saved yet: see save_at_).
    void remember(std::string_view channel, core::Mix mix, std::optional<double> volume,
                  std::optional<bool> muted);
    // Remembers a level by its tag.
    void remember_level(const std::string& tag, std::optional<double> volume, std::optional<bool> muted);
    // The node a saved level belongs to: a loopback, or WaveMix Mic itself.
    const pw::Node* level_stream(const pw::Graph& graph, const std::string& tag) const;
    // The saved-level tag for a mic slider name; throws for an unknown one.
    std::string mic_level_tag(std::string_view slider) const;
    // Copies the current levels from PipeWire into state.json.
    void snapshot_levels();
    // The node behind a slider: a loopback, or a mix's own sink for channel "".
    const pw::Node* level_node(const pw::Graph& graph, std::string_view channel, core::Mix mix) const;
    // Checks a channel/mix pair from a client and returns the mix; throws if invalid.
    core::Mix check_level_target(std::string_view channel, std::string_view mix);
    // The VOD loopback that follows a channel's Stream slider, or nullptr.
    const pw::Node* vod_follower(const pw::Graph& graph, std::string_view channel, core::Mix mix) const;

    // ---- Helpers ----------------------------------------------------------------------
    // A fresh snapshot of the graph (syncs with PipeWire first).
    pw::Graph graph();
    // Where the Personal Mix plays: the picked output (or nothing if it's gone), else the
    // default if it's a real device, else the last real default, else the first device.
    std::optional<std::string> resolve_personal_output(const pw::Graph& graph);
    // The config entry for a channel, or nullptr.
    core::json* channel(std::string_view id);
    // Every app stream, as GetState lists them.
    core::json apps(const pw::Graph& graph);
    // An app's icon name or path, from its menu entry (cached).
    std::string app_icon(const core::Props& props);
    // Moves a stream to a sink through pipewire-pulse.
    void move_stream(const pw::Node& stream, const pw::Node& sink);
    // Throws MixerError if there's no PipeWire session.
    void require_connection() const;
    // Writes config.json.
    void save_config();
    // Writes state.json.
    void save_state();
    // Drops the session after PipeWire went away.
    void lost_connection(const std::exception& error);

    core::Paths paths_;
    std::string version_;
    core::json config_;
    core::json state_;
    std::unique_ptr<Session> session_;
    std::atomic<bool> pending_events_{false};

    using Clock = std::chrono::steady_clock;

    // WirePlumber resets a new loopback's volume shortly after it appears. For a grace
    // period after a link shows up we keep re-applying our saved level, and don't save
    // whatever level it has. tag -> (serial of the stream we saw, when we first saw it)
    std::map<std::string, std::pair<uint64_t, Clock::time_point>> born_;
    static constexpr auto kGrace = std::chrono::seconds(4);

    // The controls last set on the mic chain's node: (serial of the node they went to, the
    // values). Set again when the node is new or the values change.
    std::pair<uint64_t, core::MicPortValues> mic_controls_set_;

    // When a client last asked for the meters: they stop a few seconds after.
    std::optional<Clock::time_point> meters_asked_;
    static constexpr auto kMetersLinger = std::chrono::seconds(3);

    bool levels_dirty_ = false;                 // slider changes not yet in state.json
    std::optional<Clock::time_point> save_at_;  // when to write state.json next
    std::optional<Clock::time_point> settle_at_;  // events are handled once this passes
    pw::Graph last_graph_;                        // what poll() last saw, to spot new streams

    // Apps' icons, from their menu entries (core/app_icons.hpp). The entries are read
    // again when an app's icon isn't found, at most every kEntriesMaxAge.
    std::vector<core::DesktopEntry> desktop_entries_;
    std::optional<Clock::time_point> entries_loaded_;
    std::map<std::string, std::string> icons_;  // app -> icon
    static constexpr auto kEntriesMaxAge = std::chrono::seconds(30);
};

}  // namespace wavemix::mixer
