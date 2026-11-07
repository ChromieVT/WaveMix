// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "client/client.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>

#include "core/names.hpp"
#include "core/paths.hpp"

namespace wavemix::client {

using core::json;

namespace {

const std::string kBusName(core::kBusName);
const std::string kObjectPath(core::kObjectPath);
const std::string kInterface(core::kInterface);

// Owns a GVariant reply and frees it at the end of the scope.
struct VariantDeleter {
    // Frees the GVariant.
    void operator()(GVariant* v) const { g_variant_unref(v); }
};
using Reply = std::unique_ptr<GVariant, VariantDeleter>;

}  // namespace

// Strips GDBus's "GDBus.Error:<name>: " prefix from an error message.
std::string clean_error(const char* message)
{
    std::string text = message ? message : "";
    if (text.starts_with("GDBus.Error:")) {
        const auto colon = text.find(": ");
        if (colon != std::string::npos)
            return text.substr(colon + 2);
    }
    return text;
}

// GIO calls these on the main loop.
struct Client::Callbacks {
    // StateChanged arrived: tells the client's on_changed callback.
    static void changed(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*,
                        gpointer data)
    {
        auto* self = static_cast<Client*>(data);
        if (self->on_changed_)
            self->on_changed_();
    }
    // The daemon's bus name gained an owner: reported as a change.
    static void appeared(GDBusConnection*, const char*, const char*, gpointer data)
    {
        changed(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, data);
    }
    // The daemon's bus name lost its owner: reported as a change.
    static void vanished(GDBusConnection*, const char*, gpointer data)
    {
        changed(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, data);
    }
    // The reply to an asynchronous call. `data` is a heap-allocated ErrorHandler.
    static void async_done(GObject* bus, GAsyncResult* result, gpointer data)
    {
        std::unique_ptr<ErrorHandler> on_error(static_cast<ErrorHandler*>(data));
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, &error);
        if (reply) {
            g_variant_unref(reply);
        } else {
            if (*on_error)
                (*on_error)(clean_error(error->message));
            g_error_free(error);
        }
    }
};

// Connects to the session bus and, with `autostart`, starts the daemon if needed.
Client::Client(bool autostart)
{
    GError* error = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus_) {
        const std::string message = error->message;
        g_error_free(error);
        throw ClientError("can't connect to the session bus: " + message);
    }
    if (autostart)
        ensure_daemon();
}

// Unsubscribes from the signal and the name watch, then releases the bus.
Client::~Client()
{
    if (signal_id_)
        g_dbus_connection_signal_unsubscribe(bus_, signal_id_);
    if (watch_id_)
        g_bus_unwatch_name(watch_id_);
    g_object_unref(bus_);
}

// Asks the bus whether anyone owns the daemon's name.
bool Client::running() const
{
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", kBusName.c_str()), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        nullptr);
    if (!reply)
        return false;
    gboolean has_owner = FALSE;
    g_variant_get(reply, "(b)", &has_owner);
    g_variant_unref(reply);
    return has_owner;
}

