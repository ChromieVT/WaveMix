// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/notifications.hpp"

#include "gui/helpers.hpp"
#include "gui/window.hpp"

namespace wavemix::gui {

using core::json;

namespace {

// The icon for a notification's level.
const char* level_icon(const std::string& level)
{
    if (level == "error")
        return "dialog-error-symbolic";
    if (level == "warning")
        return "dialog-warning-symbolic";
    return "dialog-information-symbolic";
}

}  // namespace

// A column of cards at the bottom right, clear of the edges; it only takes the space
// its cards need, so the mixer under it stays usable.
NotificationStack::NotificationStack(MixerWindow& window) : Gtk::Box(Gtk::Orientation::VERTICAL, 8), window_(window)
{
    add_css_class("wm-notifications");
    set_halign(Gtk::Align::END);
    set_valign(Gtk::Align::END);
    set_margin(16);
    set_visible(false);
}

// Rebuilds the cards when the notifications changed, and hides the stack when there are
// none.
void NotificationStack::update(const json& notifications)
{
    const std::string shown = notifications.dump();
    if (shown == shown_)
        return;
    shown_ = shown;
    remove_children(*this);
    bool any = false;
    for (const auto& notification : notifications) {
        if (notification.value("desktop_only", false))
            continue;  // shown on the desktop, not here
        append(*card(notification));
        any = true;
    }
    set_visible(any);
}

// Builds a card: icon, title, ⋮ and ×, the text, then the notification's buttons.
Gtk::Widget* NotificationStack::card(const json& notification)
{
    const std::string id = str(notification.value("id", json("")));
    const std::string kind = str(notification.value("kind", json("")));
    const std::string level = str(notification.value("level", json("info")));

    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    card->add_css_class("wm-notification");
    card->add_css_class("wm-notification-" + level);
    card->set_size_request(380, -1);

    auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(level_icon(level));
    icon->add_css_class("wm-notification-icon");
    icon->set_valign(Gtk::Align::START);
    auto* title = make_label(str(notification.value("title", json(""))), {"wm-notification-title"}, 0.0F, true);
    title->set_hexpand(true);
    title->set_max_width_chars(36);

    // ⋮: what can be done with this kind of notification.
    auto* more = flat_menu_button("view-more-symbolic", "More");
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* items = popover_box(*popover);
    menu_item(*popover, *items, "Don't show again", [this, kind] {
        json muted = window_.state().value("settings", json::object()).value("muted_notifications", json::array());
        if (!muted.is_array())
            muted = json::array();
        muted.push_back(kind);
        window_.call([&] { window_.client().set_setting("muted_notifications", muted); });
    });
    more->set_popover(*popover);
    more->set_valign(Gtk::Align::START);

    auto* close = Gtk::make_managed<Gtk::Button>();
    close->set_icon_name("window-close-symbolic");
    close->add_css_class("flat");
    close->set_tooltip_text("Dismiss");
    close->set_valign(Gtk::Align::START);
    close->signal_clicked().connect([this, id] { window_.call([&] { window_.client().dismiss_notification(id); }); });

    top->append(*icon);
    top->append(*title);
    top->append(*more);
    top->append(*close);
    card->append(*top);

    const std::string text = str(notification.value("body", json("")));
    if (!text.empty()) {
        auto* body = make_label(text, {"wm-notification-body"}, 0.0F, true);
        body->set_max_width_chars(44);
        body->set_margin_start(24);  // under the title, not the icon
        card->append(*body);
    }

    const json actions = notification.value("actions", json::array());
    if (!actions.empty()) {
        auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_margin_start(24);
        buttons->set_margin_top(4);
        for (const auto& action : actions) {
            const std::string action_id = str(action.value("id", json("")));
            auto* button = Gtk::make_managed<Gtk::Button>(str(action.value("label", json(""))));
            button->add_css_class("wm-notification-button");
            button->signal_clicked().connect([this, id, action_id] {
                window_.call([&] { window_.client().notification_action(id, action_id); });
            });
            buttons->append(*button);
        }
        card->append(*buttons);
    }
    return card;
}

}  // namespace wavemix::gui
