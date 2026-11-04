// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "mixer/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "core/app_icons.hpp"
#include "core/apps.hpp"
#include "core/config.hpp"
#include "core/ids.hpp"
#include "core/log.hpp"
#include "core/links.hpp"
#include "core/login_conf.hpp"
#include "plumbing/pulse_control.hpp"
#include "plumbing/meters.hpp"
#include "plumbing/pulse_plumbing.hpp"
#include "plumbing/service_links.hpp"
#include "pw/connection.hpp"
#include "pw/registry.hpp"

namespace wavemix::mixer {

using core::json;
using namespace std::chrono_literals;

namespace {

// The mixer's lines in the log (core/log.hpp).
void log(const std::string& message)
{
    core::log::info("mixer", message);
}

// Writes a warning line for the mixer.
void log_warning(const std::string& message)
{
    core::log::warning("mixer", message);
}

// Writes an error line for the mixer.
void log_error(const std::string& message)
{
    core::log::error("mixer", message);
}

// object[key] if it's a string, else "".
std::string string_field(const json& object, const std::string& key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Strips whitespace (spaces, tabs, newlines) from both ends.
std::string trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos)
        return "";
    return std::string(text.substr(first, text.find_last_not_of(" \t\n\r") - first + 1));
}

// Lowercases ASCII letters.
std::string lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A MixerError of kind Failed with `message`, ready to throw.
MixerError failed(const std::string& message)
{
    return MixerError(MixerError::Kind::Failed, message);
}

// A slider as GetState shows it (docs/contract/get-state.md#level).
json level_json(const pw::Node* node)
{
    if (!node)
        return json{{"present", false}, {"volume", 0.0}, {"muted", false}};
    return json{{"present", true}, {"volume", core::round4(node->slider().value_or(0.0))}, {"muted", node->muted}};
}

// Debounce delays: long enough to batch a burst of events, short enough to feel instant.
constexpr auto kSettle = 150ms;      // let a burst of PipeWire events finish first
constexpr auto kSaveDelay = 1000ms;  // write levels a second after the last change
constexpr auto kSaveRetry = 1500ms;  // ...or again later if links are still settling

}  // namespace

// Everything that only exists while connected. The order matters: members are
// destroyed bottom-up, so plumbing goes before the connections it uses.
struct Mixer::Session {
    std::unique_ptr<pw::Connection> connection;
    std::unique_ptr<pw::Registry> registry;
    std::unique_ptr<plumbing::PulseControl> pulse;  // module loading, moving streams, default output
    std::unique_ptr<plumbing::PulsePlumbing> plumbing;  // the devices, in pipewire-pulse
    std::unique_ptr<plumbing::ServiceLinks> links;      // the loopbacks, in this process
    std::unique_ptr<plumbing::Meters> meters;           // the mic's meters, while asked for
    bool pulse_links_cleared = false;  // none of the channels' loopbacks left in pipewire-pulse
};

// Loads config.json and state.json; PipeWire comes later, in connect().
Mixer::Mixer(core::Paths paths, std::string version)
    : paths_(std::move(paths)),
      version_(std::move(version)),
      config_(core::load_config(paths_)),
      state_(core::load_state(paths_))
{
}

// Tears down the session (if any) and everything else in member order.
Mixer::~Mixer() = default;

// =====================================================================================
// Connection
// =====================================================================================

// Builds a session (PipeWire, the registry, libpulse and the plumbing on top) and runs
// the startup sync. False, with no session, if PipeWire isn't reachable or fails meanwhile.
bool Mixer::connect()
{
    try {
        // The loopbacks between the devices run in this process.
        static const long file_limit = pw::raise_open_file_limit();
        static_cast<void>(file_limit);
        auto session = std::make_unique<Session>();
        session->connection = std::make_unique<pw::Connection>("wavemix-daemon");
        session->registry = std::make_unique<pw::Registry>(*session->connection);
        // Runs on PipeWire's thread: only raise the flag, poll() does the work.
        session->registry->on_changed([this] { pending_events_ = true; });
        session->connection->sync();  // every object announced...
        session->connection->sync();  // ...and their details received
        session->pulse = std::make_unique<plumbing::PulseControl>("wavemix-daemon");
        session->plumbing = std::make_unique<plumbing::PulsePlumbing>(
            *session->connection, *session->registry, *session->pulse);
        session->links = std::make_unique<plumbing::ServiceLinks>(*session->connection);
        session->meters = std::make_unique<plumbing::Meters>(*session->connection);
        session_ = std::move(session);
    } catch (const std::exception& e) {
        log_warning(std::string("PipeWire not reachable yet: ") + e.what());
        session_.reset();
        return false;
    }

    log("connected to PipeWire");
    born_.clear();
    try {
        startup_sync();
        if (session_)
            last_graph_ = graph();
    } catch (const std::exception& e) {
        lost_connection(e);
        return false;
    }
    return session_ != nullptr;
}

// Brings PipeWire in line with the config, routes assigned apps and rewrites the login file.
void Mixer::startup_sync()
{
    reconcile();
    route_apps();
    write_login_conf();
}

// Drops the session after the connection failed, and forgets the timers that depend on it.
void Mixer::lost_connection(const std::exception& error)
{
    if (!session_)
        return;
    log_warning(std::string("lost PipeWire connection: ") + error.what());
    session_.reset();
    born_.clear();
    settle_at_.reset();
    save_at_.reset();
}

// Saves the current levels and closes the session; the devices stay in pipewire-pulse.
void Mixer::shutdown()
{
    if (!session_)
        return;
    try {
        snapshot_levels();
    } catch (const std::exception& e) {
        log_error(std::string("could not save levels: ") + e.what());
    }
    // The devices are pipewire-pulse modules, so they stay as they are: apps stay in
    // their channels and OBS on its devices. The loopbacks go with this process, so
    // they're quiet until the service runs again.
    session_.reset();
}

