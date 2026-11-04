// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/paths.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>

namespace fs = std::filesystem;

namespace wavemix::core {

namespace {

// An environment variable, or an empty string if it's unset.
std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

// The user's home folder: $HOME, else the password database, else "/".
fs::path home_dir()
{
    if (auto home = env("HOME"); !home.empty())
        return home;
    // No $HOME (rare: some service managers): ask the password database instead.
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
        return pw->pw_dir;
    return "/";
}

}  // namespace

// $XDG_STATE_HOME/wavemix/wavemix.log, defaulting to ~/.local/state.
fs::path log_file()
{
    const std::string state_home = env("XDG_STATE_HOME");
    const fs::path base = state_home.empty() ? home_dir() / ".local" / "state" : fs::path(state_home);
    return base / "wavemix" / "wavemix.log";
}

// Detects a Flatpak sandbox by the /.flatpak-info file it always mounts.
bool in_flatpak()
{
    // Flatpak always mounts this file into the sandbox.
    return fs::exists("/.flatpak-info");
}

// Resolves /proc/self/exe to the binary's real path, minus any " (deleted)" suffix.
std::string self_executable()
{
    std::error_code error;
    std::string real = fs::read_symlink("/proc/self/exe", error).string();
    if (error)
        return "/proc/self/exe";

    // After a package update replaced the binary we're running, the link reads
    // "/usr/bin/wavemix (deleted)". The path without that suffix is the *new* binary,
    // which is exactly what a restart after an update should run.
    constexpr std::string_view kDeleted = " (deleted)";
    if (real.ends_with(kDeleted))
        real.erase(real.size() - kDeleted.size());
    if (fs::exists(real, error))
        return real;
    return "/proc/self/exe";  // uninstalled meanwhile: the running copy is all there is
}

// Looks for the hicolor icon theme next to the binary (installed or build layout).
std::string icon_theme_dir()
{
    const fs::path bin = fs::path(self_executable()).parent_path();
    for (const fs::path& candidate : {bin / ".." / "share" / "icons", bin / ".." / "data" / "icons"}) {
        std::error_code error;
        if (fs::exists(candidate / "hicolor" / "16x16" / "apps" / "io.github.ChromieVT.WaveMix.png", error))
            return fs::weakly_canonical(candidate, error).string();
    }
    return "";
}

// Derives every WaveMix file path from the two config base folders.
Paths make_paths(const fs::path& config_home, const fs::path& host_config_home)
{
    Paths p;
    p.config_dir = config_home / "wavemix";
    p.config_file = p.config_dir / "config.json";
    p.state_file = p.config_dir / "state.json";
    p.login_conf = host_config_home / "pipewire" / "pipewire-pulse.conf.d" / "60-wavemix.conf";
    return p;
}

// Builds the paths from XDG_CONFIG_HOME (and, in a Flatpak, HOST_XDG_CONFIG_HOME).
Paths paths_from_environment()
{
    auto xdg = env("XDG_CONFIG_HOME");
    const fs::path config_home = xdg.empty() ? home_dir() / ".config" : fs::path(xdg);

    fs::path host_config_home = config_home;
    if (in_flatpak()) {
        // Flatpak sets HOST_XDG_CONFIG_HOME when the host has its own XDG_CONFIG_HOME.
        auto host = env("HOST_XDG_CONFIG_HOME");
        host_config_home = host.empty() ? home_dir() / ".config" : fs::path(host);
    }
    return make_paths(config_home, host_config_home);
}

}  // namespace wavemix::core
