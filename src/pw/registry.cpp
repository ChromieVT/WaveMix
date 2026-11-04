// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "pw/registry.hpp"

#include "core/log.hpp"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <iostream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace wavemix::pw {

namespace {

// The node types we follow. Everything else (sources, video, MIDI, ...) is ignored.
constexpr std::string_view kSinkClass = "Audio/Sink";
constexpr std::string_view kSourceClass = "Audio/Source";  // microphones, and the mic chain
constexpr std::string_view kPlaybackStreamClass = "Stream/Output/Audio";
constexpr std::string_view kCaptureStreamClass = "Stream/Input/Audio";  // the mic chain's inputs, among others

// The metadata key holding the current default output. Its value is JSON like
// {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}.
constexpr std::string_view kDefaultSinkKey = "default.audio.sink";
constexpr std::string_view kDefaultSourceKey = "default.audio.source";  // the default input

// A property's value from a PipeWire dict, or "" if it's missing.
std::string_view lookup(const spa_dict* props, const char* key)
{
    const char* value = props ? spa_dict_lookup(props, key) : nullptr;
    return value ? std::string_view(value) : std::string_view();
}

// Parses a whole-number id from a property, or 0 if it's missing or not a number.
uint32_t lookup_id(const spa_dict* props, const char* key)
{
    const auto text = lookup(props, key);
    uint32_t id = 0;
    for (char c : text) {
        if (c < '0' || c > '9')
            return 0;
        id = id * 10 + static_cast<uint32_t>(c - '0');
    }
    return id;
}

}  // namespace

struct Registry::BoundNode {
    Registry* owner = nullptr;
    uint32_t id = 0;
    pw_node* proxy = nullptr;
    spa_hook listener{};
};

struct Registry::BoundMetadata {
    Registry* owner = nullptr;
    pw_metadata* proxy = nullptr;
    spa_hook listener{};
};

// PipeWire calls these on its thread, with the loop lock held.
struct Registry::Callbacks {
    // ---- Registry: objects appearing and disappearing ----------------------------