// Handles settled PipeWire events (routing new apps, reconciling when devices or
// defaults changed, reapplying levels), saves levels when due, and stops idle meters.
bool Mixer::poll()
{
    if (!session_)
        return false;
    if (!session_->connection->connected()) {
        lost_connection(std::runtime_error("the server went away"));
        return true;
    }

    const auto now = Clock::now();
    if (pending_events_.exchange(false))
        settle_at_ = now + kSettle;  // restart the wait with every new event

    bool changed = false;
    if (meters_asked_ && now - *meters_asked_ > kMetersLinger) {
        meters_asked_.reset();
        session_->meters->stop();  // nobody's looking: let the mic rest
    }
    try {
        if (settle_at_ && now >= *settle_at_) {
            settle_at_.reset();
            const pw::Graph before = std::move(last_graph_);
            const pw::Graph now_graph = graph();

            // New apps: route them if their app has a remembered channel.
            for (const auto& [id, node] : now_graph.nodes) {
                if (node.is_stream() && !before.nodes.contains(id) && !core::is_internal(node.props))
                    route_apps(node.serial());
            }

            // Sinks or sources came or went, one of our own nodes went (a loopback, or
            // the mic chain, whose device disappeared), or the default output changed:
            // the Personal side follows the default, and missing plumbing gets recreated.
            const auto plumbing = [](const pw::Graph& graph) {
                std::set<std::string> names;
                for (const auto& [id, node] : graph.nodes) {
                    if (node.is_sink() || node.is_source() || node.name().starts_with(core::kPrefix))
                        names.insert(node.name());
                    if (const auto tag = node.props.find("wavemix.link"); tag != node.props.end())
                        names.insert("link " + tag->second);
                }
                return names;
            };
            if (plumbing(before) != plumbing(now_graph) || before.default_sink != now_graph.default_sink ||
                before.default_source != now_graph.default_source)
                reconcile();

            // New loopbacks get their saved levels back (no-op outside the grace period),
            // the VOD loopbacks catch up with Stream levels changed anywhere else, and a
            // new mic chain node gets the filters' settings.
            apply_saved_levels();
            apply_mic_controls(now_graph);
            sync_vod_levels();

            // Levels may have changed (in pavucontrol, say): save them a bit later.
            if (!save_at_)
                save_at_ = now + kSaveDelay;
            last_graph_ = graph();
            changed = true;
        }
        if (save_at_ && now >= *save_at_) {
            save_at_.reset();
            snapshot_levels();
            if (has_fresh_links())
                save_at_ = now + kSaveRetry;  // try again once they've settled
        }
    } catch (const MixerError& e) {
        if (e.kind() == MixerError::Kind::Disconnected)
            lost_connection(e);
        else
            log_error(e.what());
        changed = true;
    } catch (const std::exception& e) {
        // Never let one bad event stop the daemon.
        log_error(e.what());
    }
    return changed;
}

// Syncs with PipeWire and returns a copy of the graph; throws Disconnected if it's gone.
pw::Graph Mixer::graph()
{
    require_connection();
    if (!session_->connection->sync()) {
        // Don't destroy the session here: the caller may still be using parts of it.
        // poll() notices the dead connection next time round.
        throw MixerError(MixerError::Kind::Disconnected, "lost the connection to PipeWire");
    }
    return session_->registry->snapshot();
}

// Throws a Failed MixerError while there's no PipeWire session.
void Mixer::require_connection() const
{
    if (!session_)
        throw failed("not connected to PipeWire");
}

// =====================================================================================
// Keeping PipeWire in line with the config
// =====================================================================================

// Creates the devices the config needs, then the loopbacks, the mic chain and their levels.
void Mixer::reconcile()
{
    // The devices first (pipewire-pulse), then what runs between them in this process:
    // the loopbacks and the mic chain. The Personal Mix exists while anything plays into
    // it: a channel, or the mic's Monitor.
    const bool personal_mix = !config_["channels"].empty() || core::mic_enabled(config_);
    if (personal_mix) {
        for (const auto& line : session_->plumbing->ensure_personal())
            log("created " + line);
    } else {
        session_->plumbing->remove_personal();
    }
    for (const auto& line : session_->plumbing->ensure(config_))
        log("created " + line);
    reconcile_mic();
    sync_pulse_links();
    sync_links();
    apply_mic_controls(graph());
    apply_saved_levels();
    sync_vod_levels();
}

// Makes the service's loopbacks (and mic chain) match the config.
void Mixer::sync_links()
{
    // Loads what's missing, follows a change of output device or of the mic chain, and
    // unloads what the config no longer has (a removed channel, the VOD Mix turned off,
    // the mic off).
    const pw::Graph g = graph();
    const auto wanted = core::wanted_links(config_, resolve_personal_output(g), mic_plan(g));
    for (const auto& line : session_->links->sync(wanted, g))
        log(line);
}

// Keeps pipewire-pulse's loopbacks in line with "pulse_routing" (see the comment below).
void Mixer::sync_pulse_links()
{
    // With "pulse_routing" on, pipewire-pulse runs the channels' loopbacks and the
    // Personal Mix's to the output, and the login file has them, for the output device of
    // the moment. Off, this clears out any there (from before it was turned off), once
    // per connection: nothing else puts them there.
    const bool on = core::pulse_routing(config_);
    if (!on && session_->pulse_links_cleared)
        return;
    const pw::Graph g = graph();
    const auto output = resolve_personal_output(g);
    for (const auto& line : session_->plumbing->sync_loopbacks(core::wanted_pulse_links(config_, output)))
        log(line);
    session_->pulse_links_cleared = !on;
    if (on)
        write_login_conf();
}

// Creates WaveMix Mic's devices while the mic is on, and removes them when it's off.
void Mixer::reconcile_mic()
{
    if (!core::mic_enabled(config_)) {
        session_->plumbing->remove_mic();
        return;
    }
    for (const auto& line : session_->plumbing->ensure_mic())
        log("created " + line);
}

// Sends every filter's settings to the mic chain's node, when it's new or they changed.
void Mixer::apply_mic_controls(const pw::Graph& g)
{
    // Every filter's settings, set on the chain's node (its capture side, where the
    // filter-chain keeps the controls, named "s<position>:<port>"). A new node starts on
    // the plugins' defaults, so it gets every value as soon as it appears: after the
    // chain changed, or when the service starts.
    const auto plan = mic_plan(g);
    if (!plan || plan->stages.empty())
        return;
    const pw::Node* node = g.node_named(std::string(core::kMicChainNode) + "_in");
    if (!node)
        return;  // not there yet: the next poll catches it

    const auto steps = core::mic_chain(config_);
    core::MicPortValues values;
    for (const auto& stage : plan->stages)
        for (auto& [port, value] : core::mic_port_values(steps.at(stage.position - 1)))
            values.emplace_back(stage.node() + ":" + port, value);
    if (mic_controls_set_.first == node->serial() && mic_controls_set_.second == values)
        return;
    if (session_->registry->set_controls(node->id, values))
        mic_controls_set_ = {node->serial(), std::move(values)};
}

