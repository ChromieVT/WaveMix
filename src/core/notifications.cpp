// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/notifications.hpp"

#include <algorithm>

namespace wavemix::core {

// The GetState spelling of a level.
std::string_view to_string(NotificationLevel level)
{
    switch (level) {
    case NotificationLevel::Warning:
        return "warning";
    case NotificationLevel::Error:
        return "error";
    case NotificationLevel::Info:
        break;
    }
    return "info";
}

// Reads settings "muted_notifications", keeping only its strings.
std::vector<std::string> muted_notification_kinds(const json& config)
{
    std::vector<std::string> kinds;
    const auto settings = config.find("settings");
    if (settings == config.end() || !settings->is_object())
        return kinds;
    const auto muted = settings->find("muted_notifications");
    if (muted == settings->end() || !muted->is_array())
        return kinds;
    for (const auto& kind : *muted)
        if (kind.is_string())
            kinds.push_back(kind.get<std::string>());
    return kinds;
}

// Refuses a muted kind; otherwise replaces the entry with the same id, or appends.
bool Notifications::raise(Notification notification, const std::vector<std::string>& muted, Clock::time_point now)
{
    if (std::find(muted.begin(), muted.end(), notification.kind) != muted.end())
        return false;
    // A replaced one moves to the end, as the newest, and its clock starts again.
    dismiss(notification.id);
    entries_.push_back({std::move(notification), now});
    return true;
}

// Erases the entry with this id.
bool Notifications::dismiss(const std::string& id)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.notification.id == id; });
    if (it == entries_.end())
        return false;
    entries_.erase(it);
    return true;
}

// Erases the entries `gone` picks, returning their ids.
std::vector<std::string> Notifications::remove_if(const std::function<bool(const Entry&)>& gone)
{
    std::vector<std::string> ids;
    std::erase_if(entries_, [&](const Entry& e) {
        if (!gone(e))
            return false;
        ids.push_back(e.notification.id);
        return true;
    });
    return ids;
}

// Erases every entry of this kind.
std::vector<std::string> Notifications::dismiss_kind(const std::string& kind)
{
    return remove_if([&](const Entry& e) { return e.notification.kind == kind; });
}

// Erases information notifications (no buttons) raised at least kInfoSeconds ago.
std::vector<std::string> Notifications::expire(Clock::time_point now)
{
    return remove_if([&](const Entry& e) { return e.notification.actions.empty() && now - e.raised >= kInfoSeconds; });
}

// Searches the entries for this id.
const Notification* Notifications::find(const std::string& id) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.notification.id == id; });
    return it == entries_.end() ? nullptr : &it->notification;
}

// Copies the notifications out, oldest first.
std::vector<Notification> Notifications::all() const
{
    std::vector<Notification> out;
    for (const auto& entry : entries_)
        out.push_back(entry.notification);
    return out;
}

// Lists the notifications as GetState shows them, oldest first.
json Notifications::to_json() const
{
    json out = json::array();
    for (const auto& entry : entries_) {
        const Notification& n = entry.notification;
        json actions = json::array();
        for (const auto& action : n.actions)
            actions.push_back({{"id", action.id}, {"label", action.label}});
        out.push_back({{"id", n.id},
                       {"kind", n.kind},
                       {"level", std::string(to_string(n.level))},
                       {"title", n.title},
                       {"body", n.body},
                       {"actions", actions},
                       {"desktop_only", n.desktop_only}});
    }
    return out;
}

}  // namespace wavemix::core
