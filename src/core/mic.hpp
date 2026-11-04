// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The microphone chain: what it's made of, worked out from the config.
//
//   mic input ─► the chain (one filter-chain node in the service: gate, compressor, ...)
//             ─► wavemix_mic_feed ─► wavemix_mic ("WaveMix Mic", what OBS records)
//                                       └─► the Monitor loopback ─► the Personal Mix
//
// The chain is the user's own list of filters, in their order: it starts empty (every
// mic and every voice needs something different), and filters are added, removed,
// moved and switched off as needed. The same filter can be in it more than once. A
// filter that's off, or whose plugin isn't installed, is simply left out.
//
// The chain runs in the WaveMix service, as one PipeWire filter-chain node with every
// filter inside it (core/links.hpp hosts it like the loopbacks), so there's no limit to
// how many it holds. It plays into WaveMix Mic, a pipewire-pulse device from the login
// file, so OBS always finds it: while the service isn't running it's just quiet.
//
// Plain C++ on plain data, apart from the plugin search, so it's unit-tested.

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json_file.hpp"

namespace wavemix::core {

// WaveMix Mic, the source OBS records, and the sink the chain plays into, whose monitor
// it is.
inline constexpr std::string_view kMicSource = "wavemix_mic";
inline constexpr std::string_view kMicLabel = "WaveMix Mic";
inline constexpr std::string_view kMicFeedSink = "wavemix_mic_feed";
inline constexpr std::string_view kMicFeedLabel = "WaveMix Mic feed";

// The chain's tag among the service's modules, and its two nodes: the capture side
// (where the filters' controls are) and the playback side.
inline constexpr std::string_view kMicChainTag = "mic:chain";
inline constexpr std::string_view kMicChainNode = "wavemix_mic_chain";

// The Monitor loopback's wavemix.link tag, and its key for saved levels.
inline constexpr std::string_view kMicMonitorTag = "mic:monitor";
// The Mic slider's key for saved levels: the level of WaveMix Mic itself.
inline constexpr std::string_view kMicLevelTag = "mic:level";

// The filters a chain can be made of, in the order they're offered.
enum class MicFilter { Noise, Gain, Gate, Expander, Equalizer, Compressor, Limiter };
inline constexpr std::array<MicFilter, 7> kMicFilters{MicFilter::Noise,     MicFilter::Gain,
                                                      MicFilter::Gate,      MicFilter::Expander,
                                                      MicFilter::Equalizer, MicFilter::Compressor,
                                                      MicFilter::Limiter};

// "noise", "gain", "gate", "expander", "equalizer", "compressor", "limiter": in
// config.json and on D-Bus.
std::string_view to_string(MicFilter filter);
// Parses a filter name back into a MicFilter; nullopt if it isn't one.
std::optional<MicFilter> parse_mic_filter(std::string_view text);

// The LADSPA plugin a filter runs.
struct MicPlugin {
    std::string_view file;     // "librnnoise_ladspa.so"
    std::string_view label;    // the plugin's LADSPA label inside that file
    std::string_view package;  // what to install when it's missing (Arch and most others)
};
// The LADSPA plugin (file, label, package) that runs a filter.
const MicPlugin& mic_plugin(MicFilter filter);

// What to install when a filter's plugin is missing: the distribution's package, or
// inside the Flatpak, Flathub's LSP extension (the app brings RNNoise and the runtime
// has the amplifier).
std::string mic_package(MicFilter filter);

// A setting a filter offers (the ones the window shows): its key in config.json and on
// D-Bus, its unit, range and default. Values are in the unit shown ("dB", not the
// plugin's own linear gain), and stored only once the user changes them.
struct MicControl {
    std::string_view key;   // "threshold"
    std::string_view unit;  // "dB", "%", "Hz" or "" (a plain number, like a ratio)
    double min;
    double max;
    double fallback;  // the default
};
// The settings a filter offers, in the order they're shown.
std::span<const MicControl> mic_controls(MicFilter filter);
// One setting of a filter by key, or nullptr if the filter has no such setting.
const MicControl* mic_control(MicFilter filter, std::string_view key);

// One entry in the user's chain.
struct MicStep {
    MicFilter filter;
    bool on = true;
    std::map<std::string, double> settings;  // only the ones changed from the default
};

// The longest list of filters config.json keeps (only the first kMaxMicFilters that can
// run are built).
inline constexpr std::size_t kMaxMicChain = 16;

// The "mic" section of config.json: whether the mic exists at all, the input
// ("default" or a source name) and the chain, in order. Entries naming a filter this
// version doesn't know are skipped.
bool mic_enabled(const json& config);
// The mic's input setting: "default" or a source name.
std::string mic_input_setting(const json& config);
// The user's chain as stored, skipping entries with unknown filters.
std::vector<MicStep> mic_chain(const json& config);

// Editing the chain in config.json, as the D-Bus methods do. Positions count from 0,
// in the user's list. Bad input (an unknown filter or setting, a position that isn't
// in the list, a full list) throws std::invalid_argument with a message for the user.
// add_mic_filter returns where the filter went: `position`, or the end when it's out
// of range. A value is clamped to its setting's range.
std::size_t add_mic_filter(json& config, std::string_view filter, int position);
// Removes the filter at `position`.
void remove_mic_filter(json& config, int position);
// Moves the filter at `from` to `to`.
void move_mic_filter(json& config, int from, int to);
// Switches the filter at `position` on or off.
void set_mic_filter_on(json& config, int position, bool on);
// Changes one setting of the filter at `position`, clamped to its range.
void set_mic_filter_value(json& config, int position, std::string_view key, double value);

// Every setting of a step, with the defaults filled in.
json mic_step_settings(const MicStep& step);

// What a step's settings become in its plugin: (port name, value) pairs, in the plugin's
// own units (linear gain rather than dB), plus the fixed values WaveMix runs it with.
// They're set as params on the filter's node (module-ladspa-source is a filter-chain,
// whose controls are named after the plugin's ports), live, without a rebuild.
using MicPortValues = std::vector<std::pair<std::string, double>>;
// Converts a step's settings to the plugin's port values (see above).
MicPortValues mic_port_values(const MicStep& step);

// Where each filter's plugin is, as a path this process can load (the service runs the
// plugins itself). Folders are searched in order: `first` (the plugins the Flatpak
// brings along), LADSPA_PATH, ~/.ladspa (where a plugin downloaded by hand goes: RNNoise
// isn't packaged everywhere), the usual system folders, and inside a Flatpak, the
// system's own folders under /run/host. Filters whose plugin isn't anywhere are missing
// from the map.
using MicPlugins = std::map<MicFilter, std::string>;
// Searches the plugin folders for each filter's plugin (see above).
MicPlugins find_mic_plugins(const std::vector<std::filesystem::path>& first = {});

// A filter the chain runs.
struct MicStage {
    std::size_t position;  // its place in the user's list, from 1
    MicFilter filter;
    std::string plugin;  // the plugin's path