// The input the mic chain should read: the picked device, fallback device, the default, or the first one.
std::optional<std::string> Mixer::resolve_mic_input(const pw::Graph& g) const
{
    const std::string setting = core::mic_input_setting(config_);
    const auto inputs = g.hardware_sources();
    const auto present = [&](const std::string& name) {
        return std::any_of(inputs.begin(), inputs.end(), [&](const pw::Node* n) { return n->name() == name; });
    };
    if (setting != "default") {
        if (present(setting))
            return setting;
        const std::string fallback = string_field(config_["settings"], "fallback_input");
        return present(fallback) ? std::optional(fallback) : std::nullopt;
    }
    if (present(g.default_source))
        return g.default_source;
    if (!inputs.empty())
        return inputs.front()->name();
    return std::nullopt;
}

// The chain to build for the current config, input and installed plugins.
std::optional<core::MicPlan> Mixer::mic_plan(const pw::Graph& g) const
{
    return core::plan_mic(config_, resolve_mic_input(g), core::find_mic_plugins());
}

// The device the Personal Mix plays to, by the rules numbered below.
std::optional<std::string> Mixer::resolve_personal_output(const pw::Graph& g)
{
    std::vector<std::string> hardware;
    for (const pw::Node* sink : g.hardware_sinks())
        hardware.push_back(sink->name());
    const auto is_hardware = [&](const std::string& name) {
        return std::find(hardware.begin(), hardware.end(), name) != hardware.end();
    };

    // 1. A device the user picked, when it's there; or else the fallback output they
    // picked, if it's there. Neither: the system default, as below.
    const std::string setting = string_field(config_["settings"], "personal_output");
    if (!setting.empty() && setting != "default") {
        if (is_hardware(setting))
            return setting;
        const std::string fallback = string_field(config_["settings"], "fallback_output");
        if (is_hardware(fallback))
            return fallback;
    }

    // 2. Following the default, and the default is a real device.
    if (is_hardware(g.default_sink)) {
        if (string_field(state_, "last_hardware_default") != g.default_sink) {
            state_["last_hardware_default"] = g.default_sink;
            save_state();
        }
        return g.default_sink;
    }

    // 3. The default is one of ours (picked in the sound settings): the last real default.
    const std::string last = string_field(state_, "last_hardware_default");
    if (is_hardware(last))
        return last;

    // 4. Anything, or nothing.
    if (!hardware.empty())
        return hardware.front();
    return std::nullopt;
}

// Moves a stream to a sink through pipewire-pulse, logging if that's refused.
void Mixer::move_stream(const pw::Node& stream, const pw::Node& sink)
{
    // Through pipewire-pulse, not the native API: see plumbing/pulse_control.hpp.
    if (!session_->pulse->move_stream(static_cast<uint32_t>(stream.serial()), sink.name()))
        log_warning("could not move stream " + std::to_string(stream.serial()) + " to " + sink.name());
}

// Moves each app with a remembered channel (or just `only_stream`) into that channel.
void Mixer::route_apps(std::optional<uint64_t> only_stream)
{
    // Only apps the user assigned are moved. Everything else plays wherever the system
    // sends it, normally the default output from the desktop's sound settings, which
    // WaveMix never changes.
    std::map<std::string, std::string> rules;  // app key -> channel id
    for (const auto& rule : config_["assignments"])
        rules[string_field(rule, "app")] = string_field(rule, "channel");
    if (rules.empty())
        return;

    const pw::Graph g = graph();
    for (const auto& [id, stream] : g.nodes) {
        if (!stream.is_stream() || core::is_internal(stream.props))
            continue;
        if (only_stream && stream.serial() != *only_stream)
            continue;
        const auto key = core::app_key(stream.props);
        if (!key || !rules.contains(*key))
            continue;
        const pw::Node* target = g.sink_named(core::base_sink(rules[*key]));
        if (target && g.sink_of(stream) != target) {
            move_stream(stream, *target);
            log("routed " + core::app_name(stream.props, stream.id) + " -> " + rules[*key]);
        }
    }
}

// Rewrites the login file from the config, logging (not throwing) on failure.
void Mixer::write_login_conf()
{
    // Keep the file at login in step with the config. If this fails, the live devices
    // still work; they just won't come back at the next login.
    try {
        // With "pulse_routing" on, the file has the Personal Mix's loopback to the output.
        std::optional<std::string> output;
        if (session_ && core::pulse_routing(config_))
            output = resolve_personal_output(graph());
        core::write_login_conf(paths_, config_, output);
    } catch (const std::system_error& e) {
        log_error("could not write " + paths_.login_conf.string() + ": " + e.what());
    }
}

// =====================================================================================
// Levels
// =====================================================================================

// True while the loopback is in its grace period; a new stream (by serial) starts one.
bool Mixer::is_fresh(const std::string& tag, const pw::Node& stream)
{
    const auto now = Clock::now();
    const auto it = born_.find(tag);
    if (it == born_.end() || it->second.first != stream.serial()) {
        born_[tag] = {stream.serial(), now};  // a new loopback: its grace period starts now
        return true;
    }
    return now - it->second.second < kGrace;
}

// True if any loopback is still in its grace period.
bool Mixer::has_fresh_links() const
{
    const auto now = Clock::now();
    return std::any_of(born_.begin(), born_.end(),
                       [&](const auto& entry) { return now - entry.second.second < kGrace; });
}

// The tags of every saved level: each channel's two sliders and the mic's two.
std::vector<std::string> Mixer::level_tags() const
{
    // Every loopback whose level is saved: each channel's two sliders, and the mic's
    // Monitor. (VOD loopbacks follow the Stream level, so they aren't saved.)
    std::vector<std::string> tags;
    for (const auto& ch : config_["channels"])
        for (core::Mix mix : core::kMixes)
            tags.push_back(core::link_tag(string_field(ch, "id"), mix));
    tags.emplace_back(core::kMicMonitorTag);
    tags.emplace_back(core::kMicLevelTag);
    return tags;
}

