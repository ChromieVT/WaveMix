// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The loopbacks the service hosts (core/links.hpp): PipeWire loopback modules loaded
// into the service's own connection. They exist while it runs and go with it; the
// devices they connect stay in pipewire-pulse.
//
// Use it from one thread (the mixer's). Loading and unloading take the connection's
// loop lock themselves.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/links.hpp"
#include "pw/graph.hpp"

struct pw_impl_module;
struct spa_hook;

namespace wavemix::pw {
class Connection;
}

namespace wavemix::plumbing {

class ServiceLinks {
public:
    // Starts with no loopbacks loaded; sync() loads them.
    explicit ServiceLinks(pw::Connection& connection);
    // Unloads every loopback it loaded.
    ~ServiceLinks();

    // Not copyable (copy constructor and assignment deleted): it owns the loaded modules.
    ServiceLinks(const ServiceLinks&) = delete;
    ServiceLinks& operator=(const ServiceLinks&) = delete;

    // Makes the loaded modules match `wanted`: loads the missing ones whose two devices
    // are in `graph`, reloads any whose arguments changed (the Personal Mix's, when the
    // output device changes, or the mic chain) and unloads the rest. Returns a line per
    // change.
    //
    // A loopback whose devices disappear (pipewire-pulse restarting, say) is torn down
    // by PipeWire; it counts as missing from then on, so the next sync loads it again
    // once its devices are back.
    std::vector<std::string> sync(const std::vector<core::LinkSpec>& wanted, const pw::Graph& graph);

private:
    struct Loaded;
    struct Callbacks;  // the C callback PipeWire calls when a module goes; in the .cpp

    // Unloads the module with this tag, if it's loaded.
    void unload(const std::string& tag);

    pw::Connection& connection_;
    std::map<std::string, std::unique_ptr<Loaded>> loaded_;  // tag -> its module
};

}  // namespace wavemix::plumbing