    // A new object: binds audio nodes and the "default" metadata, and records links.
    static void on_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                          uint32_t /*version*/, const spa_dict* props)
    {
        auto* self = static_cast<Registry*>(data);

        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const auto media_class = lookup(props, PW_KEY_MEDIA_CLASS);
            if (media_class == kSinkClass || media_class == kSourceClass || media_class == kPlaybackStreamClass ||
                media_class == kCaptureStreamClass)
                self->bind_node(id, media_class);
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
            Link link;
            link.id = id;
            link.output_node = lookup_id(props, PW_KEY_LINK_OUTPUT_NODE);
            link.input_node = lookup_id(props, PW_KEY_LINK_INPUT_NODE);
            self->graph_.links[id] = link;
            self->changed();
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            if (lookup(props, PW_KEY_METADATA_NAME) == "default" && !self->default_metadata_)
                self->bind_default_metadata(id);
        }
    }

    // An object went away: drops it from the graph and unbinds it if it was bound.
    static void on_global_remove(void* data, uint32_t id)
    {
        auto* self = static_cast<Registry*>(data);
        const bool removed = self->graph_.nodes.erase(id) + self->graph_.links.erase(id) > 0;
        if (auto it = self->bound_nodes_.find(id); it != self->bound_nodes_.end()) {
            spa_hook_remove(&it->second->listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second->proxy));
            self->bound_nodes_.erase(it);
        }
        if (removed)
            self->changed();
    }

    // ---- Nodes: full properties, volume and mute ---------------------------------

    // A bound node's info: replaces its properties with the full set.
    static void on_node_info(void* data, const pw_node_info* info)
    {
        auto* bound = static_cast<BoundNode*>(data);
        if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props)
            return;
        Node& node = bound->owner->graph_.nodes[bound->id];
        node.props.clear();
        const spa_dict_item* item;
        spa_dict_for_each(item, info->props) {
            node.props.emplace(item->key, item->value ? item->value : "");
        }
        bound->owner->changed();
    }

    // A bound node's Props param: picks up its channel volumes and mute.
    static void on_node_param(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/,
                              uint32_t /*next*/, const spa_pod* param)
    {
        auto* bound = static_cast<BoundNode*>(data);
        if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param))
            return;

        Node& node = bound->owner->graph_.nodes[bound->id];
        bool changed = false;
        const spa_pod_prop* prop;
        SPA_POD_OBJECT_FOREACH(reinterpret_cast<const spa_pod_object*>(param), prop) {
            if (prop->key == SPA_PROP_channelVolumes) {
                // One linear gain per channel (left, right, ...).
                float volumes[SPA_AUDIO_MAX_CHANNELS];
                const uint32_t count = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
                                                          volumes, SPA_AUDIO_MAX_CHANNELS);
                node.channel_volumes.assign(volumes, volumes + count);
                changed = true;
            } else if (prop->key == SPA_PROP_mute) {
                bool muted = false;
                if (spa_pod_get_bool(&prop->value, &muted) == 0) {
                    node.muted = muted;
                    changed = true;
                }
            }
        }
        if (changed)
            bound->owner->changed();
    }

    // ---- Metadata: the default output and input -----------------------------------

    // The "default" metadata changed: updates the default sink and/or source name.
    static int on_metadata_property(void* data, uint32_t subject, const char* key,
                                    const char* /*type*/, const char* value)
    {
        auto* bound = static_cast<BoundMetadata*>(data);
        // key == nullptr means "everything was cleared".
        const bool sink = key && kDefaultSinkKey == key;
        const bool source = key && kDefaultSourceKey == key;
        if (subject != PW_ID_CORE || (key && !sink && !source))
            return 0;

        std::string name;
        if (value) {
            const auto parsed = nlohmann::json::parse(value, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_object() && parsed.contains("name") && parsed["name"].is_string())
                name = parsed["name"].get<std::string>();
        }
        if (source)
            bound->owner->graph_.default_source = name;
        else if (sink)
            bound->owner->graph_.default_sink = name;
        else {
            bound->owner->graph_.default_sink.clear();  // everything was cleared
            bound->owner->graph_.default_source.clear();
        }
        bound->owner->changed();
        return 0;
    }

    static inline const pw_registry_events kRegistryEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = &on_global,
        .global_remove = &on_global_remove,
    };

    static inline const pw_node_events kNodeEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = &on_node_info,
        .param = &on_node_param,
    };

    static inline const pw_metadata_events kMetadataEvents = {
        .version = PW_VERSION_METADATA_EVENTS,
        .property = &on_metadata_property,
    };
};

// Gets the server's registry and starts listening for objects.
Registry::Registry(Connection& connection)
    : connection_(connection), registry_listener_(std::make_unique<spa_hook>())
{
    Connection::Lock lock(connection_);
    registry_ = pw_core_get_registry(connection_.core(), PW_VERSION_REGISTRY, 0);
    if (!registry_)
        throw std::runtime_error("could not get the PipeWire registry");
    *registry_listener_ = {};
    pw_registry_add_listener(registry_, registry_listener_.get(), &Callbacks::kRegistryEvents, this);
}

// Unbinds every node and the metadata, then drops the registry.
Registry::~Registry()
{
    Connection::Lock lock(connection_);
    for (auto& [id, bound] : bound_nodes_) {
        spa_hook_remove(&bound->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(bound->proxy));
    }
    if (default_metadata_) {
        spa_hook_remove(&default_metadata_->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(default_metadata_->proxy));
    }
    spa_hook_remove(registry_listener_.get());
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
}