    // Its name inside the chain: s<position>. Its controls are "s<position>:<port>".
    std::string node() const;
};

// The chain to build: the input, and the filters in order.
struct MicPlan {
    std::string input;
    std::vector<MicStage> stages;
};

// The plan for this config, or nothing when the mic is off or there's no input.
std::optional<MicPlan> plan_mic(const json& config, const std::optional<std::string>& input,
                                const MicPlugins& plugins);

// How much later the voice reaches WaveMix Mic through a filter, in milliseconds:
// RNNoise works in 10 ms frames, and the limiter looks 5.3 ms ahead to catch peaks.
// The rest work sample by sample. Measured, since the chain can't ask the plugins.
double mic_filter_delay_ms(MicFilter filter);
// The whole chain's: what the filters that run add up to.
double mic_chain_delay_ms(const MicPlan& plan);

// The service's module for the chain: libpipewire-module-filter-chain with every filter
// in it, or with no filters, a plain libpipewire-module-loopback; from the input to
// WaveMix Mic's feed. (name, arguments)
std::pair<std::string, std::string> mic_chain_module(const MicPlan& plan);

// The pipewire-pulse modules for WaveMix Mic: the feed sink, then WaveMix Mic reading its
// monitor. (name, arguments)
std::vector<std::pair<std::string, std::string>> mic_device_modules();

}  // namespace wavemix::core
