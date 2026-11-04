// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "pw/graph.hpp"

#include <charconv>

#include "core/names.hpp"

namespace wavemix::pw {

// Parses the object.serial property; 0 if it's missing or not a number.
uint64_t Node::serial() const
{
    const auto it = props.find("object.serial");
    if (it == props.end())
        return 0;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    return error == std::errc() ? value : 0;
}

// Looks up node.name; "" if it isn't set.
std::string Node::name() const
{
    const auto it = props.find("node.name");
    return it == props.end() ? std::string() : it->second;
}

// Looks up node.description, using name() when it's missing or empty.
std::string Node::description() const
{
    const auto it = props.find("node.description");
    return it == props.end() || it->second.empty() ? name() : it->second;
}

// Converts the node's channel volumes to a slider value, once they're known.
std::optional<double> Node::slider() const
{
    if (channel_volumes.empty())
        return std::nullopt;
    return core::slider_from_gains(channel_volumes);
}

// Follows the stream's outgoing links to the first sink it reaches.
const Node* Graph::sink_of(const Node& stream) const
{
    for (const auto& [id, link] : links) {
        if (link.output_node != stream.id)
            continue;
        const auto target = nodes.find(link.input_node);
        if (target != nodes.end() && target->second.is_sink())
            return &target->second;
    }
    return nullptr;
}

// Searches the nodes for a sink with this node.name.
const Node* Graph::sink_named(const std::string& name) const
{
    for (const auto& [id, node] : nodes) {
        if (node.is_sink() && node.name() == name)
            return &node;
    }
    return nullptr;
}

// Searches the nodes for a playback stream with this object.serial.
const Node* Graph::stream_with_serial(uint64_t serial) const
{
    for (const auto& [id, node] : nodes) {
        if (node.is_stream() && node.serial() == serial)
            return &node;
    }
    return nullptr;
}

// Searches the nodes for a playback stream tagged wavemix.link = `tag`.
const Node* Graph::link_stream(const std::string& tag) const
{
    for (const auto& [id, node] : nodes) {
        const auto it = node.props.find("wavemix.link");
        if (node.is_stream() && it != node.props.end() && it->second == tag)
            return &node;
    }
    return nullptr;
}

// Lists the sinks that are neither named "wavemix_..." nor tagged wavemix.channel.
std::vector<const Node*> Graph::hardware_sinks() const
{
    std::vector<const Node*> out;
    for (const auto& [id, node] : nodes) {
        if (node.is_sink() && !node.name().starts_with(core::kPrefix) &&
            !node.props.contains("wavemix.channel"))
            out.push_back(&node);
    }
    return out;
}

// Searches the nodes for a source with this node.name.
const Node* Graph::source_named(const std::string& name) const
{
    for (const auto& [id, node] : nodes) {
        if (node.is_source() && node.name() == name)
            return &node;
    }
    return nullptr;
}

// Searches every node for one with this node.name.
const Node* Graph::node_named(const std::string& name) const
{
    for (const auto& [id, node] : nodes) {
        if (node.name() == name)
            return &node;
    }
    return nullptr;
}

// Follows the capture stream's incoming links back to the first source.
const Node* Graph::source_of(const Node& capture) const
{
    for (const auto& [id, link] : links) {
        if (link.input_node != capture.id)
            continue;
        const auto source = nodes.find(link.output_node);
        if (source != nodes.end() && source->second.is_source())
            return &source->second;
    }
    return nullptr;
}

// Lists the sources that are neither named "wavemix_..." nor tagged wavemix.mic.
std::vector<const Node*> Graph::hardware_sources() const
{
    std::vector<const Node*> out;
    for (const auto& [id, node] : nodes) {
        if (node.is_source() && !node.name().starts_with(core::kPrefix) && !node.props.contains("wavemix.mic"))
            out.push_back(&node);
    }
    return out;
}

}  // namespace wavemix::pw
