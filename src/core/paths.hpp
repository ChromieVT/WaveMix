// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Where WaveMix keeps its files. See docs/contract/files.md#where-files-live.

#pragma once

#include <filesystem>
#include <string>

namespace wavemix::core {

struct Paths {
    std::filesystem::path config_dir;   // .../wavemix
    std::filesystem::path config_file;  // .../wavemix/config.json
    std::filesystem::path state_file;   // .../wavemix/state.json

    // The file pipewire-pulse reads at login to recreate every channel.
    std::filesystem::path login_conf;
};

// Builds the paths from two base folders:
//   config_home       WaveMix's own files (XDG_CONFIG_HOME)
//   host_config_home  the real ~/.config, where PipeWire looks for its config
// Outside a Flatpak the two are the same folder.
Paths make_paths(const std::filesystem::path& config_home,
                 const std::filesystem::path& host_config_home);

// The paths for this process, from the environment.
//
// Inside a Flatpak, XDG_CONFIG_HOME is the app's private folder
// (~/.var/app/io.github.ChromieVT.WaveMix/config), which is right for config.json and
// state.json. The login file has to go into the *host's* ~/.config instead, because
// that's where pipewire-pulse reads it.
Paths paths_from_environment();

// WaveMix's log file: $XDG_STATE_HOME/wavemix/wavemix.log (~/.local/state/wavemix/ by
// default; inside a Flatpak, the app's own ~/.var/app/<id>/.local/state/wavemix/).
std::filesystem::path log_file();

// True when running inside a Flatpak sandbox.
bool in_flatpak();

// The path of the running wavemix binary, for starting another copy of it (the daemon
// restarting itself, the window starting the daemon, the tray opening the window).
//
// Use this rather than running /proc/self/exe directly: Linux names a process after the
// file it was started from, so a copy started through /proc/self/exe shows up as "exe"
// in process lists instead of "wavemix". After an update replaced the binary, this is
// the path of the new one, so Restart picks up the update. Falls back to /proc/self/exe
// only if nothing is at that path any more (WaveMix was uninstalled while running),
// because starting *something* matters more than its name.
std::string self_executable();

// The folder holding WaveMix's hicolor icon theme ("<dir>/hicolor/16x16/apps/..."), or
// "" if there isn't one. Found relative to the running program, so it works wherever
// WaveMix is installed and needs no build path compiled in:
//   <bin>/../share/icons   installed (/usr, /usr/local, ~/.local, the Flatpak's /app)
//   <bin>/../data/icons    running from a build folder (Meson links it to the source)
std::string icon_theme_dir();

}  // namespace wavemix::core
