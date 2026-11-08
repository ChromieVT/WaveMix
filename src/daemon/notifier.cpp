// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "daemon/notifier.hpp"

#include <gio/gio.h>

#include "core/log.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"

namespace wavemix::daemon {

namespace {

constexpr const char* kServerName = "org.freedesktop.Notifications";
constexpr const char* kServerPath = "/org/freedesktop/Notifications";
constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kPortalInterface = "org.freedesktop.portal.Notification";

// What the notification itself does when clicked: opens the window.
constexpr const char* kDefaultAction = "default";

// NotificationClosed's reason when the user closed it (1 is expired, 3 is by us).
constexpr unsigned kClosedByUser = 2;

// Logs a failed call to the notification server or portal, once per reason.
void log_failure(const char* what, GError* error)
{
    core::log::warning("notify", std::string(what) + ": " + (error ? error->message : "no reply"));
}

// The server's urgency for a level: low 0, normal 1, critical 2.
guchar urgency(core::NotificationLevel level)
{
    return level == core::NotificationLevel::Error ? 2 : 1;
}

// The portal's priority for a level.
const char* priority(core::NotificationLevel level)
{
    return level == core::NotificationLevel::Error ? "high" : "normal";
}

}  // namespace

struct Notifier::Callbacks {
    // What a Notify call needs to remember until its reply.
    struct Pending {
        Notifier* self;
        std::shared_ptr<bool> alive;
        std::string id;
    };

    // The server's reply to Notify: the number of the notification it showed.
    static void sent(GObject* source, GAsyncResult* result, gpointer data)
    {
        std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (!reply) {
            log_failure("could not show a notification", error);
            if (error)
                g_error_free(error);
            return;
        }
        guint32 server_id = 0;
        g_variant_get(reply, "(u)", &server_id);
        g_variant_unref(reply);
        if (*pending->alive)
            pending->self->on_sent(pending->id, server_id);
    }

    // The reply to a call whose answer doesn't matter, only whether it failed.
    static void done(GObject* source, GAsyncResult* result, gpointer what)
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (reply) {
            g_variant_unref(reply);
        } else {
            log_failure(static_cast<const char*>(what), error);
            if (error)
                g_error_free(error);
        }
    }

    // org.freedesktop.Notifications.ActionInvoked (u id, s action).
    static void server_action(GDBusConnection*, const char*, const char*, const char*, const char*,
                              GVariant* params, gpointer data)
    {
        guint32 server_id = 0;
        const char* action = nullptr;
        g_variant_get(params, "(u&s)", &server_id, &action);
        static_cast<Notifier*>(data)->on_server_action(server_id, action);
    }

    // org.freedesktop.Notifications.NotificationClosed (u id, u reason).
    static void server_closed(GDBusConnection*, const char*, const char*, const char*, const char*,
                              GVariant* params, gpointer data)
    {
        guint32 server_id = 0;
        guint32 reason = 0;
        g_variant_get(params, "(uu)", &server_id, &reason);
        static_cast<Notifier*>(data)->on_server_closed(server_id, reason);
    }

    // org.freedesktop.portal.Notification.ActionInvoked (s id, s action, av parameter).
    static void portal_action(GDBusConnection*, const char*, const char*, const char*, const char*,
                              GVariant* params, gpointer data)
    {
        const char* id = nullptr;
        const char* action = nullptr;
        g_variant_get(params, "(&s&s@av)", &id, &action, nullptr);
        auto* self = static_cast<Notifier*>(data);
        self->on_action_(id, action);
    }
};

// Picks the portal inside a Flatpak, the server elsewhere, and listens for their signals.
Notifier::Notifier(GDBusConnection* bus, std::function<void(const std::string&, const std::string&)> on_action,
                   std::function<void(const std::string&)> on_closed)
    : bus_(bus), portal_(core::in_flatpak()), on_action_(std::move(on_action)), on_closed_(std::move(on_closed))
{
    if (portal_) {
        action_signal_ = g_dbus_connection_signal_subscribe(bus_, kPortalName, kPortalInterface, "ActionInvoked",
                                                            kPortalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            &Callbacks::portal_action, this, nullptr);
    } else {
        action_signal_ = g_dbus_connection_signal_subscribe(bus_, kServerName, kServerName, "ActionInvoked",
                                                            kServerPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            &Callbacks::server_action, this, nullptr);
        closed_signal_ = g_dbus_connection_signal_subscribe(bus_, kServerName, kServerName, "NotificationClosed",
                                                            kServerPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            &Callbacks::server_closed, this, nullptr);
    }
}

