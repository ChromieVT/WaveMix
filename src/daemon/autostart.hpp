// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Starting the WaveMix service at login: the "Start at login" preference.
//
// Inside a Flatpak, the Background portal writes the login entry (a sandbox can't
// write ~/.config/autostart), and the desktop remembers the user's answer, so asking
// again on every start is harmless and keeps the entry's command line up to date.
//
// Otherwise WaveMix writes the user's own entry, ~/.config/autostart/
// io.github.ChromieVT.WaveMix.Daemon.desktop. Turned off, that entry says Hidden=true,
// which also hides an entry of the same name a package installed in /etc/xdg/autostart.
//
// Either way the login entry runs `wavemix --daemon --login`: --login tells the
// service it was started at login, so it can open the window unless "Start minimised"
// is on.

#pragma once

#include <gio/gio.h>

namespace wavemix::daemon {

// Turns starting the service at login on or off: through the Background portal inside
// a Flatpak, else by writing ~/.config/autostart/io.github.ChromieVT.WaveMix.Daemon.desktop.
void set_autostart(GDBusConnection* bus, bool enabled);

}  // namespace wavemix::daemon
