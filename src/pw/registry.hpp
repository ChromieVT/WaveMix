// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Keeps an up-to-date Graph by watching PipeWire's registry.
//
// The registry announces every object on the server as it appears and disappears.
// For the ones we care about we ask for more:
//   - sinks and playback streams: we bind to the node to get its full properties
//     (the announcement only has a few) and subscribe to its Props param for volume
//     and mute;
//   - links: the announcement already says which nodes they connect;
//   - the "default" metadata object: it says which sink is the default output.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pw/connection.hpp"
#include "pw/graph.hpp"

struct pw_registry;
struct spa_hook;

namespace wavemix::pw {

class Registry {
public:
    // Starts watching. Objects arrive over the next moment; call connection.sync()
    // (twice: once for the objects, once for the details we then ask for) before
    // relying on the graph being complete.
    explicit Registry(Connection& connection);
    // Removes the listeners and lets go of every bound object.
    ~Registry();

    // Not copyable (copy constructor and assignment deleted): it owns PipeWire listeners.
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // A copy of the current graph. Safe to call from any thread except PipeWire's own.
    Graph snapshot() const;

    // Called (on the PipeWire thread, with the lock held) whenever the graph changes.
    // Keep it short: typically it just schedules work on the main loop.
    void on_changed(std::function<void()> callback);

    // ---- Changing things ---------------------------------------------------------
    // All of these send a request and return straight away; the change shows up in
    // the graph (and on_changed fires) once PipeWire has applied it. They return false
    // if the object isn't known (any more).

    // Sets a node's volume on every channel. `slider` uses the pulse scale (1.0 = 100%);
    // see core::gain_from_slider.
    bool set_volume(uint32_t node_id, double slider);
    // Mutes or unmutes a node.
    bool set_mute(uint32_t node_id, bool muted);

    // Sets a filter-chain node's controls ("params" in its Props), by name: a plugin's
    // port names, for the nodes pipewire-pulse's module-ladspa-source makes.
    bool set_controls(uint32_t node_id, const std::vector<std::pair<std::string, double>>& controls);

    // Moving streams and setting the default output go through pipewire-pulse
    // instead (plumbing/pulse_control.hpp): inside a Flatpak, writes to the metadata
    // from here are dropped.

private:
    struct Callbacks;   // the C callbacks; defined in registry.cpp
    struct BoundNode;   // a node we've bound to, with its listener
    struct BoundMetadata;

    // Helpers for the callbacks. All of them run with the lock held.
    // Binds a sink, source or stream node to get its properties and volume.
    void bind_node(uint32_t id, std::string_view media_class);
    // Binds the "default" metadata object to learn the default sink and source.
    void bind_default_metadata(uint32_t id);
    // Calls the on_changed callback, if one is set.
    void changed();

    Connection& connection_;
    pw_registry* registry_ = nullptr;
    std::unique_ptr<spa_hook> registry_listener_;

    // Everything below is written by callbacks: only touch it with the lock held.
    Graph graph_;
    std::map<uint32_t, std::unique_ptr<BoundNode>> bound_nodes_;
    std::unique_ptr<BoundMetadata> default_metadata_;
    std::function<void()> on_changed_;
};

}  // namespace wavemix::pw
