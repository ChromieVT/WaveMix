// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/links.hpp"

#include "core/config.hpp"
#include "core/mic.hpp"

namespace wavemix::core {

namespace {

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

using Props = std::vector<std::pair<std::string, std::string>>;  // values already formatted

// Formats already-formatted props as a "{ key = value ... }" config block.
std::string props_block(const Props& props)
{
    std::string out = "{";
    for (const auto& [key, value] : props)
        out += " " + key + " = " + value;
    return out + " }";
}

// The capture side reads `from`: a sink's monitor (`sink` true) or a source. It's
// passive, so it doesn't keep that device awake when nothing plays.
Props capture_props(const std::string& node_name, std::string_view from, bool sink)
{
    Props props{
        {"node.name", conf_string(node_name)},
        {"target.object", conf_string(from)},
        {"node.passive", "true"},
        // While what it reads isn't there (pipewire-pulse restarting, say), it waits for
        // it, rather than reading some other device.
        {"node.dont-fallback", "true"},
    };
    if (sink)
        props.emplace_back("stream.capture.sink", "true");
    return props;
}

// Assembles a loopback module's arguments: description, channel layout, and the props
// of its capture and playback sides.
std::string module_args(std::string_view description, std::string_view position, const Props& capture,
                        const Props& playback)
{
    return "{ node.description = " + conf_string(description) + " audio.position = [ " + std::string(position) +
           " ] capture.props = " + props_block(capture) + " playback.props = " + props_block(playback) + " }";
}

// A loopback between two of WaveMix's sinks: a channel's to a mix (`mix` set), or the
// Personal Mix's to the output (`mix` empty).
struct Route {
    std::string tag;
    std::string from;
    std::string to;
    std::string channel;
    std::optional<Mix> mix;
};

// Every loopback `config` calls for: each channel's to its mixes, then "personal:out".
std::vector<Route> routes(const json& config, const std::optional<std::string>& personal_output)
{
    std::vector<Route> out;
    const auto found = config.find("channels");
    const json channels = found != config.end() && found->is_array() ? *found : json::array();
    for (const auto& channel : channels) {
        const auto id_field = channel.find("id");
        if (id_field == channel.end() || !id_field->is_string())
            continue;
        const std::string id = id_field->get<std::string>();
        for (Mix mix : kLinks) {
            if (mix == Mix::Vod && !wants_vod_link(config, channel))
                continue;  // no VOD Mix, or left out of it: no loopback at all
            const std::string_view destination = mix == Mix::Stream ? kMixSink
                                                 : mix == Mix::Vod  ? kVodSink
                                                                    : kPersonalSink;
            out.push_back({link_tag(id, mix), base_sink(id), std::string(destination), id, mix});
        }
    }
    if ((!channels.empty() || mic_enabled(config)) && personal_output)
        out.push_back({std::string(kPersonalOutTag), std::string(kPersonalSink), *personal_output, "", std::nullopt});
    return out;
}

}  // namespace

// The libpipewire-module-loopback arguments for one of a channel's loopbacks.
std::string link_module_args(std::string_view channel_id, Mix mix, std::string_view destination)
{
    const std::string name = link_node_name(channel_id, mix);
    // The playback side carries the slider: its volume *is* the slider's level.
    const Props playback{
        {"node.name", conf_string(name)},
        {"media.name", conf_string(link_media_name(channel_id, mix))},
        {"wavemix.link", conf_string(link_tag(channel_id, mix))},
        {"target.object", conf_string(destination)},
        // Its mix sink or nothing: while it isn't there, wait for it.
        {"node.dont-fallback", "true"},
    };
    return module_args(link_media_name(channel_id, mix), "FL FR",
                       capture_props(name + "_in", base_sink(channel_id), true), playback);
}

// The libpipewire-module-loopback arguments for the Personal Mix's loopback to the output.
std::string personal_out_module_args(std::string_view output)
{
    const std::string name = std::string(kPersonalSink) + "_out";
    const Props playback{
        {"node.name", conf_string(name)},
        {"media.name", conf_string(kPersonalLabel)},
        {"wavemix.link", conf_string(kPersonalOutTag)},
        {"target.object", conf_string(output)},
        // If the output device disappears, go quiet rather than suddenly playing the
        // Personal mix through some other device.
        {"node.dont-fallback", "true"},
    };
    return module_args(kPersonalLabel, "FL FR", capture_props(name + "_in", kPersonalSink, true), playback);
}

// The libpipewire-module-loopback arguments for the mic's Monitor into the Personal Mix.
std::string mic_monitor_module_args()
{
    const std::string name = std::string(kMicSource) + "_monitor_link";
    // Silence rather than the raw mic, if it can't be linked to WaveMix Mic.
    const Props capture = capture_props(name + "_in", kMicSource, false);
    const Props playback{
        {"node.name", conf_string(name)},
        {"media.name", conf_string("WaveMix " + std::string(kMicMonitorTag))},
        {"wavemix.link", conf_string(kMicMonitorTag)},
        {"target.object", conf_string(kPersonalSink)},
        {"node.dont-fallback", "true"},
    };
    return module_args("WaveMix " + std::string(kMicMonitorTag), "MONO", capture, playback);
}

// Lists the modules the service should host for `config` (see the header).
std::vector<LinkSpec> wanted_links(const json& config, const std::optional<std::string>& personal_output,
                                   const std::optional<MicPlan>& mic_plan)
{
    const bool mic = mic_enabled(config);
    std::vector<LinkSpec> links;
    for (const auto& route : routes(config, personal_output)) {
        if (route.mix)
            links.push_back({route.tag, link_module_args(route.channel, *route.mix, route.to), route.from,
                             route.to, link_node_name(route.channel, *route.mix) + "_in"});
        else
            links.push_back({route.tag, personal_out_module_args(route.to), route.from, route.to,
                             std::string(kPersonalSink) + "_out_in"});
    }
    if (mic)
        links.push_back({std::string(kMicMonitorTag), mic_monitor_module_args(), std::string(kMicSource),
                         std::string(kPersonalSink), std::string(kMicSource) + "_monitor_link_in"});
    if (mic && mic_plan) {
        auto [module, args] = mic_chain_module(*mic_plan);
        // Without PipeWire's filter-chain module (a runtime that doesn't ship it), the mic
        // goes to WaveMix Mic as it is, rather than not at all.
        std::string fallback;
        if (module != "libpipewire-module-loopback")
            fallback = mic_chain_module(MicPlan{mic_plan->input, {}}).second;
        links.push_back({std::string(kMicChainTag), std::move(args), mic_plan->input, std::string(kMicFeedSink),
                         std::string(kMicChainNode) + "_in", std::move(module), std::move(fallback)});
    }
    return links;
}

}  // namespace wavemix::core