// The node a level tag's slider controls: a loopback, or WaveMix Mic for "mic:level".
const pw::Node* Mixer::level_stream(const pw::Graph& g, const std::string& tag) const
{
    // A loopback, or for the Mic slider, WaveMix Mic itself.
    if (tag == core::kMicLevelTag)
        return g.source_named(std::string(core::kMicSource));
    return g.link_stream(tag);
}

// Reapplies saved levels to loopbacks still in their grace period (the Monitor starts muted).
void Mixer::apply_saved_levels()
{
    const pw::Graph g = graph();
    const auto& levels = state_["levels"];
    for (const auto& tag : level_tags()) {
        const pw::Node* link = level_stream(g, tag);
        if (!link || !is_fresh(tag, *link))
            continue;  // missing, or settled: leave it to the user
        json saved = levels.contains(tag) ? levels[tag] : json();
        if (!saved.is_object()) {
            // Hearing yourself starts off: the Monitor is muted until it's turned up.
            if (tag != core::kMicMonitorTag)
                continue;
            saved = {{"volume", 1.0}, {"muted", true}};
        }

        const double want_volume = saved.value("volume", 1.0);
        const bool want_muted = saved.value("muted", false);
        const auto now_volume = link->slider();
        if (!now_volume || std::abs(*now_volume - want_volume) > 0.002)
            session_->registry->set_volume(link->id, want_volume);
        if (link->muted != want_muted)
            session_->registry->set_mute(link->id, want_muted);
    }
}

// Copies each channel's Stream level and mute onto its VOD loopback when they differ.
void Mixer::sync_vod_levels()
{
    // A VOD loopback has no slider of its own: it always has its channel's Stream level
    // and mute. Most changes go through set_volume and set_mute, which set both at once;
    // this catches the rest: a new VOD loopback, WirePlumber restoring an old level, or
    // the Stream level changed in another app such as pavucontrol.
    const pw::Graph g = graph();
    for (const auto& ch : config_["channels"]) {
        const std::string id = string_field(ch, "id");
        const pw::Node* stream = g.link_stream(core::link_tag(id, core::Mix::Stream));
        const pw::Node* vod = g.link_stream(core::link_tag(id, core::Mix::Vod));
        if (!stream || !vod || !stream->slider())
            continue;
        const auto vod_volume = vod->slider();
        if (!vod_volume || std::abs(*vod_volume - *stream->slider()) > 0.002)
            session_->registry->set_volume(vod->id, *stream->slider());
        if (vod->muted != stream->muted)
            session_->registry->set_mute(vod->id, stream->muted);
    }
}

// Records a channel slider's new level in state_ (the mix masters aren't saved).
void Mixer::remember(std::string_view channel, core::Mix mix, std::optional<double> volume,
                     std::optional<bool> muted)
{
    if (channel.empty())
        return;  // the Stream Mix master isn't saved (WirePlumber remembers it)
    remember_level(core::link_tag(channel, mix), volume, muted);
}

// Records a level in state_ under `tag`, marking the levels dirty if anything changed.
void Mixer::remember_level(const std::string& tag, std::optional<double> volume, std::optional<bool> muted)
{
    auto& levels = state_["levels"];
    if (!levels.contains(tag) || !levels[tag].is_object())
        levels[tag] = {{"volume", 1.0}, {"muted", tag == core::kMicMonitorTag}};
    auto& level = levels[tag];
    if (volume && level["volume"] != *volume) {
        level["volume"] = *volume;
        levels_dirty_ = true;
    }
    if (muted && level["muted"] != *muted) {
        level["muted"] = *muted;
        levels_dirty_ = true;
    }
}

// Reads every settled slider from PipeWire into state_ and writes state.json if changed.
void Mixer::snapshot_levels()
{
    const pw::Graph g = graph();
    bool changed = levels_dirty_;
    levels_dirty_ = false;
    auto& levels = state_["levels"];
    for (const auto& tag : level_tags()) {
        const pw::Node* link = level_stream(g, tag);
        if (!link || !link->slider() || is_fresh(tag, *link))
            continue;  // missing, or WirePlumber may still be resetting it
        const json now = {{"volume", core::round4(*link->slider())}, {"muted", link->muted}};
        if (!levels.contains(tag) || levels[tag] != now) {
            levels[tag] = now;
            changed = true;
        }
    }
    if (changed)
        save_state();
}

// Validates a SetVolume/SetMute target and returns its mix; throws Failed if it's invalid.
core::Mix Mixer::check_level_target(std::string_view channel, std::string_view mix)
{
    if (!channel.empty() && !this->channel(channel))
        throw failed("no channel '" + std::string(channel) + "'");
    const auto parsed = core::parse_mix(mix);
    if (!parsed)
        throw failed("mix must be 'personal', 'stream' or 'vod', not '" + std::string(mix) + "'");
    if (*parsed == core::Mix::Vod && !channel.empty())
        throw failed("a channel's VOD level follows its Stream level; set 'stream' instead");
    return *parsed;
}

// The node behind a slider: a channel's loopback, or for channel "", a mix's own sink.
const pw::Node* Mixer::level_node(const pw::Graph& g, std::string_view channel, core::Mix mix) const
{
    // Channel "" is a mix's master level: the Personal, Stream or VOD Mix sink itself,
    // not a loopback.
    if (channel.empty())
        return g.sink_named(std::string(mix == core::Mix::Vod        ? core::kVodSink
                                        : mix == core::Mix::Personal ? core::kPersonalSink
                                                                     : core::kMixSink));
    return g.link_stream(core::link_tag(channel, mix));
}

// =====================================================================================
// D-Bus methods
// =====================================================================================

// SetVolume: sets and remembers a slider's level (and its VOD follower's).
void Mixer::set_volume(std::string_view channel, std::string_view mix, double volume)
{
    require_connection();
    const core::Mix m = check_level_target(channel, mix);
    volume = core::clamp_volume(volume);
    const pw::Graph g = graph();
    if (const pw::Node* node = level_node(g, channel, m)) {
        remember(channel, m, core::round4(volume), std::nullopt);
        session_->registry->set_volume(node->id, volume);
        save_at_ = Clock::now() + kSaveDelay;
    }
    if (const pw::Node* vod = vod_follower(g, channel, m))
        session_->registry->set_volume(vod->id, volume);
}

