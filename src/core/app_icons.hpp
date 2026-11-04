// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Finding an app's icon from its playback stream.
//
// Few apps put an icon name on their audio stream, so the stream is matched to the
// app's menu entry (its .desktop file) instead, and that entry's Icon is used. A stream
// is matched by, in order:
//
//   1. application.icon_name (or icon-name), when the app sets one;
//   2. its Flatpak app ID, which is also its menu entry's ID;
//   3. its program (application.process.binary) or name (application.name), against
//      the entry's ID, StartupWMClass, program in Exec, and Name;
//   4. its name starting or ending with an entry's Name, for games whose audio has the
//      full title ("Tom Clancy's The Division 2") and whose shortcut a short one;
//   5. failing all that, Wine's icon for Windows programs (Wine, or Proton for a Steam
//      game without a desktop shortcut), so they're at least recognisable.
//
// StartupWMClass is how Wine shortcuts name their program ("cubismviewer5.exe"), and
// Name is how Steam shortcuts are found ("Grounded 2" for "Grounded2.exe"). Names are
// compared loosely: lowercase, ".exe" dropped, letters and digits only.
//
// Plain C++ on plain data, so it's unit-tested without PipeWire or a desktop.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/apps.hpp"

namespace wavemix::core {

struct DesktopEntry {
    std::string id;            // "firefox", "org.mozilla.firefox", "wine-Programs-Foo-Bar"
    std::string name;          // Name=
    std::string icon;          // Icon=: a theme icon name, or an absolute path
    std::string wm_class;      // StartupWMClass=
    std::string exec_program;  // the program Exec= runs, without its folder ("spotify")
};

// Parses one .desktop file. `id` is the desktop file ID (see load_desktop_entries).
// Returns nothing for files that aren't applications or have no icon.
std::optional<DesktopEntry> parse_desktop_entry(std::string id, std::string_view text);

// Every application entry in these folders, subfolders included. An entry's ID is its
// path inside the folder, with "/" turned into "-" and ".desktop" dropped, as the
// desktop entry spec says. When two folders have the same ID, the earlier folder wins,
// so list the user's own folder first.
std::vector<DesktopEntry> load_desktop_entries(const std::vector<std::filesystem::path>& dirs);

// The folders apps install menu entries to, the user's first: XDG_DATA_HOME and
// XDG_DATA_DIRS, plus ~/.local/share, Flatpak's exports and, inside a Flatpak, the
// host's /usr (seen at /run/host/usr), none of which a sandbox lists by itself.
std::vector<std::filesystem::path> application_dirs();

// The icon for a stream with these properties: a theme icon name or an absolute path,
// or nothing if no entry matches.
std::optional<std::string> icon_for(const Props& props, const std::vector<DesktopEntry>& entries);

}  // namespace wavemix::core
