// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The service's notifications (core/notifications.hpp), as cards floating over the
// bottom right of the window while it's on screen: an icon for the level, the title and
// text, a ⋮ menu with "Don't show again", a close button, and the notification's own
// buttons. Questions stay until they're answered or closed; information goes by itself
// (the service removes it). Off screen, the service sends them to the desktop instead.

#pragma once

#include <gtkmm.h>

#include <string>

#include "core/json_file.hpp"

namespace wavemix::gui {

class MixerWindow;

class NotificationStack : public Gtk::Box {
public:
    // Starts empty; update() fills it.
    explicit NotificationStack(MixerWindow& window);

    // Shows GetState's "notifications", newest at the bottom, except desktop-only ones.
    // The cards are only rebuilt when the list changed, so a card isn't replaced under
    // the pointer.
    void update(const core::json& notifications);

private:
    // One notification's card.
    Gtk::Widget* card(const core::json& notification);

    MixerWindow& window_;
    std::string shown_;  // the list the cards were built from
};

}  // namespace wavemix::gui