// SetMute: sets and remembers a slider's mute (and its VOD follower's).
void Mixer::set_mute(std::string_view channel, std::string_view mix, bool muted)
{
    require_connection();
    const core::Mix m = check_level_target(channel, mix);
    const pw::Graph g = graph();
    if (const pw::Node* node = level_node(g, channel, m)) {
        remember(channel, m, std::nullopt, muted);
        session_->registry->set_mute(node->id, muted);
        save_at_ = Clock::now() + kSaveDelay;
    }
    if (const pw::Node* vod = vod_follower(g, channel, m))
        session_->registry->set_mute(vod->id, muted);
}

// The VOD loopback that follows this slider: only a channel's Stream slider has one.
const pw::Node* Mixer::vod_follower(const pw::Graph& g, std::string_view channel, core::Mix mix) const
{
    // A channel's Stream slider also moves its VOD loopback, so both mixes always match.
    if (channel.empty() || mix != core::Mix::Stream)
        return nullptr;
    return g.link_stream(core::link_tag(channel, core::Mix::Vod));
}

// SetInVod: stores the flag, then reconciles so the VOD loopback is created or removed.
void Mixer::set_in_vod(std::string_view id, bool in_vod)
{
    require_connection();
    json* ch = channel(id);
    if (!ch)
        throw failed("no channel '" + std::string(id) + "'");
    (*ch)["in_vod"] = in_vod;
    save_config();

    // Out: the loopback goes, so nothing of the channel can reach the VOD, and it costs
    // nothing while it's off. In: reconcile creates it again, and it gets the Stream level.
    born_.erase(core::link_tag(id, core::Mix::Vod));
    reconcile();
    write_login_conf();
}

// AddChannel: validates the label, makes a unique ID, creates the channel's sink and
// loopbacks, and returns the ID.
std::string Mixer::add_channel(std::string_view label_in, std::string_view icon)
{
    require_connection();
    auto& channels = config_["channels"];
    if (static_cast<int>(channels.size()) >= core::kMaxChannels)
        throw failed("WaveMix supports up to " + std::to_string(core::kMaxChannels) + " channels");

    std::string label = trimmed(label_in);
    if (label.empty())
        label = "Channel";
    std::set<std::string, std::less<>> existing;
    for (const auto& ch : channels)
        existing.insert(string_field(ch, "id"));
    const std::string id = core::make_id(label, existing);

    channels.push_back({{"id", id}, {"label", label},
                        {"icon", icon.empty() ? std::string(core::kDefaultIcon) : std::string(icon)}});
    save_config();

    // A new channel starts at 100% and unmuted, whatever WirePlumber remembers from an
    // earlier channel with the same name. These levels are re-applied during the grace
    // period, which overrides WirePlumber's restore.
    for (core::Mix mix : core::kMixes) {
        const std::string tag = core::link_tag(id, mix);
        state_["levels"][tag] = {{"volume", 1.0}, {"muted", false}};
        born_.erase(tag);
    }
    save_state();

    reconcile();
    write_login_conf();
    return id;
}

// RenameChannel: stores the new label and/or icon; the live sink is renamed at next login.
void Mixer::rename_channel(std::string_view id, std::string_view label, std::string_view icon)
{
    require_connection();
    json* ch = channel(id);
    if (!ch)
        throw failed("no channel '" + std::string(id) + "'");
    if (const auto clean = trimmed(label); !clean.empty())
        (*ch)["label"] = clean;
    if (!icon.empty())
        (*ch)["icon"] = std::string(icon);
    save_config();
    // The live sink keeps its old description until the next login, when the
    // login file creates it with the new one.
    write_login_conf();
}

// MoveChannel: moves the channel to `position` (clamped) in config.json's list.
void Mixer::move_channel(std::string_view id, int position)
{
    require_connection();
    auto& channels = config_["channels"];
    const auto it = std::find_if(channels.begin(), channels.end(),
                                 [&](const json& ch) { return string_field(ch, "id") == id; });
    if (it == channels.end())
        throw failed("no channel '" + std::string(id) + "'");
    json moved = *it;
    channels.erase(it);
    const auto clamped = std::clamp<long>(position, 0, static_cast<long>(channels.size()));
    channels.insert(channels.begin() + clamped, std::move(moved));
    save_config();
}

// RemoveChannel: moves its apps to a real output, forgets its routes and levels, and
// unloads its sink and loopbacks.
void Mixer::remove_channel(std::string_view id)
{
    require_connection();
    if (!channel(id))
        throw failed("no channel '" + std::string(id) + "'");

    // Send the channel's apps somewhere sensible before its sink disappears: the
    // default output if that's a real device, else wherever the Personal mix plays.
    const pw::Graph g = graph();
    const pw::Node* base = g.sink_named(core::base_sink(id));
    const pw::Node* fallback = nullptr;
    for (const pw::Node* sink : g.hardware_sinks())
        if (sink->name() == g.default_sink)
            fallback = sink;
    if (!fallback) {
        if (const auto personal = resolve_personal_output(g))
            fallback = g.sink_named(*personal);
    }
    if (base && fallback) {
        for (const auto& [node_id, stream] : g.nodes) {
            if (stream.is_stream() && !core::is_internal(stream.props) && g.sink_of(stream) == base)
                move_stream(stream, *fallback);
        }
    }

    auto& channels = config_["channels"];
    channels.erase(std::remove_if(channels.begin(), channels.end(),
                                  [&](const json& ch) { return string_field(ch, "id") == id; }),
                   channels.end());
    auto& rules = config_["assignments"];
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&](const json& r) { return string_field(r, "channel") == id; }),
                rules.end());
    save_config();
    for (core::Mix mix : core::kMixes)
        state_["levels"].erase(core::link_tag(id, mix));
    save_state();

    session_->plumbing->remove_channel(id);
    reconcile();  // its loopbacks go, and the Personal Mix too if nothing's left
    write_login_conf();
}

