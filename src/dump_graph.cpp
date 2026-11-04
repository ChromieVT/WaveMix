// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "dump_graph.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "core/apps.hpp"
#include "core/names.hpp"
#include "pw/connection.hpp"
#include "pw/registry.hpp"

namespace wavemix {

namespace {

// "80%", "80% muted" or "?" (volume not reported yet), like `wavemix --status` prints.
std::string level(const pw::Node& node)
{
    const auto slider = node.slider();
    if (!slider)
        return "?";
    std::string text = std::to_string(static_cast<int>(std::lround(*slider * 100))) + "%";
    return node.muted ? text + " muted" : text;
}

// A node's property, or "" if it isn't set.
std::string prop(const pw::Node& node, const char* key)
{
    const auto it = node.props.find(key);
    return it == node.props.end() ? std::string() : it->second;
}

}  // namespace

// Connects, waits for the registry to fill in, and prints outputs, WaveMix sinks,
// sliders and apps.
int dump_graph()
{
    try {
        pw::Connection connection("wavemix-dump-graph");
        pw::Registry registry(connection);

        // First sync: the registry has announced every object and we've bound to the
        // nodes. Second: those nodes have sent their properties and volumes.
        if (!connection.sync() || !connection.sync()) {
            std::cerr << "wavemix: lost the PipeWire connection\n";
            return 1;
        }
        const pw::Graph graph = registry.snapshot();

        std::cout << "Default output: "
                  << (graph.default_sink.empty() ? "(none)" : graph.default_sink) << "\n\n";

        std::cout << "Hardware outputs:\n";
        for (const pw::Node* sink : graph.hardware_sinks()) {
            std::cout << "  [" << sink->id << "] " << sink->description() << "  (" << sink->name() << ")"
                      << (sink->name() == graph.default_sink ? "  <- default" : "") << "\n";
        }

        std::cout << "\nWaveMix sinks:\n";
        bool any_sink = false;
        for (const auto& [id, node] : graph.nodes) {
            if (node.is_sink() && node.name().starts_with(core::kPrefix)) {
                std::cout << "  [" << id << "] " << node.name() << "  \"" << node.description()
                          << "\"  " << level(node) << "\n";
                any_sink = true;
            }
        }
        if (!any_sink)
            std::cout << "  (none)\n";

        // The sliders: each loopback's playback stream carries a wavemix.link tag.
        std::cout << "\nChannel sliders (loopbacks):\n";
        bool any_link = false;
        for (const auto& [id, node] : graph.nodes) {
            const auto tag = prop(node, "wavemix.link");
            if (!node.is_stream() || tag.empty())
                continue;
            const pw::Node* target = graph.sink_of(node);
            std::cout << "  [" << id << "] " << tag << " -> " << (target ? target->name() : "(not linked)")
                      << "  " << level(node) << "\n";
            any_link = true;
        }
        if (!any_link)
            std::cout << "  (none)\n";

        std::cout << "\nApps playing:\n";
        bool any_app = false;
        for (const auto& [id, node] : graph.nodes) {
            if (!node.is_stream() || core::is_internal(node.props))
                continue;
            const pw::Node* target = graph.sink_of(node);
            const auto key = core::app_key(node.props);
            // "#serial" is the number pactl shows, and what AssignApp will take.
            std::cout << "  [" << id << " #" << node.serial() << "] " << core::app_name(node.props, id)
                      << "  key=" << key.value_or("(none)")
                      << "  -> " << (target ? target->name() : "(not linked)") << "\n";
            any_app = true;
        }
        if (!any_app)
            std::cout << "  (none)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "wavemix: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace wavemix
