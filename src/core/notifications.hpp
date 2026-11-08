// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Notifications: what WaveMix tells the user about (an app moved by another program,
// a device unplugged, a problem a check found). The service keeps them in a
// Notifications list; the window shows them as cards while it's on screen, and the
// desktop gets them otherwise (daemon/notifier.hpp).
//
// A notification is either a question, with buttons, which stays until it's answered
// or dismissed, or information, which goes away by itself after kInfoSeconds. Each has
// a kind ("test", "app-moved", ...): the user can ask not to see a kind again, and one
// of a muted kind is never raised. Raising one with the id of one that's still there
// replaces it, so a repeat doesn't stack.
//
// Plain C++ on plain data, so it's unit-tested.

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "core/json_file.hpp"

namespace wavemix::core {

// How long an information notification stays.
inline constexpr std::chrono::seconds kInfoSeconds{8};

// How a notification is shown: its icon, and on the desktop its urgency.
enum class NotificationLevel { Info, Warning, Error };

// One button: the action it sends back, and its label.
struct NotificationAction {
    std::string id;
    std::string label;
};

struct Notification {
    std::string id;    // unique while it's shown; raising the same id again replaces it
    std::string kind;  // what "Don't show again" mutes
    NotificationLevel level = NotificationLevel::Info;
    std::string title;
    std::string body;
    std::vector<NotificationAction> actions;  // none: information, which goes away by itself
    // Always shown on the desktop, even while the window is on screen, and never as a
    // card in it (the "Test on the desktop" button).
    bool desktop_only = false;
};

// "info", "warning" or "error", as GetState spells a level.
std::string_view to_string(NotificationLevel level);

// The kinds the user muted: settings "muted_notifications", a list of kind names.
// Anything that isn't a list of strings counts as none.
std::vector<std::string> muted_notification_kinds(const json& config);

// The notifications being shown, newest last.
class Notifications {
public:
    using Clock = std::chrono::steady_clock;

    // Adds `notification`, or replaces the one with its id. False (and nothing added) if
    // its kind is in `muted`.
    bool raise(Notification notification, const std::vector<std::string>& muted, Clock::time_point now = Clock::now());

    // Removes the notification with this id. False if there was none.
    bool dismiss(const std::string& id);

    // Removes every notification of a kind (it was just muted), and returns their ids.
    std::vector<std::string> dismiss_kind(const std::string& kind);

    // Removes information notifications older than kInfoSeconds, and returns their ids.
    std::vector<std::string> expire(Clock::time_point now = Clock::now());

    // The notification with this id, or nullptr.
    const Notification* find(const std::string& id) const;

    // All of them, oldest first.
    std::vector<Notification> all() const;

    // GetState's "notifications":
    // [{id, kind, level, title, body, actions: [{id, label}], desktop_only}].
    json to_json() const;

private:
    struct Entry {
        Notification notification;
        Clock::time_point raised;
    };
    // Erases the entries `gone` picks, and returns their ids.
    std::vector<std::string> remove_if(const std::function<bool(const Entry&)>& gone);

    std::vector<Entry> entries_;
};

}  // namespace wavemix::core