// AssignApp: remembers the stream's app for a channel ("" forgets it) and moves every
// stream of that app there (or back to the Personal output).
void Mixer::assign_app(uint64_t serial, std::string_view channel_id)
{
    require_connection();
    const pw::Graph g = graph();
    const pw::Node* stream = g.stream_with_serial(serial);
    if (!stream)
        throw failed("stream " + std::to_string(serial) + " not found");
    if (!channel_id.empty() && !channel(channel_id))
        throw failed("no channel '" + std::string(channel_id) + "'");

    // Remember it for the app (every stream of it, now and in future), not just
    // this one stream.
    const auto key = core::app_key(stream->props);
    if (key) {
        auto& rules = config_["assignments"];
        rules.erase(std::remove_if(rules.begin(), rules.end(),
                                   [&](const json& r) { return string_field(r, "app") == *key; }),
                    rules.end());
        if (!channel_id.empty())
            rules.push_back({{"app", *key}, {"label", core::app_name(stream->props, stream->id)},
                             {"channel", std::string(channel_id)}});
        save_config();
    }

    // "" sends it back to the user's own output. Not simply the default output: that
    // could be a WaveMix channel, if the user picked one in their sound settings.
    const auto personal = resolve_personal_output(g);
    const pw::Node* target = channel_id.empty() ? (personal ? g.sink_named(*personal) : nullptr)
                                                : g.sink_named(core::base_sink(channel_id));
    if (!target)
        return;
    for (const auto& [id, s] : g.nodes) {
        if (!s.is_stream())
            continue;
        const bool same_app = key && !core::is_internal(s.props) && core::app_key(s.props) == key;
        if ((s.serial() == serial || same_app) && g.sink_of(s) != target)
            move_stream(s, *target);
    }
}

// ForgetAssignment: drops the remembered route for an app key.
void Mixer::forget_assignment(std::string_view app_key)
{
    require_connection();
    auto& rules = config_["assignments"];
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&](const json& r) { return string_field(r, "app") == app_key; }),
                rules.end());
    save_config();
}

// SetSetting: stores a known setting and rebuilds what depends on it.
void Mixer::set_setting(std::string_view key, const json& value)
{
    require_connection();
    auto& settings = config_["settings"];
    if (!settings.contains(std::string(key)))
        throw failed("unknown setting '" + std::string(key) + "'");
    settings[std::string(key)] = value;
    save_config();
    if (key == "personal_output" || key == "fallback_output" || key == "fallback_input") {
        reconcile();
        write_login_conf();
    }
    if (key == "pulse_routing") {
        // The loopbacks move over: the ones where they ran go, and the others load.
        reconcile();
        write_login_conf();
    }
    if (key == "vod_enabled") {
        // Off: the VOD Mix device and every VOD loopback go, so OBS no longer lists it
        // and they cost no open files. On: reconcile creates them again, for the
        // channels that are in the VOD.
        if (!core::vod_enabled(config_))
            session_->plumbing->remove_vod();
        reconcile();
        write_login_conf();
    }
}

// Reset: levels to 100%, every channel removed, the default config, and the mic's
// levels as on a first run.
void Mixer::reset()
{
    require_connection();
    const auto set_level = [&](const pw::Node* node, bool muted) {
        if (!node)
            return;
        session_->registry->set_volume(node->id, 1.0);
        session_->registry->set_mute(node->id, muted);
    };
    // The mixes' own levels first, while they're all there (the VOD Mix goes with its
    // setting), so none is left for a session manager that remembers levels by device
    // name to bring back.
    {
        const pw::Graph g = graph();
        for (core::Mix mix : {core::Mix::Personal, core::Mix::Stream, core::Mix::Vod})
            set_level(level_node(g, "", mix), false);
    }

    // The channels go one at a time, as Remove does it, so their apps move to the
    // default output before their devices disappear.
    std::vector<std::string> ids;
    for (const auto& ch : config_["channels"])
        ids.push_back(string_field(ch, "id"));
    for (const auto& id : ids)
        remove_channel(id);

    // Then everything else. The hardware default WaveMix may have to give back is kept:
    // it's the system's, not a preference.
    config_ = core::default_config();
    state_["levels"] = json::object();
    save_config();
    save_state();
    session_->plumbing->remove_vod();
    reconcile();
    write_login_conf();

    // And the mic's, as a first run has them.
    const pw::Graph g = graph();
    set_level(level_stream(g, std::string(core::kMicLevelTag)), false);
    set_level(level_stream(g, std::string(core::kMicMonitorTag)), true);
    log("reset to the defaults");
}

// =====================================================================================
// GetState
// =====================================================================================

// The icon for an app, cached per app, re-reading menu entries when one isn't found.
std::string Mixer::app_icon(const core::Props& props)
{
    // Remembered per app, so the menu entries are only read when a new app shows up.
    const std::string key = core::app_key(props).value_or("") + "|" + core::app_name(props, 0);
    if (const auto it = icons_.find(key); it != icons_.end())
        return it->second;

    const auto now = Clock::now();
    const auto find = [&] { return core::icon_for(props, desktop_entries_); };
    auto icon = entries_loaded_ ? find() : std::nullopt;
    if (!icon && (!entries_loaded_ || now - *entries_loaded_ > kEntriesMaxAge)) {
        // Not found: maybe the app was installed since we last looked.
        desktop_entries_ = core::load_desktop_entries(core::application_dirs());
        entries_loaded_ = now;
        icons_.clear();
        icon = find();
    }
    return icons_[key] = icon.value_or("application-x-executable");
}

