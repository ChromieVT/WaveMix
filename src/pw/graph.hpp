// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// A snapshot of the parts of the PipeWire graph WaveMix cares about: sinks, playback
// streams, the links between them and the default output.
//
// This is a plain copy. Registry fills one in on the PipeWire thread and hands out
// copies, so the rest of the program can read it without locks.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/apps.hpp"

namespace wavemix::pw {

struct Node {
    uint32_t id = 0;
    std::string media_class;  // "Audio/Sink" or "Stream/Output/Audio"
    core::Props props;

    // From the node's Props param. Empty until PipeWire has sent it.
    std::vector<float> channel_volumes;  // linear gain per channel
    bool muted = false;

    // object.serial: the number pulse tools (pactl, pavucontrol) show as the stream
    // or sink "index", and the one WaveMix uses in GetState and AssignApp. Unlike
    // `id`, a serial is never reused, so a client acting on a stale one can't hit a
    // different stream by mistake. 0 if it isn't known yet.
    uint64_t serial() const;

    // The node's node.name property (a sink's name, as pulse called it), or "".
    std::string name() const;
    // The node's node.description, falling back to its name.
    std::string description() const;
    // True for an output device (media.class Audio/Sink).
    bool is_sink() const { return media_class == "Audio/Sink"; }
    // True for an app's playback stream (media.class Stream/Output/Audio).
    bool is_stream() const { return media_class == "Stream/Output/Audio"; }
    // True for an input device (media.class Audio/Source).
    bool is_source() const { return media_class == "Audio/Source"; }

    // The slider value (pulse scale), or nullopt before the volume is known.
    std::optional<double> slider() const;
};

struct Link {
    uint32_t id = 0;
    uint32_t output_node = 0;  // the node sending audio
    uint32_t input_node = 0;   // the node receiving it
};

struct Graph {
    std::map<uint32_t, Node> nodes;
    std::map<uint32_t, Link> links;
    std::string default_sink;    // node.name of the current default output, "" if none
    std::string default_source;  // node.name of the current default input, "" if none

    // The sink a stream is playing into, or nullptr if it isn't linked to one (yet).
    // A stream can have one link per channel (left, right, ...); they all go to the
    // same sink, so the first one found decides.
    const Node* sink_of(const Node& stream) const;

    // A sink by node.name.
    const Node* sink_named(const std::string& name) const;

    // A playback stream by object.serial (the "index" clients see), or nullptr.
    const Node* stream_with_serial(uint64_t serial) const;

    // The playback stream of the WaveMix loopback with this wavemix.link tag
    // ("game:personal"), or nullptr if that loopback doesn't exist.
    const Node* link_stream(const std::string& tag) const;

    // Hardware outputs: sinks that aren't WaveMix's own. A sink is ours if its name
    // starts with "wavemix_" or it carries a wavemix.channel property.
    std::vector<const Node*> hardware_sinks() const;

    // A source (an input, or WaveMix Mic) by node.name.
    const Node* source_named(const std::string& name) const;

    // Any node by node.name (a stream of the service's mic chain, say), or nullptr.
    const Node* node_named(const std::string& name) const;

    // Inputs to pick a microphone from: sources that aren't WaveMix's own (the mic
    // chain's stages are named "wavemix_..." and carry wavemix.mic).
    std::vector<const Node*> hardware_sources() const;

    // The source a capture stream is reading from, or nullptr if it isn't linked.
    const Node* source_of(const Node& capture) const;
};

}  // namespace wavemix::pw