// Binds a node, adds it to the graph and subscribes to its Props param.
void Registry::bind_node(uint32_t id, std::string_view media_class)
{
    auto bound = std::make_unique<BoundNode>();
    bound->owner = this;
    bound->id = id;
    bound->proxy = static_cast<pw_node*>(
        pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!bound->proxy) {
        core::log::warning("pw", "could not watch PipeWire node " + std::to_string(id));
        return;
    }

    Node& node = graph_.nodes[id];
    node.id = id;
    node.media_class = std::string(media_class);

    pw_node_add_listener(bound->proxy, &bound->listener, &Callbacks::kNodeEvents, bound.get());
    // Ask to be told about the Props param now and every time it changes.
    uint32_t params[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(bound->proxy, params, 1);

    bound_nodes_[id] = std::move(bound);
    changed();
}

// Binds the "default" metadata object and listens for its properties.
void Registry::bind_default_metadata(uint32_t id)
{
    auto bound = std::make_unique<BoundMetadata>();
    bound->owner = this;
    bound->proxy = static_cast<pw_metadata*>(
        pw_registry_bind(registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (!bound->proxy) {
        core::log::warning("pw", "could not watch the default-device metadata");
        return;
    }
    pw_metadata_add_listener(bound->proxy, &bound->listener, &Callbacks::kMetadataEvents, bound.get());
    default_metadata_ = std::move(bound);
}

// Copies the graph under the lock.
Graph Registry::snapshot() const
{
    Connection::Lock lock(connection_);
    return graph_;
}

// Sets the callback for graph changes (under the lock).
void Registry::on_changed(std::function<void()> callback)
{
    Connection::Lock lock(connection_);
    on_changed_ = std::move(callback);
}

// Tells the owner the graph changed.
void Registry::changed()
{
    if (on_changed_)
        on_changed_();
}

namespace {

// Sends a Props param holding one property, built by `add` (e.g. a volume array).
template <typename AddProperty>
void send_props(pw_node* node, AddProperty add, size_t size = 1024)
{
    // A Props object: 1 KiB fits even 64 channel volumes; named controls ask for more.
    std::vector<uint8_t> buffer(size);
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer.data(), static_cast<uint32_t>(buffer.size()));
    spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    add(builder);
    const auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame));
    pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
}

}  // namespace

// Sends the slider's gain for every channel the node has (stereo if not yet known).
bool Registry::set_volume(uint32_t node_id, double slider)
{
    Connection::Lock lock(connection_);
    const auto bound = bound_nodes_.find(node_id);
    const auto node = graph_.nodes.find(node_id);
    if (bound == bound_nodes_.end() || node == graph_.nodes.end())
        return false;

    // Same gain on every channel the node has (a stereo stream has two). Before the
    // node has reported its volumes we don't know the count; stereo is the safe guess.
    const size_t count = node->second.channel_volumes.empty() ? 2 : node->second.channel_volumes.size();
    std::vector<float> gains(std::min<size_t>(count, SPA_AUDIO_MAX_CHANNELS), core::gain_from_slider(slider));
    send_props(bound->second->proxy, [&](spa_pod_builder& b) {
        spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, static_cast<uint32_t>(gains.size()), gains.data());
    });
    return true;
}

// Sends a mute or unmute to the node.
bool Registry::set_mute(uint32_t node_id, bool muted)
{
    Connection::Lock lock(connection_);
    const auto bound = bound_nodes_.find(node_id);
    if (bound == bound_nodes_.end())
        return false;
    send_props(bound->second->proxy, [&](spa_pod_builder& b) {
        spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(&b, muted);
    });
    return true;
}

// Sends the named controls as one "params" struct in the node's Props.
bool Registry::set_controls(uint32_t node_id, const std::vector<std::pair<std::string, double>>& controls)
{
    Connection::Lock lock(connection_);
    const auto bound = bound_nodes_.find(node_id);
    if (bound == bound_nodes_.end())
        return false;
    // Each control: its name, padded, and a float, each with a pod header.
    size_t size = 256;
    for (const auto& [name, value] : controls)
        size += name.size() + 48;
    send_props(bound->second->proxy, [&](spa_pod_builder& b) {
        // params = [ name, value, name, value, ... ]
        spa_pod_builder_prop(&b, SPA_PROP_params, 0);
        spa_pod_frame frame;
        spa_pod_builder_push_struct(&b, &frame);
        for (const auto& [name, value] : controls) {
            spa_pod_builder_string(&b, name.c_str());
            spa_pod_builder_float(&b, static_cast<float>(value));
        }
        spa_pod_builder_pop(&b, &frame);
    }, size);
    return true;
}

}  // namespace wavemix::pw