// Every app stream as GetState lists it, sorted by name.
json Mixer::apps(const pw::Graph& g)
{
    std::map<std::string, std::string> base_to_channel;
    for (const auto& ch : config_["channels"])
        base_to_channel[core::base_sink(string_field(ch, "id"))] = string_field(ch, "id");
    std::map<std::string, std::string> rules;
    for (const auto& rule : config_["assignments"])
        rules[string_field(rule, "app")] = string_field(rule, "channel");

    std::vector<json> out;
    for (const auto& [id, stream] : g.nodes) {
        if (!stream.is_stream() || core::is_internal(stream.props))
            continue;
        const pw::Node* sink = g.sink_of(stream);
        const auto key = core::app_key(stream.props);
        const auto prop = [&](const char* name) {
            const auto it = stream.props.find(name);
            return it == stream.props.end() ? std::string() : it->second;
        };
        const auto channel_it = sink ? base_to_channel.find(sink->name()) : base_to_channel.end();

        out.push_back({
            {"index", stream.serial()},
            {"name", core::app_name(stream.props, stream.id)},
            {"media", prop("media.name")},
            {"icon", app_icon(stream.props)},
            {"key", key ? json(*key) : json(nullptr)},
            {"channel", channel_it != base_to_channel.end() ? json(channel_it->second) : json(nullptr)},
            {"assigned", key && rules.contains(*key) ? json(rules.at(*key)) : json(nullptr)},
            {"sink", sink ? json(sink->name()) : json(nullptr)},
            {"sink_description", sink ? sink->description() : std::string()},
        });
    }
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return lowercase(a["name"].get<std::string>()) < lowercase(b["name"].get<std::string>());
    });
    return json(out);
}

// GetState: channels with their levels and apps, outputs, mixes, the mic and settings;
// a shorter "connected": false state while PipeWire is away.
json Mixer::state()
{
    json presets = json::array();
    for (const auto& p : core::kPresets)
        presets.push_back({{"label", std::string(p.label)}, {"icon", std::string(p.icon)}});

    const auto disconnected = [&] {
        return json{{"connected", false},    {"channels", json::array()},
                    {"unrouted_apps", json::array()}, {"outputs", json::array()},
                    {"settings", config_["settings"]}, {"presets", presets},
                    {"assignments", config_["assignments"]}, {"version", version_}};
    };
    if (!session_)
        return disconnected();

    pw::Graph g;
    try {
        g = graph();
    } catch (const MixerError&) {
        return disconnected();
    }

    const auto level = level_json;

    const json all_apps = apps(g);
    json channels = json::array();
    for (const auto& ch : config_["channels"]) {
        const std::string id = string_field(ch, "id");
        json entry = ch;  // id, label, icon, and anything else stored with it
        entry["personal"] = level(g.link_stream(core::link_tag(id, core::Mix::Personal)));
        entry["stream"] = level(g.link_stream(core::link_tag(id, core::Mix::Stream)));
        entry["in_vod"] = core::in_vod(ch);
        entry["apps"] = json::array();
        for (const auto& app : all_apps)
            if (app["channel"] == id)
                entry["apps"].push_back(app);
        channels.push_back(std::move(entry));
    }
    json unrouted = json::array();
    for (const auto& app : all_apps)
        if (app["channel"].is_null())
            unrouted.push_back(app);

    json outputs = json::array();
    for (const pw::Node* sink : g.hardware_sinks())
        outputs.push_back({{"name", sink->name()}, {"description", sink->description()},
                           {"is_default", sink->name() == g.default_sink}});

    const auto resolved = resolve_personal_output(g);
    const pw::Node* resolved_sink = resolved ? g.sink_named(*resolved) : nullptr;
    const std::string setting = string_field(config_["settings"], "personal_output");

    return {
        {"connected", true},
        {"channels", channels},
        {"unrouted_apps", unrouted},
        {"stream_mix", level(g.sink_named(std::string(core::kMixSink)))},
        {"vod_mix", level(g.sink_named(std::string(core::kVodSink)))},
        {"outputs", outputs},
        {"personal_output",
         {{"setting", setting.empty() ? "default" : setting},
          {"resolved", resolved ? json(*resolved) : json(nullptr)},
          {"description", resolved_sink ? json(resolved_sink->description()) : json(nullptr)},
          {"level", level(resolved_sink)}}},
        {"personal_mix", level(g.sink_named(std::string(core::kPersonalSink)))},
        {"mic", mic_state(g)},
        {"settings", config_["settings"]},
        {"assignments", config_["assignments"]},
        {"presets", presets},
        {"version", version_},
    };
}

// The "mic" part of GetState: input, levels, the chain (with why a filter isn't
// running) and every filter on offer.
json Mixer::mic_state(const pw::Graph& g) const
{
    const auto plugins = core::find_mic_plugins();
    const auto input = resolve_mic_input(g);
    const auto plan = core::plan_mic(config_, input, plugins);
    std::set<std::size_t> built;
    if (plan)
        for (const auto& stage : plan->stages)
            built.insert(stage.position);

    // Each filter in the user's list, and if it isn't running, why not.
    json chain = json::array();
    const auto steps = core::mic_chain(config_);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        json why = nullptr;
        if (!built.contains(i + 1)) {
            if (!plan)
                why = core::mic_enabled(config_) ? "no_input" : "disabled";
            else if (!step.on)
                why = "off";
            else
                why = "missing";
        }
        chain.push_back({{"filter", std::string(core::to_string(step.filter))},
                         {"on", step.on},
                         {"settings", core::mic_step_settings(step)},
                         {"running", why.is_null()},
                         {"why_not", why}});
    }

    json filters = json::array();
    for (core::MicFilter filter : core::kMicFilters) {
        json controls = json::array();
        for (const auto& control : core::mic_controls(filter))
            controls.push_back({{"key", std::string(control.key)},
                                {"unit", std::string(control.unit)},
                                {"min", control.min},
                                {"max", control.max},
                                {"default", control.fallback}});
        filters.push_back({{"filter", std::string(core::to_string(filter))},
                           {"found", plugins.contains(filter)},
                           {"package", core::mic_package(filter)},
                           {"delay_ms", core::mic_filter_delay_ms(filter)},
                           {"controls", controls}});
    }

    json inputs = json::array();
    for (const pw::Node* source : g.hardware_sources())
        inputs.push_back({{"name", source->name()}, {"description", source->description()},
                          {"is_default", source->name() == g.default_source}});
    const pw::Node* input_node = input ? g.source_named(*input) : nullptr;

    return {
        {"enabled", core::mic_enabled(config_)},
        {"input",
         {{"setting", core::mic_input_setting(config_)},
          {"resolved", input ? json(*input) : json(nullptr)},
          {"description", input_node ? json(input_node->description()) : json(nullptr)}}},
        {"inputs", inputs},
        {"level", level_json(g.source_named(std::string(core::kMicSource)))},
        {"monitor", level_json(g.link_stream(std::string(core::kMicMonitorTag)))},
        {"chain", chain},
        {"delay_ms", plan ? core::mic_chain_delay_ms(*plan) : 0.0},
        {"max_filters", core::kMaxMicChain},
        {"filters", filters},
    };
}

