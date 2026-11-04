// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The tray icon, hosted by the daemon. Clicking it opens the mixer; right-clicking shows
// Open / Restart / Quit.
//
// It uses StatusNotifierItem, the freedesktop/KDE tray protocol that KDE Plasma,
// Cinnamon (XApp status applet), XFCE, Budgie, LXQt and GNOME with the AppIndicator
// extension all speak. It's plain D-Bus, so it needs no extra libraries:
//
//   /StatusNotifierItem  org.kde.StatusNotifierItem  the icon itself
//   /MenuBar             com.canonical.dbusmenu      its right-click menu, which the
//                                                    tray draws natively
//
// Both live on the daemon's own bus connection, and the item is registered with the
// tray (the StatusNotifierWatcher) by the connection's unique name. So no second bus
// name is needed, which keeps the Flatpak permissions to just talking to the watcher.
// If no tray is running yet, the icon registers as soon as one appears.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusMethodInvocation GDBusMethodInvocation;
typedef struct _GVariant GVariant;

namespace wavemix::daemon {

class Tray {
public:
    // Loads the icon's pixmaps, publishes the item and menu objects, and registers with
    // the tray whenever one is running.
    Tray(GDBusConnection* bus, std::function<void()> on_restart, std::function<void()> on_quit);
    // Stops watching for the tray and unpublishes both objects.
    ~Tray();

    // Not copyable (copy constructor and assignment deleted): it owns D-Bus registrations.
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

private:
    struct Callbacks;  // the C callbacks; defined in tray.cpp

    // One icon size, as tray hosts want it: ARGB32 pixels in network byte order.
    struct Pixmap {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> argb;
    };

    // The value of one StatusNotifierItem property, or nullptr if there's no such property.
    GVariant* item_property(const std::string& name) const;
    // The value of one dbusmenu property, or nullptr if there's no such property.
    GVariant* menu_property(const std::string& name) const;
    // Handles a click on the icon: any of them opens the mixer.
    void on_item_method(const std::string& method, GDBusMethodInvocation* invocation);
    // Handles the dbusmenu methods the tray host calls to draw the menu and report clicks.
    void on_menu_method(const std::string& method, GVariant* params, GDBusMethodInvocation* invocation);
    // Carries out a menu entry: Open now, Restart or Quit a moment later.
    void menu_clicked(int item);
    // Registers the item with the StatusNotifierWatcher under our unique bus name.
    void register_with_watcher();

    // The icon's pixmaps as the a(iiay) value the protocol wants.
    GVariant* pixmaps_variant() const;

    GDBusConnection* bus_;
    std::function<void()> on_restart_;
    std::function<void()> on_quit_;
    std::vector<Pixmap> pixmaps_;
    std::string icon_theme_path_;
    unsigned item_id_ = 0;
    unsigned menu_id_ = 0;
    unsigned watch_id_ = 0;
    bool registered_ = false;
};

// Starts the mixer window. WaveMix allows only one window, so a second start just
// raises the existing one.
//
// The command is this program itself (`wavemix` with no arguments). For testing with
// another window build, set WAVEMIX_WINDOW_COMMAND to the command that starts it.
void open_mixer();

}  // namespace wavemix::daemon
