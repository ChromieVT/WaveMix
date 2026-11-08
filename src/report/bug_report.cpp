// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "report/bug_report.hpp"

#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "build_config.hpp"
#include "core/apps.hpp"
#include "core/config.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"
#include "core/profiles.hpp"
#include "pw/connection.hpp"
#include "pw/registry.hpp"
#include "pw/server_info.hpp"

namespace fs = std::filesystem;

namespace wavemix::report {

namespace {

constexpr size_t kLogLines = 300;

// An environment variable, or "(not set)" if it's unset or empty.
std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value ? value : "(not set)";
}

// A whole file's contents, or "" if it can't be read.
std::string read_file(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
        return "";
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A KEY=value or KEY="value" line from an os-release style file.
std::string key_value(const std::string& text, const std::string& key)
{
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (!line.starts_with(key + "="))
            continue;
        std::string value = line.substr(key.size() + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return "";
}

// A node's level as "85%" (plus " muted"), or "?" before its volume is known.
std::string percent(const pw::Node& node)
{
    const auto slider = node.slider();
    std::string text = slider ? std::to_string(static_cast<int>(std::lround(*slider * 100))) + "%" : "?";
    return node.muted ? text + " muted" : text;
}

// ---- Sections ------------------------------------------------------------------------

// "## WaveMix": the version, how it was installed, and what the service says (asked
// without starting it).
void wavemix_section(std::ostream& out)
{
    out << "## WaveMix\n\n";
    out << "Version: " WAVEMIX_VERSION "\n";

    std::string installed;
    const std::string exe = core::self_executable();
    if (core::in_flatpak()) {
        const std::string info = read_file("/.flatpak-info");
        const std::string commit = key_value(info, "app-commit");
        installed = "Flatpak" + (commit.empty() ? std::string() : " (commit " + commit.substr(0, 12) + ")");
    } else if (exe.starts_with("/usr/bin/")) {
        installed = "package (AUR or your distribution), " + exe;
    } else if (exe.starts_with("/usr/local/")) {
        installed = "install.sh, system-wide, " + exe;
    } else if (exe.find("/.local/") != std::string::npos) {
        installed = "install.sh, for this user, " + exe;
    } else {
        installed = "run from " + exe + " (a build folder?)";
    }
    out << "Installed: " << installed << "\n";

    // Is the service there, and what does it say? Asked without starting it.
    std::string service = "not running";
    if (GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
        const std::string name(core::kBusName);
        GVariant* owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                      "org.freedesktop.DBus", "NameHasOwner",
                                                      g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(b)"),
                                                      G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        gboolean running = FALSE;
        if (owned) {
            g_variant_get(owned, "(b)", &running);
            g_variant_unref(owned);
        }
        if (running) {
            GVariant* reply = g_dbus_connection_call_sync(
                bus, name.c_str(), std::string(core::kObjectPath).c_str(), std::string(core::kInterface).c_str(),
                "GetState", nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, nullptr);
            service = "running, but not answering";
            if (reply) {
                const char* text = nullptr;
                g_variant_get(reply, "(&s)", &text);
                const auto state = core::json::parse(text ? text : "", nullptr, false);
                if (!state.is_discarded()) {
                    service = "running, version " + state.value("version", std::string("?")) +
                              (state.value("connected", false) ? ", connected to PipeWire"
                                                               : ", NOT connected to PipeWire") +
                              ", " + std::to_string(state.value("channels", core::json::array()).size()) +
                              " channels";
                }
                g_variant_unref(reply);
            }
        }
        g_object_unref(bus);
    }
    out << "Service: " << service << "\n\n";
}

// "## System": distribution, desktop, session type and kernel.
void system_section(std::ostream& out)
{
    out << "## System\n\n";
    // Inside a Flatpak, /etc/os-release is the runtime's; the host's is mounted here.
    std::string os = read_file(core::in_flatpak() ? "/run/host/os-release" : "/etc/os-release");
    if (os.empty())
        os = read_file("/usr/lib/os-release");
    const std::string pretty = key_value(os, "PRETTY_NAME");
    out << "Distribution: " << (pretty.empty() ? "(unknown)" : pretty) << "\n";
    out << "Desktop: " << env("XDG_CURRENT_DESKTOP") << "\n";
    // XDG_SESSION_TYPE doesn't always reach apps (some sandboxes, some launchers); the
    // display variables say the same thing.
    std::string session = env("XDG_SESSION_TYPE");
    if (session == "(not set)") {
        if (std::getenv("WAYLAND_DISPLAY"))
            session = "wayland (from WAYLAND_DISPLAY)";
        else if (std::getenv("DISPLAY"))
            session = "x11 (from DISPLAY)";
    }
    out << "Session: " << session << "\n";
    utsname uts{};
    out << "Kernel: " << (uname(&uts) == 0 ? std::string(uts.release) : "(unknown)") << "\n\n";
}

// "245 of 1024" for each of the user's processes with this name, from /proc.
std::string open_files(const std::string& process)
{
    std::string result;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/proc", error)) {
        const std::string pid = entry.path().filename();
        if (pid.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::string comm = read_file(entry.path() / "comm");
        if (!comm.empty() && comm.back() == '\n')
            comm.pop_back();
        struct stat info {};
        if (comm != process || stat(entry.path().c_str(), &info) != 0 || info.st_uid != getuid())
            continue;

        size_t open = 0;
        for ([[maybe_unused]] const auto& fd : fs::directory_iterator(entry.path() / "fd", error))
            ++open;
        std::string limit = "?";
        std::istringstream limits(read_file(entry.path() / "limits"));
        for (std::string line; std::getline(limits, line);) {
            if (line.starts_with("Max open files")) {
                std::istringstream words(line.substr(std::string("Max open files").size()));
                words >> limit;
            }
        }
        result += (result.empty() ? "" : ", ") + std::to_string(open) + " of " + limit;
    }
    return result.empty() ? "not running" : result;
}

// The versions, then WaveMix's part of the graph. Says so if PipeWire can't be reached.
void pipewire_and_graph_sections(std::ostream& out, const core::json& config)
{
    out << "## PipeWire\n\n";
    std::unique_ptr<pw::Connection> connection;
    try {
        connection = std::make_unique<pw::Connection>("wavemix-bug-report");
    } catch (const std::exception& e) {
        out << "PipeWire: NOT REACHABLE (" << e.what() << ")\n\n";
        return;
    }
    const pw::ServerInfo versions = pw::server_info(*connection);
    out << "PipeWire server: " << (versions.server.empty() ? "(unknown)" : versions.server) << "\n";
    out << "libpipewire in WaveMix: " << versions.library << "\n";
    out << "WirePlumber: " << (versions.wireplumber.empty() ? "not found" : versions.wireplumber) << "\n";
    if (core::in_flatpak()) {
        out << "Open files: not visible from inside the Flatpak. On the host, run:\n"
               "  for p in pipewire pipewire-pulse; do echo \"$p $(ls /proc/$(pgrep -xu $USER $p | head -1)/fd | "
               "wc -l)\"; done\n";
    } else {
        out << "Open files: pipewire " << open_files("pipewire") << "; pipewire-pulse "
            << open_files("pipewire-pulse") << "\n";
    }
    out << "\n";

    pw::Registry registry(*connection);
    connection->sync();
    connection->sync();
    const pw::Graph g = registry.snapshot();

    out << "## Audio graph (WaveMix's part)\n\n";
    out << "VOD mix: " << (core::vod_enabled(config) ? "on" : "off") << "\n";
    out << "Default output: " << (g.default_sink.empty() ? "(none)" : g.default_sink) << "\n";
    out << "Outputs:\n";

    for (const pw::Node* sink : g.hardware_sinks())
        out << "  " << sink->name() << "  \"" << sink->description() << "\"\n";

    out << "WaveMix sinks:\n";
    for (const auto& [id, node] : g.nodes) {
        if (node.is_sink() && node.name().starts_with(core::kPrefix))
            out << "  " << node.name() << "  " << percent(node) << "\n";
    }
    out << "Loopbacks:\n";
    for (const auto& [id, node] : g.nodes) {
        const auto tag = node.props.find("wavemix.link");
        if (tag == node.props.end())
            continue;
        const pw::Node* to = g.sink_of(node);
        out << "  " << tag->second << " -> " << (to ? to->name() : "(not linked)") << "  " << percent(node) << "\n";
    }
    out << "Apps playing through WaveMix:\n";
    for (const auto& [id, node] : g.nodes) {
        const pw::Node* to = node.is_stream() ? g.sink_of(node) : nullptr;
        if (!to || !to->name().starts_with(core::kPrefix) || core::is_internal(node.props))
            continue;
        out << "  " << core::app_name(node.props, node.id) << "  [" << core::app_key(node.props).value_or("no key")
            << "] -> " << to->name() << "\n";
    }

    // The mic: what it's set to, which plugins were found, the chain as configured and
    // as it's actually linked.
    const bool mic_on = core::mic_enabled(config);
    out << "Mic: " << (mic_on ? "on" : "off") << ", input setting " << core::mic_input_setting(config)
        << ", default input " << (g.default_source.empty() ? "(none)" : g.default_source) << "\n";
    out << "Inputs:\n";
    for (const pw::Node* source : g.hardware_sources())
        out << "  " << source->name() << "  \"" << source->description() << "\"\n";
    out << "Mic plugins:\n";
    const core::MicPlugins plugins = core::find_mic_plugins();
    for (core::MicFilter filter : core::kMicFilters) {
        const auto found = plugins.find(filter);
        out << "  " << core::to_string(filter) << ": "
            << (found != plugins.end() ? found->second
                                       : "missing (install " + core::mic_package(filter) + ")")
            << "\n";
    }
    out << "Mic chain (config):";
    const auto steps = core::mic_chain(config);
    if (steps.empty())
        out << " empty";
    for (size_t i = 0; i < steps.size(); ++i)
        out << (i ? ", " : " ") << i + 1 << "_" << core::to_string(steps[i].filter) << (steps[i].on ? "" : " (off)");
    out << "\n";
    // The chain runs in the service: its two sides, and where they're linked.
    out << "Mic chain (graph): ";
    const pw::Node* chain_in = g.node_named(std::string(core::kMicChainNode) + "_in");
    const pw::Node* chain_out = g.node_named(std::string(core::kMicChainNode));
    if (!chain_in || !chain_out) {
        out << "not running\n";
    } else {
        const pw::Node* reads = g.source_of(*chain_in);
        const pw::Node* plays = g.sink_of(*chain_out);
        out << (reads ? reads->name() : "(not linked)") << " -> " << core::kMicChainNode << " -> "
            << (plays ? plays->name() : "(not linked)") << "\n";
    }
    if (const pw::Node* mic = g.source_named(std::string(core::kMicSource)))
        out << "WaveMix Mic: " << percent(*mic) << "\n";

    // What the config expects that isn't there: the quickest way to spot a broken channel.
    // (Disabled channels have nothing to find.)
    std::vector<std::string> missing;
    if (mic_on && !g.hardware_sources().empty() && !g.source_named(std::string(core::kMicSource)))
        missing.push_back("source " + std::string(core::kMicSource) + " (WaveMix Mic)");
    const auto channels = core::running_config(config).value("channels", core::json::array());
    if (!channels.empty() && !g.sink_named(std::string(core::kPersonalSink)))
        missing.push_back("sink " + std::string(core::kPersonalSink) + " (WaveMix Personal Mix)");
    if (!channels.empty() && !g.hardware_sinks().empty() && !g.link_stream(std::string(core::kPersonalOutTag)))
        missing.push_back("loopback " + std::string(core::kPersonalOutTag));
    for (const auto& channel : channels) {
        const std::string channel_id = channel.value("id", std::string());
        if (!g.sink_named(core::base_sink(channel_id)))
            missing.push_back("sink " + core::base_sink(channel_id));
        for (core::Mix mix : core::kLinks) {
            if (mix == core::Mix::Vod && !core::wants_vod_link(config, channel))
                continue;
            if (!g.link_stream(core::link_tag(channel_id, mix)))
                missing.push_back("loopback " + core::link_tag(channel_id, mix));
        }
    }
    out << "Missing: " << (missing.empty() ? "nothing" : "") << "\n";
    for (const auto& thing : missing)
        out << "  " << thing << "\n";
    out << "\n";
}

// "### <title>": a file's contents, or "(missing)".
void file_section(std::ostream& out, const std::string& title, const fs::path& path)
{
    out << "### " << title << "\n\n";
    const std::string text = read_file(path);
    out << (text.empty() ? "(missing)\n" : text) << (text.empty() || text.back() == '\n' ? "" : "\n") << "\n";
}

// "## Log": the last kLogLines lines of the log, rotated file included.
void log_section(std::ostream& out)
{
    const fs::path file = core::log_file();
    out << "## Log (the last " << kLogLines << " lines)\n\n";
    // The rotated file first, in case the current one only just started.
    std::deque<std::string> lines;
    for (const fs::path& part : {fs::path(file.string() + ".1"), file}) {
        std::istringstream text(read_file(part));
        for (std::string line; std::getline(text, line);) {
            lines.push_back(line);
            if (lines.size() > kLogLines)
                lines.pop_front();
        }
    }
    if (lines.empty())
        out << "(no log yet)\n";
    for (const auto& line : lines)
        out << line << "\n";
}

// The home folder as "~", so the user's name doesn't end up in a public issue.
std::string without_home(std::string text)
{
    const char* home = std::getenv("HOME");
    if (!home || std::string_view(home).size() < 2)
        return text;
    const std::string needle = home;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        text.replace(at, needle.size(), "~");
    return text;
}

}  // namespace

// Writes every section in order and strips the home folder from the result.
std::string bug_report()
{
    const core::Paths paths = core::paths_from_environment();
    const core::json config = core::load_config(paths);

    char made[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(made, sizeof made, "%Y-%m-%d %H:%M", std::localtime(&now));

    std::ostringstream out;
    out << "# WaveMix bug report\n\n"
        << "Made " << made << ". It contains your WaveMix settings, the names of your audio\n"
        << "outputs and of the apps playing through WaveMix, and the end of WaveMix's log.\n"
        << "Nothing else about your system, and nothing has been sent anywhere. Read it,\n"
        << "then attach it to an issue at " << core::kWebsite << "/issues\n\n";
    wavemix_section(out);
    system_section(out);
    pipewire_and_graph_sections(out, config);
    out << "## Files\n\n";
    file_section(out, "config.json", paths.config_file);
    file_section(out, "state.json", paths.state_file);
    file_section(out, "60-wavemix.conf (the login file)", paths.login_conf);
    log_section(out);
    return without_home(out.str());
}

// "wavemix-bug-report-YYYYMMDD-HHMM.txt" for the current local time.
std::string suggested_file_name()
{
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", std::localtime(&now));
    return std::string("wavemix-bug-report-") + stamp + ".txt";
}

}  // namespace wavemix::report