// GetMeters: (re)starts the meters for the mic and channels and returns their peaks.
json Mixer::meters()
{
    // The mic before the chain, WaveMix Mic after it, and what plays into each channel,
    // while someone's watching.
    require_connection();
    meters_asked_ = Clock::now();
    const pw::Graph g = graph();
    std::map<std::string, plumbing::MeterSource> sources;
    if (const auto input = resolve_mic_input(g); input && core::mic_enabled(config_))
        sources["input"] = {*input};
    if (g.source_named(std::string(core::kMicSource)))
        sources["mic"] = {std::string(core::kMicSource)};
    constexpr std::string_view kChannel = "channel:";
    for (const auto& channel : config_["channels"]) {
        const std::string sink = core::base_sink(string_field(channel, "id"));
        if (g.sink_named(sink))
            sources[std::string(kChannel) + string_field(channel, "id")] = {sink, true};
    }
    session_->meters->watch(sources);

    // {"input": .., "mic": .., "channels": {"<id>": ..}}
    json out = {{"channels", json::object()}};
    for (const auto& [name, peak] : session_->meters->take()) {
        if (name.starts_with(kChannel))
            out["channels"][name.substr(kChannel.size())] = core::round4(peak);
        else
            out[name] = core::round4(peak);
    }
    return out;
}

// =====================================================================================
// The mic's D-Bus methods
// =====================================================================================

// SetMicInput: stores the input ("default" if empty) and rebuilds the chain on it.
void Mixer::set_mic_input(std::string_view device)
{
    require_connection();
    std::string setting = trimmed(device);
    if (setting.empty())
        setting = "default";
    if (!config_["mic"].is_object())
        config_["mic"] = json::object();
    config_["mic"]["input"] = setting;
    save_config();
    reconcile();  // a different input: the chain is rebuilt on it
    write_login_conf();
}

// SetMicEnabled: stores the flag and creates or removes the mic's devices and chain.
void Mixer::set_mic_enabled(bool enabled)
{
    require_connection();
    if (!config_["mic"].is_object())
        config_["mic"] = json::object();
    config_["mic"]["enabled"] = enabled;
    save_config();
    reconcile();  // off: the chain and WaveMix Mic go; on: they're built again
    write_login_conf();
}

// The saved-level tag for a mic slider name ("mic" or "monitor"); throws for others.
std::string Mixer::mic_level_tag(std::string_view slider) const
{
    if (slider == "mic")
        return std::string(core::kMicLevelTag);
    if (slider == "monitor")
        return std::string(core::kMicMonitorTag);
    throw failed("slider must be 'mic' or 'monitor', not '" + std::string(slider) + "'");
}

// SetMicVolume: remembers and sets the Mic or Monitor level.
void Mixer::set_mic_volume(std::string_view slider, double volume)
{
    require_connection();
    const std::string tag = mic_level_tag(slider);
    volume = core::clamp_volume(volume);
    // Saved even while the chain isn't there (no mic plugged in), so it's used once it is.
    remember_level(tag, core::round4(volume), std::nullopt);
    const pw::Graph g = graph();  // the node below points into it
    if (const pw::Node* node = level_stream(g, tag))
        session_->registry->set_volume(node->id, volume);
    save_at_ = Clock::now() + kSaveDelay;
}

// SetMicMute: remembers and sets the Mic or Monitor mute.
void Mixer::set_mic_mute(std::string_view slider, bool muted)
{
    require_connection();
    const std::string tag = mic_level_tag(slider);
    remember_level(tag, std::nullopt, muted);
    const pw::Graph g = graph();  // the node below points into it
    if (const pw::Node* node = level_stream(g, tag))
        session_->registry->set_mute(node->id, muted);
    save_at_ = Clock::now() + kSaveDelay;
}

// Runs a chain edit, turning bad input into a Failed error, then saves and rebuilds.
void Mixer::edit_mic_chain(const std::function<void()>& edit)
{
    require_connection();
    try {
        edit();
    } catch (const std::invalid_argument& e) {
        throw failed(e.what());
    }
    save_config();
    reconcile();  // a chain that no longer matches its plan is rebuilt
    write_login_conf();
}

// AddMicFilter: inserts a filter and returns where it went.
int Mixer::add_mic_filter(std::string_view filter, int position)
{
    std::size_t at = 0;
    edit_mic_chain([&] { at = core::add_mic_filter(config_, filter, position); });
    return static_cast<int>(at);
}

// RemoveMicFilter: removes the filter at `position`.
void Mixer::remove_mic_filter(int position)
{
    edit_mic_chain([&] { core::remove_mic_filter(config_, position); });
}

// MoveMicFilter: moves a filter within the chain.
void Mixer::move_mic_filter(int from, int to)
{
    edit_mic_chain([&] { core::move_mic_filter(config_, from, to); });
}

// SetMicFilterOn: switches a filter on or off.
void Mixer::set_mic_filter_on(int position, bool on)
{
    edit_mic_chain([&] { core::set_mic_filter_on(config_, position, on); });
}

// SetMicFilterValue: stores one filter setting and sends it to the running chain live.
void Mixer::set_mic_filter_value(int position, std::string_view key, double value)
{
    // Live: the value goes straight to the filter's node. The chain stays as it is.
    require_connection();
    try {
        core::set_mic_filter_value(config_, position, key, value);
    } catch (const std::invalid_argument& e) {
        throw failed(e.what());
    }
    save_config();
    apply_mic_controls(graph());
}

// =====================================================================================
// Small helpers
// =====================================================================================

// The config entry for a channel ID, or nullptr if there's none.
json* Mixer::channel(std::string_view id)
{
    for (auto& ch : config_["channels"])
        if (string_field(ch, "id") == id)
            return &ch;
    return nullptr;
}

// Writes config.json.
void Mixer::save_config()
{
    core::save_config(paths_, config_);
}

// Writes state.json.
void Mixer::save_state()
{
    core::save_state(paths_, state_);
}

}  // namespace wavemix::mixer
