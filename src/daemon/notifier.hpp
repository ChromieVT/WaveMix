// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The service's notifications (core/notifications.hpp) on the desktop, sent while the
// window isn't on screen to show them as cards.
//
// Inside a Flatpak they go through the Notification portal, which needs no permission;
// elsewhere straight to org.freedesktop.Notifications. Both report button presses back
// to this process, so the service answers them itself, whether or not a window is open.
// (GLib's GNotification would send them to the app's GApplication instead, which for
// WaveMix is the window.) Clicking the notification itself is the "default" action:
// the service opens the window.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "core/notifications.hpp"

typedef struct _GDBusConnection GDBusConnection;

namespace wavemix::daemon {

class Notifier {
public:
    // `on_action(id, action)`: a button, or "default" for the notification itself.
    // `on_closed(id)`: the user closed it without answering.
    Notifier(GDBusConnection* bus, std::function<void(const std::string&, const std::string&)> on_action,
             std::function<void(const std::string&)> on_closed);
    // Stops listening; notifications already shown stay.
    ~Notifier();

    // Not copyable (copy constructor and assignment deleted): it owns signal subscriptions.
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    // Shows `notification` on the desktop, replacing what it showed for it before.
    void send(const core::Notification& notification);
    // Takes back what's shown on the desktop for this notification, if anything.
    void withdraw(const std::string& id);

private:
    struct Callbacks;  // the C callbacks; defined in notifier.cpp

    // The notification server's answer to Notify: the number it gave the notification.
    void on_sent(const std::string& id, unsigned server_id);
    // org.freedesktop.Notifications' ActionInvoked and NotificationClosed signals.
    void on_server_action(unsigned server_id, const std::string& action);
    void on_server_closed(unsigned server_id, unsigned reason);

    GDBusConnection* bus_;
    bool portal_;
    std::function<void(const std::string&, const std::string&)> on_action_;
    std::function<void(const std::string&)> on_closed_;
    unsigned action_signal_ = 0;
    unsigned closed_signal_ = 0;
    std::map<std::string, unsigned> server_ids_;  // notification id -> the server's number (not the portal)
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed, for late replies
};

}  // namespace wavemix::daemon
