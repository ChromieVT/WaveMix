// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/mic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "core/login_conf.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"

namespace fs = std::filesystem;

namespace wavemix::core {

namespace {

// The config's "mic" object, or an empty object when it's missing or not an object.
const json& mic_section(const json& config)
{
    static const json empty = json::object();
    const auto it = config.find("mic");
    return it != config.end() && it->is_object() ? *it : empty;
}

// A string in PipeWire's config format: "..." with \ and " escaped.
std::string conf_string(std::string_view text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

// The config / D-Bus name of a filter.
std::string_view to_string(MicFilter filter)
{
    switch (filter) {
    case MicFilter::Noise:
        return "noise";
    case MicFilter::Gain:
        return "gain";
    case MicFilter::Gate:
        return "gate";
    case MicFilter::Expander:
        return "expander";
    case MicFilter::Equalizer:
        return "equalizer";
    case MicFilter::Compressor:
        return "compressor";
    case MicFilter::Limiter:
        return "limiter";
    }
    return "";
}

// Looks a filter up by its config / D-Bus name.
std::optional<MicFilter> parse_mic_filter(std::string_view text)
{
    for (MicFilter filter : kMicFilters) {
        if (to_string(filter) == text)
            return filter;
    }
    return std::nullopt;
}

// The fixed table of which LADSPA plugin file and label each filter runs.
const MicPlugin& mic_plugin(MicFilter filter)
{
    // RNNoise (noise-suppression-for-voice) for noise removal, the LADSPA SDK's amplifier
    // for gain, and Linux Studio Plugins for the rest. Mono versions: a microphone is one
    // channel.
    static const MicPlugin kNoise{"librnnoise_ladspa.so", "noise_suppressor_mono", "noise-suppression-for-voice"};
    static const MicPlugin kGain{"amp.so", "amp_mono", "ladspa"};
    static const MicPlugin kGate{"lsp-plugins-ladspa.so", "http://lsp-plug.in/plugins/ladspa/gate_mono",
                                 "lsp-plugins-ladspa"};
    static const MicPlugin kExpander{"lsp-plugins-ladspa.so", "http://lsp-plug.in/plugins/ladspa/expander_mono",
                                     "lsp-plugins-ladspa"};
    static const MicPlugin kEqualizer{"lsp-plugins-ladspa.so",
                                      "http://lsp-plug.in/plugins/ladspa/para_equalizer_x8_mono",
                                      "lsp-plugins-ladspa"};
    static const MicPlugin kCompressor{"lsp-plugins-ladspa.so", "http://lsp-plug.in/plugins/ladspa/compressor_mono",
                                       "lsp-plugins-ladspa"};
    static const MicPlugin kLimiter{"lsp-plugins-ladspa.so", "http://lsp-plug.in/plugins/ladspa/limiter_mono",
                                    "lsp-plugins-ladspa"};
    switch (filter) {
    case MicFilter::Noise:
        return kNoise;
    case MicFilter::Gain:
        return kGain;
    case MicFilter::Gate:
        return kGate;
    case MicFilter::Expander:
        return kExpander;
    case MicFilter::Equalizer:
        return kEqualizer;
    case MicFilter::Compressor:
        return kCompressor;
    case MicFilter::Limiter:
        break;
    }
    return kLimiter;
}

// The fixed table of settings (key, unit, range, default) each filter offers.
std::span<const MicControl> mic_controls(MicFilter filter)
{
    // The main controls of each filter, as the window shows them. The defaults are the
    // plugins' own (LSP's gate at -24 dB, its compressor and expander at -15 dB, 3:1),
    // apart from the gate's reduction (the plugin's is 0 dB, a gate that does nothing)
    // and the limiter's ceiling, which sits just under full scale.
    static constexpr MicControl kNoise[] = {{"threshold", "%", 0, 99, 50}};
    static constexpr MicControl kGain[] = {{"gain", "dB", -20, 20, 0}};
    static constexpr MicControl kGate[] = {{"threshold", "dB", -60, 0, -24}, {"reduction", "dB", -60, 0, -40}};
    static constexpr MicControl kDynamics[] = {{"threshold", "dB", -60, 0, -15}, {"ratio", "", 1, 20, 3}};
    static constexpr MicControl kLimiter[] = {{"ceiling", "dB", -24, 0, -1}};
    static constexpr MicControl kEqualizer[] = {
        {"band_1_frequency", "Hz", 20, 20000, 60},    {"band_1_gain", "dB", -24, 24, 0}, {"band_1_q", "", 0.1, 10, 1},
        {"band_2_frequency", "Hz", 20, 20000, 150},   {"band_2_gain", "dB", -24, 24, 0}, {"band_2_q", "", 0.1, 10, 1},
        {"band_3_frequency", "Hz", 20, 20000, 400},   {"band_3_gain", "dB", -24, 24, 0}, {"band_3_q", "", 0.1, 10, 1},
        {"band_4_frequency", "Hz", 20, 20000, 1000},  {"band_4_gain", "dB", -24, 24, 0}, {"band_4_q", "", 0.1, 10, 1},
        {"band_5_frequency", "Hz", 20, 20000, 2500},  {"band_5_gain", "dB", -24, 24, 0}, {"band_5_q", "", 0.1, 10, 1},
        {"band_6_frequency", "Hz", 20, 20000, 5000},  {"band_6_gain", "dB", -24, 24, 0}, {"band_6_q", "", 0.1, 10, 1},
        {"band_7_frequency", "Hz", 20, 20000, 10000}, {"band_7_gain", "dB", -24, 24, 0}, {"band_7_q", "", 0.1, 10, 1},
        {"band_8_frequency", "Hz", 20, 20000, 15000}, {"band_8_gain", "dB", -24, 24, 0}, {"band_8_q", "", 0.1, 10, 1},
    };
    switch (filter) {
    case MicFilter::Noise:
        return kNoise;
    case MicFilter::Gain:
        return kGain;
    case MicFilter::Gate:
        return kGate;
    case MicFilter::Expander:
    case MicFilter::Compressor:
        return kDynamics;
    case MicFilter::Equalizer:
        return kEqualizer;
    case MicFilter::Limiter:
        break;
    }
    return kLimiter;
}

// Finds one of a filter's settings by key.
const MicControl* mic_control(MicFilter filter, std::string_view key)
{
    for (const auto& control : mic_controls(filter))
        if (control.key == key)
            return &control;
    return nullptr;
}

// The package to suggest for a missing plugin: Flathub's LSP extension inside a
// Flatpak, else the distribution package.
std::string mic_package(MicFilter filter)
{
    if (in_flatpak() && mic_plugin(filter).package == "lsp-plugins-ladspa")
        return "org.freedesktop.LinuxAudio.Plugins.LSP//25.08";
    return std::string(mic_plugin(filter).package);
}

// Reads mic "enabled"; missing or non-boolean counts as on.
bool mic_enabled(const json& config)
{
    const json& mic = mic_section(config);
    const auto it = mic.find("enabled");
    return it == mic.end() || !it->is_boolean() || it->get<bool>();
}

// Reads mic "input"; missing or empty counts as "default".
std::string mic_input_setting(const json& config)
{
    const json& mic = mic_section(config);
    const auto it = mic.find("input");
    return it != mic.end() && it->is_string() && !it->get<std::string>().empty() ? it->get<std::string>()
                                                                                : "default";
}

// Reads mic "chain" into steps, skipping unknown filters and clamping known settings.
std::vector<MicStep> mic_chain(const json& config)
{
    std::vector<MicStep> steps;
    const json& mic = mic_section(config);
    const auto chain = mic.find("chain");
    if (chain == mic.end() || !chain->is_array())
        return steps;
    for (const auto& entry : *chain) {
        if (!entry.is_object())
            continue;
        const auto name = entry.find("filter");
        const auto filter =
            name != entry.end() && name->is_string() ? parse_mic_filter(name->get<std::string>()) : std::nullopt;
        if (!filter)
            continue;
        const auto on = entry.find("on");
        MicStep step{*filter, on == entry.end() || !on->is_boolean() || on->get<bool>(), {}};
        if (const auto settings = entry.find("settings"); settings != entry.end() && settings->is_object()) {
            for (const auto& [key, value] : settings->items()) {
                const MicControl* control = mic_control(*filter, key);
                if (control && value.is_number())
                    step.settings[key] = std::clamp(value.get<double>(), control->min, control->max);
            }
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

// A step's settings as JSON, with the default for each one it hasn't changed.
json mic_step_settings(const MicStep& step)
{
    json settings = json::object();
    for (const auto& control : mic_controls(step.filter)) {
        const auto it = step.settings.find(std::string(control.key));
        settings[std::string(control.key)] = it != step.settings.end() ? it->second : control.fallback;
    }
    return settings;
}

namespace {

// Converts decibels to linear gain.
double gain_from_db(double db)
{
    return std::pow(10.0, db / 20.0);
}

}  // namespace

// Maps a step's settings to its plugin's port names and units (see the header).
MicPortValues mic_port_values(const MicStep& step)
{
    const json settings = mic_step_settings(step);
    const auto value = [&](const std::string& key) { return settings.at(key).get<double>(); };
    switch (step.filter) {
    case MicFilter::Noise:
        // "Retroactive VAD Grace" holds the voice back to keep the start of each word, by
        // 100 ms out of the box: far too late for a stream. Off, RNNoise delays it by its
        // 10 ms frame only.
        return {{"VAD Threshold (%)", value("threshold")}, {"Retroactive VAD Grace (ms)", 0}};
    case MicFilter::Gain:
        return {{"Gain", gain_from_db(value("gain"))}};
    case MicFilter::Gate:
        return {{"Curve threshold (G)", gain_from_db(value("threshold"))},
                {"Reduction (G)", gain_from_db(value("reduction"))}};
    case MicFilter::Expander:
        // Mode 0 is downward expansion (quiet sounds made quieter), what a mic needs;
        // the plugin starts in upward mode, which makes them louder.
        return {{"Expander mode", 0},
                {"Attack threshold (G)", gain_from_db(value("threshold"))},
                {"Ratio", value("ratio")}};
    case MicFilter::Compressor:
        return {{"Attack threshold (G)", gain_from_db(value("threshold"))}, {"Ratio", value("ratio")}};
    case MicFilter::Equalizer: {
        // LSP numbers the bands from 0, and starts with every band off. Type 1 is a bell.
        MicPortValues values;
        for (int band = 0; band < 8; ++band) {
            const std::string ours = "band_" + std::to_string(band + 1) + "_";
            const std::string n = std::to_string(band);
            values.emplace_back("Filter type " + n, 1);
            values.emplace_back("Frequency " + n + " (Hz)", value(ours + "frequency"));
            values.emplace_back("Gain " + n + " (G)", gain_from_db(value(ours + "gain")));
            values.emplace_back("Quality factor " + n, value(ours + "q"));
        }
        return values;
    }
    case MicFilter::Limiter:
        break;
    }
    // "Gain boost" off: on, the limiter turns its output up by as much as the ceiling
    // is below full scale, so a lower ceiling would make the mic louder.
    return {{"Gain boost", 0}, {"Threshold (G)", gain_from_db(value("ceiling"))}};
}

namespace {

// The chain as config.json stores it. Entries this version doesn't understand are
// dropped on the first edit, so positions mean the same here as in plan_mic.
void store_chain(json& config, const std::vector<MicStep>& steps)
{
    json chain = json::array();
    for (const auto& step : steps) {
        json entry = {{"filter", std::string(to_string(step.filter))}, {"on", step.on}};
        if (!step.settings.empty())
            entry["settings"] = step.settings;
        chain.push_back(std::move(entry));
    }
    if (!config["mic"].is_object())
        config["mic"] = json::object();
    config["mic"]["chain"] = std::move(chain);
}

// Validates a chain position, throwing std::invalid_argument if it's not in the list.
std::size_t checked_position(const std::vector<MicStep>& steps, int position)
{
    if (position < 0 || static_cast<std::size_t>(position) >= steps.size())
        throw std::invalid_argument("no filter at position " + std::to_string(position));
    return static_cast<std::size_t>(position);
}

}  // namespace

// Inserts a new, enabled filter at `position` (or the end) and stores the chain.
std::size_t add_mic_filter(json& config, std::string_view filter, int position)
{
    const auto parsed = parse_mic_filter(filter);
    if (!parsed)
        throw std::invalid_argument("no filter called '" + std::string(filter) + "'");
    auto steps = mic_chain(config);
    if (steps.size() >= kMaxMicChain)
        throw std::invalid_argument("the mic chain holds up to " + std::to_string(kMaxMicChain) + " filters");
    const std::size_t at =
        position < 0 || static_cast<std::size_t>(position) > steps.size() ? steps.size() : position;
    steps.insert(steps.begin() + static_cast<long>(at), MicStep{*parsed, true, {}});
    store_chain(config, steps);
    return at;
}

// Removes the filter at `position` and stores the chain.
void remove_mic_filter(json& config, int position)
{
    auto steps = mic_chain(config);
    steps.erase(steps.begin() + static_cast<long>(checked_position(steps, position)));
    store_chain(config, steps);
}

// Moves the filter at `from` to `to` (clamped to the list) and stores the chain.
void move_mic_filter(json& config, int from, int to)
{
    auto steps = mic_chain(config);
    const std::size_t index = checked_position(steps, from);
    MicStep moved = std::move(steps[index]);
    steps.erase(steps.begin() + static_cast<long>(index));
    const auto clamped = std::clamp<long>(to, 0, static_cast<long>(steps.size()));
    steps.insert(steps.begin() + clamped, std::move(moved));
    store_chain(config, steps);
}

// Switches the filter at `position` on or off and stores the chain.
void set_mic_filter_on(json& config, int position, bool on)
{
    auto steps = mic_chain(config);
    steps[checked_position(steps, position)].on = on;
    store_chain(config, steps);
}

// Stores a new value for one of a filter's settings, clamped to its range.
void set_mic_filter_value(json& config, int position, std::string_view key, double value)
{
    auto steps = mic_chain(config);
    MicStep& step = steps[checked_position(steps, position)];
    const MicControl* control = mic_control(step.filter, key);
    if (!control)
        throw std::invalid_argument("the " + std::string(to_string(step.filter)) + " has no setting '" +
                                    std::string(key) + "'");
    if (!std::isfinite(value))
        throw std::invalid_argument("the value must be a number");
    step.settings[std::string(key)] = std::clamp(value, control->min, control->max);
    store_chain(config, steps);
}

// Builds the list of plugin folders and finds the first copy of each filter's plugin.
MicPlugins find_mic_plugins(const std::vector<fs::path>& first)
{
    std::vector<fs::path> dirs = first;
    // Inside the Flatpak: RNNoise built into the app, and LSP from Flathub's extension.
    if (in_flatpak())
        for (const char* dir : {"/app/lib/ladspa", "/app/extensions/Plugins/LSP/ladspa"})
            dirs.emplace_back(dir);
    if (const char* ladspa_path = std::getenv("LADSPA_PATH"); ladspa_path && *ladspa_path) {
        std::istringstream parts(ladspa_path);
        for (std::string part; std::getline(parts, part, ':');)
            if (!part.empty())
                dirs.emplace_back(part);
    }
    if (const char* home = std::getenv("HOME"); home && *home)
        dirs.emplace_back(fs::path(home) / ".ladspa");
    const std::vector<std::string> system = {"/usr/lib/ladspa", "/usr/lib64/ladspa", "/usr/local/lib/ladspa",
                                             "/usr/lib/x86_64-linux-gnu/ladspa",
                                             "/usr/lib/aarch64-linux-gnu/ladspa"};
    for (const auto& dir : system)
        dirs.emplace_back(dir);
    // Inside a Flatpak, the system's own plugins are visible under /run/host (the app
    // reads the host's /usr). The plugins the Flatpak brings come first, above.
    if (in_flatpak())
        for (const auto& dir : system)
            dirs.emplace_back("/run/host" + dir);
    const auto exists = [](const fs::path& path) {
        std::error_code error;
        return fs::exists(path, error);
    };

    MicPlugins found;
    for (MicFilter filter : kMicFilters) {
        for (const auto& dir : dirs) {
            const fs::path candidate = dir / mic_plugin(filter).file;
            if (exists(candidate)) {
                found[filter] = candidate.string();
                break;
            }
        }
    }
    return found;
}

// The stage's node name inside the filter graph: "s<position>".
std::string MicStage::node() const
{
    return "s" + std::to_string(position);
}

// The measured latency a filter adds (RNNoise 10 ms, limiter 5.3 ms, others 0).
double mic_filter_delay_ms(MicFilter filter)
{
    switch (filter) {
    case MicFilter::Noise:
        return 10.0;
    case MicFilter::Limiter:
        return 5.3;
    case MicFilter::Gain:
    case MicFilter::Gate:
    case MicFilter::Expander:
    case MicFilter::Equalizer:
    case MicFilter::Compressor:
        break;
    }
    return 0.0;
}

// Adds up the latency of every filter in the plan.
double mic_chain_delay_ms(const MicPlan& plan)
{
    double total = 0.0;
    for (const auto& stage : plan.stages)
        total += mic_filter_delay_ms(stage.filter);
    return total;
}

// Builds the chain to run: the input plus every enabled filter whose plugin was found.
std::optional<MicPlan> plan_mic(const json& config, const std::optional<std::string>& input,
                                const MicPlugins& plugins)
{
    if (!mic_enabled(config) || !input || input->empty())
        return std::nullopt;
    MicPlan plan;
    plan.input = *input;
    const auto steps = mic_chain(config);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto plugin = plugins.find(steps[i].filter);
        if (steps[i].on && plugin != plugins.end())
            plan.stages.push_back({i + 1, steps[i].filter, plugin->second});
    }
    return plan;
}

// Renders the plan as a filter-chain module (or a plain loopback with no filters).
std::pair<std::string, std::string> mic_chain_module(const MicPlan& plan)
{
    // The capture side reads the mic; it's passive, so the mic is only open while
    // something listens (OBS, or the Monitor). The playback side plays into WaveMix
    // Mic's feed. Neither falls back to another device while its own isn't there.
    const std::string node(kMicChainNode);
    const std::string sides =
        " audio.position = [ MONO ] capture.props = { node.name = " + conf_string(node + "_in") +
        " target.object = " + conf_string(plan.input) +
        " node.passive = true node.dont-fallback = true } playback.props = { node.name = " + conf_string(node) +
        " media.name = " + conf_string(std::string(kMicLabel) + " chain") +
        " target.object = " + conf_string(kMicFeedSink) + " node.dont-fallback = true } }";
    const std::string description = "{ node.description = " + conf_string(std::string(kMicLabel) + " chain");
    if (plan.stages.empty())
        return {"libpipewire-module-loopback", description + sides};

    // Every filter in one graph, each one's output into the next one's input. All the
    // plugins name their audio ports Input and Output.
    std::string nodes, links;
    for (std::size_t i = 0; i < plan.stages.size(); ++i) {
        const MicStage& stage = plan.stages[i];
        nodes += " { type = ladspa name = " + stage.node() + " plugin = " + conf_string(stage.plugin) +
                 " label = " + conf_string(mic_plugin(stage.filter).label) + " }";
        if (i > 0)
            links += " { output = " + conf_string(plan.stages[i - 1].node() + ":Output") +
                     " input = " + conf_string(stage.node() + ":Input") + " }";
    }
    return {"libpipewire-module-filter-chain",
            description + " filter.graph = { nodes = [" + nodes + " ] links = [" + links + " ] }" + sides};
}

// The pipewire-pulse modules that make WaveMix Mic: a mono feed sink and a remap
// source reading its monitor.
std::vector<std::pair<std::string, std::string>> mic_device_modules()
{
    // wavemix.mic marks them as the mic's, so they're removed with it.
    return {
        {"module-null-sink", "sink_name=" + std::string(kMicFeedSink) + " channels=1 sink_properties=\"" +
                                 pa_props({{"device.description", std::string(kMicFeedLabel)},
                                           {"wavemix.channel", ""},
                                           {"wavemix.mic", "device"}}) +
                                 "\""},
        {"module-remap-source", "source_name=" + std::string(kMicSource) + " master=" + std::string(kMicFeedSink) +
                                    ".monitor channels=1 source_properties=\"" +
                                    pa_props({{"device.description", std::string(kMicLabel)},
                                              {"wavemix.mic", "device"}}) +
                                    "\""},
    };
}

}  // namespace wavemix::core