// Starts the daemon by D-Bus activation, or by spawning `wavemix --daemon`, and waits
// for it to own its name.
void Client::ensure_daemon(double timeout_seconds)
{
    if (running())
        return;

    // D-Bus activation: the bus starts the daemon from its installed service file.
    // That's the normal way, and inside a Flatpak the only one that works.
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName",
        g_variant_new("(su)", kBusName.c_str(), 0u), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (reply) {
        g_variant_unref(reply);
    } else {
        // No service file (running from a build folder, say): start it ourselves, in
        // its own session so it outlives us.
        g_error_free(error);
        const std::string self = core::self_executable();
        const gchar* argv[] = {self.c_str(), "--daemon", nullptr};
        g_spawn_async(nullptr, const_cast<gchar**>(argv), nullptr,
                      static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                      [](gpointer) { setsid(); }, nullptr, nullptr, nullptr);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (!running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Calls a method synchronously; throws ClientError with the daemon's message on failure.
GVariant* Client::call(const char* method, GVariant* parameters, const char* reply_type, int timeout_ms)
{
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(bus_, kBusName.c_str(), kObjectPath.c_str(), kInterface.c_str(),
                                                  method, parameters,
                                                  reply_type ? G_VARIANT_TYPE(reply_type) : nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &error);
    if (!reply) {
        const std::string message = clean_error(error->message);
        g_error_free(error);
        throw ClientError(message);
    }
    if (!reply_type) {
        g_variant_unref(reply);  // an empty () reply
        return nullptr;
    }
    return reply;
}

// Calls a method asynchronously; the reply is handled by Callbacks::async_done.
void Client::call_async(const char* method, GVariant* parameters, ErrorHandler on_error)
{
    g_dbus_connection_call(bus_, kBusName.c_str(), kObjectPath.c_str(), kInterface.c_str(), method, parameters,
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &Callbacks::async_done,
                           new ErrorHandler(std::move(on_error)));
}

// Calls GetState and parses the JSON it returns.
json Client::state()
{
    Reply reply(call("GetState", nullptr, "(s)"));
    const char* text = nullptr;
    g_variant_get(reply.get(), "(&s)", &text);
    const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        throw ClientError("the daemon sent a state that isn't valid JSON");
    return parsed;
}

// Calls AddChannel and returns the new channel's ID.
std::string Client::add_channel(const std::string& label, const std::string& icon)
{
    Reply reply(call("AddChannel", g_variant_new("(ss)", label.c_str(), icon.c_str()), "(s)"));
    const char* id = nullptr;
    g_variant_get(reply.get(), "(&s)", &id);
    return id;
}

// Calls RemoveChannel.
void Client::remove_channel(const std::string& id)
{
    call("RemoveChannel", g_variant_new("(s)", id.c_str()));
}

// Calls RenameChannel.
void Client::rename_channel(const std::string& id, const std::string& label, const std::string& icon)
{
    call("RenameChannel", g_variant_new("(sss)", id.c_str(), label.c_str(), icon.c_str()));
}

// Calls MoveChannel.
void Client::move_channel(const std::string& id, int position)
{
    call("MoveChannel", g_variant_new("(si)", id.c_str(), position));
}

// Calls AssignApp.
void Client::assign_app(uint32_t stream, const std::string& channel)
{
    call("AssignApp", g_variant_new("(us)", stream, channel.c_str()));
}

// Calls SetInVod.
void Client::set_in_vod(const std::string& channel, bool in_vod)
{
    call("SetInVod", g_variant_new("(sb)", channel.c_str(), in_vod));
}

// Calls SetChannelEnabled.
void Client::set_channel_enabled(const std::string& channel, bool enabled)
{
    call("SetChannelEnabled", g_variant_new("(sb)", channel.c_str(), enabled));
}

// Calls SwitchProfile.
void Client::switch_profile(const std::string& name)
{
    call("SwitchProfile", g_variant_new("(s)", name.c_str()));
}

// Calls SaveProfileAs.
void Client::save_profile_as(const std::string& name)
{
    call("SaveProfileAs", g_variant_new("(s)", name.c_str()));
}

// Calls RenameProfile.
void Client::rename_profile(const std::string& name, const std::string& new_name)
{
    call("RenameProfile", g_variant_new("(ss)", name.c_str(), new_name.c_str()));
}

// Calls DeleteProfile.
void Client::delete_profile(const std::string& name)
{
    call("DeleteProfile", g_variant_new("(s)", name.c_str()));
}

// Calls ExportProfile and returns the export file's text.
std::string Client::export_profile(const std::string& name)
{
    Reply reply(call("ExportProfile", g_variant_new("(s)", name.c_str()), "(s)"));
    const char* text = nullptr;
    g_variant_get(reply.get(), "(&s)", &text);
    return text;
}

// Calls ImportProfile and returns the name the profile was stored under.
std::string Client::import_profile(const std::string& text)
{
    Reply reply(call("ImportProfile", g_variant_new("(s)", text.c_str()), "(s)"));
    const char* name = nullptr;
    g_variant_get(reply.get(), "(&s)", &name);
    return name;
}

// Calls SetSetting with the value JSON-encoded.
void Client::set_setting(const std::string& key, const json& value)
{
    // Settings travel JSON-encoded, so any type fits the one D-Bus signature.
    call("SetSetting", g_variant_new("(ss)", key.c_str(), value.dump().c_str()));
}

// Calls SetMicInput.
void Client::set_mic_input(const std::string& device)
{
    call("SetMicInput", g_variant_new("(s)", device.c_str()));
}

// Calls SetMicEnabled.
void Client::set_mic_enabled(bool enabled)
{
    call("SetMicEnabled", g_variant_new("(b)", enabled));
}

// Calls AddMicFilter and returns the position the filter went to.
int Client::add_mic_filter(const std::string& filter, int position)
{
    Reply reply(call("AddMicFilter", g_variant_new("(si)", filter.c_str(), position), "(i)"));
    int added_at = 0;
    g_variant_get(reply.get(), "(i)", &added_at);
    return added_at;
}

// Calls RemoveMicFilter.
void Client::remove_mic_filter(int position)
{
    call("RemoveMicFilter", g_variant_new("(i)", position));
}

// Calls MoveMicFilter.
void Client::move_mic_filter(int from, int to)
{
    call("MoveMicFilter", g_variant_new("(ii)", from, to));
}

// Calls SetMicFilterOn.
void Client::set_mic_filter_on(int position, bool on)
{
    call("SetMicFilterOn", g_variant_new("(ib)", position, on));
}

// Calls Reset, with a longer timeout since it can remove many channels.
void Client::reset()
{
    call("Reset", nullptr, nullptr, 15000);  // removing many channels takes a moment
}

// Calls Restart.
void Client::restart()
{
    call("Restart", nullptr);
}

// Calls Quit.
void Client::quit()
{
    call("Quit", nullptr);
}

// Sends SetVolume without waiting.
void Client::set_volume(const std::string& channel, const std::string& mix, double volume, ErrorHandler on_error)
{
    call_async("SetVolume", g_variant_new("(ssd)", channel.c_str(), mix.c_str(), volume), std::move(on_error));
}

// Sends SetMute without waiting.
void Client::set_mute(const std::string& channel, const std::string& mix, bool muted, ErrorHandler on_error)
{
    call_async("SetMute", g_variant_new("(ssb)", channel.c_str(), mix.c_str(), muted), std::move(on_error));
}

// Sends SetMicVolume without waiting.
void Client::set_mic_volume(const std::string& slider, double volume, ErrorHandler on_error)
{
    call_async("SetMicVolume", g_variant_new("(sd)", slider.c_str(), volume), std::move(on_error));
}

// Sends SetMicMute without waiting.
void Client::set_mic_mute(const std::string& slider, bool muted, ErrorHandler on_error)
{
    call_async("SetMicMute", g_variant_new("(sb)", slider.c_str(), muted), std::move(on_error));
}

// Sends SetMicFilterValue without waiting.
void Client::set_mic_filter_value(int position, const std::string& key, double value, ErrorHandler on_error)
{
    call_async("SetMicFilterValue", g_variant_new("(isd)", position, key.c_str(), value), std::move(on_error));
}

// Sends GetMeters without waiting and passes the parsed reply to `on_reply`.
void Client::get_meters(std::function<void(const json&)> on_reply)
{
    using Handler = std::function<void(const json&)>;
    g_dbus_connection_call(
        bus_, kBusName.c_str(), kObjectPath.c_str(), kInterface.c_str(), "GetMeters", nullptr,
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
        [](GObject* bus, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Handler> handler(static_cast<Handler*>(data));
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, nullptr);
            if (!reply)
                return;  // no daemon, or an older one: no meters
            const char* text = nullptr;
            g_variant_get(reply, "(&s)", &text);
            const json meters = json::parse(text, nullptr, /*allow_exceptions=*/false);
            g_variant_unref(reply);
            if (!meters.is_discarded())
                (*handler)(meters);
        },
        new Handler(std::move(on_reply)));
}

// Stores the callback and, the first time, subscribes to StateChanged and watches the
// daemon's bus name.
void Client::on_changed(std::function<void()> callback)
{
    on_changed_ = std::move(callback);
    if (signal_id_)
        return;  // already listening
    signal_id_ = g_dbus_connection_signal_subscribe(bus_, kBusName.c_str(), kInterface.c_str(), "StateChanged",
                                                    kObjectPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                    &Callbacks::changed, this, nullptr);
    // The daemon appearing or going away (a restart, a crash) changes things too.
    watch_id_ = g_bus_watch_name_on_connection(bus_, kBusName.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               &Callbacks::appeared, &Callbacks::vanished, this, nullptr);
}

}  // namespace wavemix::client
