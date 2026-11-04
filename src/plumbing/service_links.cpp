// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "plumbing/service_links.hpp"

#include <pipewire/impl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <set>

#include "core/log.hpp"
#include "pw/connection.hpp"

namespace wavemix::plumbing {

struct ServiceLinks::Loaded {
    pw_impl_module* module = nullptr;  // null once PipeWire has torn it down
    std::string module_name;
    std::string args;
    std::chrono::steady_clock::time_point at;  // when it was loaded
    spa_hook listener{};
};

namespace {

// How long a module has to show its node before it counts as not running.
constexpr auto kStartup = std::chrono::seconds(2);

}  // namespace

// Runs on PipeWire's thread, with the loop lock held, like every access to `module`.
struct ServiceLinks::Callbacks {
    // The module was torn down by PipeWire: forgets the pointer so it's never used again.
    static void destroyed(void* data)
    {
        auto* loaded = static_cast<Loaded*>(data);
        spa_hook_remove(&loaded->listener);
        loaded->module = nullptr;
    }
    static inline const pw_impl_module_events kEvents = {
        .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = &destroyed, .free = nullptr,
        .initialized = nullptr, .registered = nullptr};
};

// Stores the connection; modules are loaded by sync().
ServiceLinks::ServiceLinks(pw::Connection& connection) : connection_(connection) {}

// Unloads every module that's still loaded.
ServiceLinks::~ServiceLinks()
{
    while (!loaded_.empty())
        unload(loaded_.begin()->first);
}

// Destroys the module with this tag (if PipeWire hasn't already) and forgets it.
void ServiceLinks::unload(const std::string& tag)
{
    const auto it = loaded_.find(tag);
    if (it == loaded_.end())
        return;
    pw::Connection::Lock lock(connection_);
    if (Loaded& loaded = *it->second; loaded.module) {
        spa_hook_remove(&loaded.listener);
        pw_impl_module_destroy(loaded.module);
    }
    loaded_.erase(it);
}

// Brings the loaded modules in line with `wanted` (see the header).
std::vector<std::string> ServiceLinks::sync(const std::vector<core::LinkSpec>& wanted, const pw::Graph& graph)
{
    std::vector<std::string> changes;

    // Forget the ones PipeWire tore down, and unload the ones no longer wanted. One whose
    // node has gone (its device disappeared: a module can lose its streams without
    // being torn down) counts as gone too, once it's had time to start.
    std::set<std::string> keep;
    std::map<std::string, std::string> nodes;
    for (const auto& link : wanted) {
        keep.insert(link.tag);
        nodes[link.tag] = link.node;
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> gone, stalled;
    {
        pw::Connection::Lock lock(connection_);
        for (const auto& [tag, loaded] : loaded_) {
            if (!loaded->module)
                gone.push_back(tag);
            else if (keep.contains(tag) && now - loaded->at > kStartup && !graph.node_named(nodes[tag]))
                stalled.push_back(tag);
        }
    }
    for (const auto& tag : gone)
        loaded_.erase(tag);
    for (const auto& tag : stalled)
        unload(tag);
    for (auto it = loaded_.begin(); it != loaded_.end();) {
        const std::string tag = (it++)->first;  // unload() erases it
        if (!keep.contains(tag)) {
            unload(tag);
            changes.push_back("unloaded " + tag);
        }
    }

    for (const auto& link : wanted) {
        const auto it = loaded_.find(link.tag);
        if (it != loaded_.end() && it->second->args == link.args && it->second->module_name == link.module)
            continue;
        // Both ends first: a loopback without them would only be torn down again.
        const bool from = graph.sink_named(link.from) || graph.source_named(link.from);
        if (!from || !graph.sink_named(link.to))
            continue;
        const bool moved = it != loaded_.end();
        unload(link.tag);

        auto loaded = std::make_unique<Loaded>();
        loaded->args = link.args;
        loaded->module_name = link.module;
        loaded->at = now;
        pw::Connection::Lock lock(connection_);
        loaded->module = pw_context_load_module(connection_.context(), link.module.c_str(), link.args.c_str(), nullptr);
        if (!loaded->module && !link.fallback_args.empty()) {
            core::log::warning("plumbing", "could not load " + link.module + " for " + link.tag + " (" +
                                               std::strerror(errno) + "): running it without, as a plain loopback");
            loaded->module = pw_context_load_module(connection_.context(), "libpipewire-module-loopback",
                                                    link.fallback_args.c_str(), nullptr);
        }
        if (!loaded->module) {
            core::log::error("plumbing",
                             "could not load " + link.module + " for " + link.tag + ": " + std::strerror(errno));
            continue;
        }
        pw_impl_module_add_listener(loaded->module, &loaded->listener, &Callbacks::kEvents, loaded.get());
        loaded_[link.tag] = std::move(loaded);
        changes.push_back((moved ? "reloaded " : "loaded ") + link.tag);
    }
    return changes;
}

}  // namespace wavemix::plumbing
