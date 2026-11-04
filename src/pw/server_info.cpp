// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "pw/server_info.hpp"

#include <pipewire/pipewire.h>

#include <memory>
#include <string_view>
#include <vector>

#include "pw/connection.hpp"

namespace wavemix::pw {

namespace {

// Everything the callbacks touch. Only used with the loop lock held, or after the
// syncs (when no more events come for it).
struct Probe {
    pw_registry* registry = nullptr;
    spa_hook registry_listener{};
    ServerInfo info;

    struct Bound {
        Probe* probe = nullptr;
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
    };
    std::vector<std::unique_ptr<Bound>> bound;
};

// Core info event: records the server's version.
void on_core_info(void* data, const pw_core_info* info)
{
    auto* bound = static_cast<Probe::Bound*>(data);
    if (info && info->version)
        bound->probe->info.server = info->version;
}

// Client info event: records WirePlumber's application.version (the first one seen).
void on_client_info(void* data, const pw_client_info* info)
{
    auto* bound = static_cast<Probe::Bound*>(data);
    const char* version = info && info->props ? spa_dict_lookup(info->props, "application.version") : nullptr;
    if (version && bound->probe->info.wireplumber.empty())
        bound->probe->info.wireplumber = version;
}

const pw_core_events kCoreEvents = [] {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.info = on_core_info;
    return events;
}();

const pw_client_events kClientEvents = [] {
    pw_client_events events{};
    events.version = PW_VERSION_CLIENT_EVENTS;
    events.info = on_client_info;
    return events;
}();

// Registry event: binds the core object and WirePlumber's client, to receive their
// info events. Every other object is ignored.
void on_global(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props)
{
    auto* probe = static_cast<Probe*>(data);
    const std::string_view kind = type ? type : "";
    const bool core = kind == PW_TYPE_INTERFACE_Core;
    const char* name = props ? spa_dict_lookup(props, PW_KEY_APP_NAME) : nullptr;
    const bool wireplumber = kind == PW_TYPE_INTERFACE_Client && name && std::string_view(name) == "WirePlumber";
    if (!core && !wireplumber)
        return;

    auto bound = std::make_unique<Probe::Bound>();
    bound->probe = probe;
    if (core) {
        bound->proxy = static_cast<pw_proxy*>(pw_registry_bind(probe->registry, id, type, PW_VERSION_CORE, 0));
        if (bound->proxy)
            pw_core_add_listener(reinterpret_cast<pw_core*>(bound->proxy), &bound->listener, &kCoreEvents,
                                 bound.get());
    } else {
        bound->proxy = static_cast<pw_proxy*>(pw_registry_bind(probe->registry, id, type, PW_VERSION_CLIENT, 0));
        if (bound->proxy)
            pw_client_add_listener(reinterpret_cast<pw_client*>(bound->proxy), &bound->listener, &kClientEvents,
                                   bound.get());
    }
    if (bound->proxy)
        probe->bound.push_back(std::move(bound));
}

const pw_registry_events kRegistryEvents = [] {
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = on_global;
    return events;
}();

}  // namespace

// Binds the two objects, waits two syncs for their info, then unbinds everything.
ServerInfo server_info(Connection& connection)
{
    Probe probe;
    probe.info.library = pw_get_library_version();
    {
        Connection::Lock lock(connection);
        probe.registry = pw_core_get_registry(connection.core(), PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(probe.registry, &probe.registry_listener, &kRegistryEvents, &probe);
    }
    // First sync: every object announced, and the two we want bound. Second: their details.
    connection.sync();
    connection.sync();

    Connection::Lock lock(connection);
    for (auto& bound : probe.bound) {
        spa_hook_remove(&bound->listener);
        pw_proxy_destroy(bound->proxy);
    }
    spa_hook_remove(&probe.registry_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(probe.registry));
    return probe.info;
}

}  // namespace wavemix::pw
