// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "plumbing/pulse_plumbing.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/config.hpp"
#include "core/login_conf.hpp"
#include "core/names.hpp"

namespace wavemix::plumbing {

namespace {

// The wavemix.link tag in a loopback's arguments ("" if it has none): it's in its
// sink_input_properties, ended by a space or the closing quote.
std::string link_tag_in(const std::string& argument)
{
    constexpr std::string_view key = "wavemix.link=";
    const auto at = argument.find(key);
    if (at == std::string::npos)
        return "";
    const auto start = at + key.size();
    return argument.substr(start, argument.find_first_of(" \"'", start) - start);
}

// object[key] if it's a string, else "".
std::string string_field(const core::json& object, const std::string& key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

// Keeps references to the objects it works through.
PulsePlumbing::PulsePlumbing(pw::Connection& connection, pw::Registry& registry, PulseControl& pulse)
    : connection_(connection), registry_(registry), pulse_(pulse)
{
}

// Syncs and re-reads the graph every 20 ms until `ready` holds or 2 seconds pass.
template <typename Predicate>
pw::Graph PulsePlumbing::wait_for(Predicate ready)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    for (;;) {
        connection_.sync();
        pw::Graph graph = registry_.snapshot();
        if (ready(graph) || std::chrono::steady_clock::now() > deadline)
            return graph;
        std::this_thread::sleep_for(20ms);
    }
}

// Loads a null sink for each missing mix and channel, then waits until they all show up.
std::vector<std::string> PulsePlumbing::ensure(const core::json& config)
{
    std::vector<std::string> created;
    // The Stream Mix is there with or without channels, so OBS never loses its source.
    const auto& channels = config.at("channels");
    std::vector<std::string> wanted_sinks;
    pw::Graph graph = wait_for([](const pw::Graph&) { return true; });

    for (const auto& [mix_sink, label] : {std::pair{core::kMixSink, core::kMixLabel},
                                          std::pair{core::kVodSink, core::kVodLabel}}) {
        if (mix_sink == core::kVodSink && !core::vod_enabled(config))
            continue;  // the VOD Mix is turned off
        if (!graph.sink_named(std::string(mix_sink))) {
            pulse_.load("module-null-sink", core::sink_args(mix_sink, label, ""));
            created.push_back("sink " + std::string(mix_sink));
        }
        wanted_sinks.emplace_back(mix_sink);
    }

    for (const auto& channel : channels) {
        const std::string id = string_field(channel, "id");
        const std::string sink = core::base_sink(id);
        wanted_sinks.push_back(sink);
        if (!graph.sink_named(sink)) {
            const std::string description = string_field(channel, "label") + " (WaveMix)";
            pulse_.load("module-null-sink", core::sink_args(sink, description, id));
            created.push_back("sink " + sink);
        }
    }

    if (!created.empty()) {
        graph = wait_for([&](const pw::Graph& g) {
            return std::all_of(wanted_sinks.begin(), wanted_sinks.end(),
                               [&](const std::string& s) { return g.sink_named(s) != nullptr; });
        });
    }

    return created;
}

// Loads the Personal Mix sink if it's missing and waits for it to appear.
std::vector<std::string> PulsePlumbing::ensure_personal()
{
    std::vector<std::string> created;
    const std::string sink(core::kPersonalSink);
    if (!wait_for([](const pw::Graph&) { return true; }).sink_named(sink)) {
        pulse_.load("module-null-sink", core::sink_args(sink, core::kPersonalLabel, ""));
        created.push_back("sink " + sink);
        wait_for([&](const pw::Graph& g) { return g.sink_named(sink) != nullptr; });
    }
    return created;
}

// Unloads the Personal Mix sink.
void PulsePlumbing::remove_personal()
{
    unload_matching({"sink_name=" + std::string(core::kPersonalSink) + " "});
}

// Unloads each module-loopback whose arguments carry a wavemix.link tag.
std::vector<std::string> PulsePlumbing::remove_loopbacks()
{
    std::vector<std::string> lines;

    for (const auto& module : pulse_.list()) {
        if (module.name != "module-loopback")
            continue;

        const std::string tag = link_tag_in(module.argument);
        if (tag.empty())
            continue;  // not one of ours

        pulse_.unload(module.index);
        lines.push_back("removed pipewire-pulse's loopback " + tag);
    }

    return lines;
}

// Unloads every pipewire-pulse module whose arguments contain one of `needles`,
// loopbacks before sinks.
void PulsePlumbing::unload_matching(const std::vector<std::string>& needles)
{
    // A module belongs to us when its argument string contains one of the needles.
    // Every needle ends in a space, and a space is added to the argument, so that
    // "sink_name=wavemix_game " can't match wavemix_game_2.
    std::vector<PulseModule> matching;
    for (auto& module : pulse_.list()) {
        const std::string argument = module.argument + " ";
        if (std::any_of(needles.begin(), needles.end(),
                        [&](const std::string& n) { return argument.find(n) != std::string::npos; }))
            matching.push_back(std::move(module));
    }
    // Loopbacks first: unloading a sink takes its loopbacks with it, and unloading
    // them in this order keeps the logs free of "already gone" noise.
    std::stable_partition(matching.begin(), matching.end(),
                          [](const PulseModule& m) { return m.name == "module-loopback"; });
    for (const auto& module : matching)
        pulse_.unload(module.index);  // false just means it went with its sink
}

// Unloads a channel's sink (its loopbacks go with it).
void PulsePlumbing::remove_channel(std::string_view channel_id)
{
    // Only the channel's sink: the mixes stay, even after the last channel.
    unload_matching({"sink_name=" + core::base_sink(channel_id) + " "});
}

// Loads WaveMix Mic's feed sink and source, each only if it's missing.
std::vector<std::string> PulsePlumbing::ensure_mic()
{
    // WaveMix Mic's feed first: WaveMix Mic reads its monitor.
    std::vector<std::string> created;
    for (const auto& [module, args] : core::mic_device_modules()) {
        const std::string name = args.substr(args.find('=') + 1, args.find(' ') - args.find('=') - 1);
        const auto there = [&](const pw::Graph& g) { return g.sink_named(name) || g.source_named(name); };
        if (there(wait_for([](const pw::Graph&) { return true; })))
            continue;
        pulse_.load(module, args);
        wait_for(there);
        created.push_back("mic " + name);
    }
    return created;
}

// Unloads WaveMix Mic's devices (everything tagged wavemix.mic).
void PulsePlumbing::remove_mic()
{
    // WaveMix Mic's devices carry wavemix.mic.
    unload_matching({"wavemix.mic="});
}

// Unloads the VOD Mix sink.
void PulsePlumbing::remove_vod()
{
    unload_matching({"sink_name=" + std::string(core::kVodSink) + " "});
}

}  // namespace wavemix::plumbing