// Unsubscribes from the signals; replies still on their way are ignored.
Notifier::~Notifier()
{
    *alive_ = false;
    if (action_signal_)
        g_dbus_connection_signal_unsubscribe(bus_, action_signal_);
    if (closed_signal_)
        g_dbus_connection_signal_unsubscribe(bus_, closed_signal_);
}

// Builds the notification for the portal or the server and sends it, without waiting.
void Notifier::send(const core::Notification& notification)
{
    if (portal_) {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&props, "{sv}", "title", g_variant_new_string(notification.title.c_str()));
        g_variant_builder_add(&props, "{sv}", "body", g_variant_new_string(notification.body.c_str()));
        g_variant_builder_add(&props, "{sv}", "priority", g_variant_new_string(priority(notification.level)));
        g_variant_builder_add(&props, "{sv}", "default-action", g_variant_new_string(kDefaultAction));
        if (!notification.actions.empty()) {
            GVariantBuilder buttons;
            g_variant_builder_init(&buttons, G_VARIANT_TYPE("aa{sv}"));
            for (const auto& action : notification.actions) {
                GVariantBuilder button;
                g_variant_builder_init(&button, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&button, "{sv}", "label", g_variant_new_string(action.label.c_str()));
                g_variant_builder_add(&button, "{sv}", "action", g_variant_new_string(action.id.c_str()));
                g_variant_builder_add_value(&buttons, g_variant_builder_end(&button));
            }
            g_variant_builder_add(&props, "{sv}", "buttons", g_variant_builder_end(&buttons));
        }
        g_dbus_connection_call(bus_, kPortalName, kPortalPath, kPortalInterface, "AddNotification",
                               g_variant_new("(sa{sv})", notification.id.c_str(), &props), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &Callbacks::done,
                               const_cast<char*>("could not show a notification"));
        return;
    }

    // The server takes the buttons as a flat list: action, label, action, label, ...
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions, "s", kDefaultAction);
    g_variant_builder_add(&actions, "s", "Open WaveMix");
    for (const auto& action : notification.actions) {
        g_variant_builder_add(&actions, "s", action.id.c_str());
        g_variant_builder_add(&actions, "s", action.label.c_str());
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(std::string(core::kAppId).c_str()));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency(notification.level)));
    const auto known = server_ids_.find(notification.id);
    const guint32 replaces = known != server_ids_.end() ? known->second : 0;
    // A question stays until it's answered (0: never expires); information goes by itself.
    const gint32 timeout = notification.actions.empty() ? -1 : 0;
    g_dbus_connection_call(bus_, kServerName, kServerPath, kServerName, "Notify",
                           g_variant_new("(susssasa{sv}i)", "WaveMix", replaces,
                                         std::string(core::kAppId).c_str(), notification.title.c_str(),
                                         notification.body.c_str(), &actions, &hints, timeout),
                           G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &Callbacks::sent,
                           new Callbacks::Pending{this, alive_, notification.id});
}

// Asks the portal or the server to remove the notification, if one is shown.
void Notifier::withdraw(const std::string& id)
{
    if (portal_) {
        g_dbus_connection_call(bus_, kPortalName, kPortalPath, kPortalInterface, "RemoveNotification",
                               g_variant_new("(s)", id.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               &Callbacks::done, const_cast<char*>("could not remove a notification"));
        return;
    }
    const auto known = server_ids_.find(id);
    if (known == server_ids_.end())
        return;
    const guint32 server_id = known->second;
    server_ids_.erase(known);
    g_dbus_connection_call(bus_, kServerName, kServerPath, kServerName, "CloseNotification",
                           g_variant_new("(u)", server_id), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           &Callbacks::done, const_cast<char*>("could not remove a notification"));
}

// Remembers which server number belongs to which notification.
void Notifier::on_sent(const std::string& id, unsigned server_id)
{
    server_ids_[id] = server_id;
}

// A button (or the notification itself) was clicked on one of ours.
void Notifier::on_server_action(unsigned server_id, const std::string& action)
{
    for (const auto& [id, number] : server_ids_) {
        if (number == server_id) {
            const std::string notification = id;  // on_action_ may withdraw it, changing the map
            on_action_(notification, action);
            return;
        }
    }
}

// One of ours closed: forgets it, and if the user closed it, reports that.
void Notifier::on_server_closed(unsigned server_id, unsigned reason)
{
    for (auto it = server_ids_.begin(); it != server_ids_.end(); ++it) {
        if (it->second == server_id) {
            const std::string notification = it->first;
            server_ids_.erase(it);
            if (reason == kClosedByUser)
                on_closed_(notification);
            return;
        }
    }
}

}  // namespace wavemix::daemon
