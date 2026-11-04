// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "cli.hpp"

#include <charconv>
#include <chrono>
#include <thread>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "client/client.hpp"

namespace wavemix {

using core::json;

namespace {

// The JSON value if it's a string, else "".
std::string str(const json& value)
{
    return value.is_string() ? value.get<std::string>() : std::string();
}

// " 80%", " 80% muted" or "missing", for one slider.
std::string level(const json& lvl)
{
    if (!lvl.is_object() || !lvl.value("present", false))
        return "missing";
    std::ostringstream out;
    out << std::setw(3) << std::lround(lvl.value("volume", 0.0) * 100) << "%";
    if (lvl.value("muted", false))
        out << " muted";
    return out.str();
}

// Pads to `width` characters, like printf's %-10s.
std::string padded(const std::string& text, size_t width)
{
    return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

// The service's state, once it's connected to PipeWire. A service that was only just
// started (by us, or at login) takes a moment to get there, so every command gives it
// a few seconds; otherwise `wavemix --add Game` right after login could fail.
json connected_state(client::Client& client)
{
    json st = client.state();
    for (int tries = 0; tries < 25 && !st.value("connected", false); ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        st = client.state();
    }
    return st;
}

// --status: prints the personal output, each channel with its levels and apps, the
// mixes, and the apps that aren't in a channel.
int status(client::Client& client)
{
    const json st = connected_state(client);
    if (!st.value("connected", false)) {
        std::cout << "daemon is running but not connected to PipeWire\n";
        return 1;
    }

    const json& po = st["personal_output"];
    std::string output = str(po["description"]);
    if (output.empty())
        output = str(po["resolved"]);
    if (output.empty())
        output = "none";
    std::cout << "Personal output: " << output << (str(po["setting"]) == "default" ? " (follows default)" : "")
              << "\n";

    const json& channels = st["channels"];
    const bool vod = st.value("settings", json::object()).value("vod_enabled", false);
    if (channels.empty())
        std::cout << "No channels yet - add one with: wavemix --add Game\n";
    for (const auto& ch : channels) {
        std::cout << "[" << str(ch["id"]) << "] " << padded(str(ch["label"]), 10) << " personal "
                  << padded(level(ch["personal"]), 10) << " stream " << level(ch["stream"])
                  << (vod && !ch.value("in_vod", true) ? "  (not in the VOD)" : "") << "\n";
        for (const auto& app : ch["apps"])
            std::cout << "      - " << str(app["name"]) << " (#" << app["index"] << ")\n";
    }
    if (!channels.empty())
        std::cout << "Stream mix: " << level(st["stream_mix"])
                  << (vod ? "   VOD mix: " + level(st["vod_mix"]) : std::string()) << "\n";

    if (!st["unrouted_apps"].empty()) {
        std::cout << "Not in a channel:\n";
        for (const auto& app : st["unrouted_apps"]) {
            std::string sink = str(app["sink_description"]);
            if (sink.empty())
                sink = str(app["sink"]);
            std::cout << "      - " << str(app["name"]) << " (#" << app["index"] << ") -> " << sink << "\n";
        }
    }
    return 0;
}

// Restart: the service comes back by itself. If it wasn't running, it's just started.
int restart()
{
    client::Client client(false);
    if (!client.running()) {
        client.ensure_daemon();
        std::cout << "WaveMix wasn't running; started it\n";
        return 0;
    }
    client.restart();
    std::cout << "Restarting WaveMix\n";
    return 0;
}

// Quit: never starts the service just to stop it.
int quit()
{
    client::Client client(false);
    if (!client.running()) {
        std::cout << "WaveMix isn't running\n";
        return 0;
    }
    client.quit();
    std::cout << "WaveMix has quit: what goes through it is quiet until it runs again\n";
    return 0;
}

// Reset: asks first, like the button in Preferences. `echo y | wavemix --reset` answers
// for a script.
int reset()
{
    std::cout << "All your preferences and configuration will be reset: your channels and the apps in\n"
                 "them, the mic's filters, your levels, the output device and the theme go back to how\n"
                 "a new install has them. This can't be undone.\n"
                 "Reset WaveMix? [y/N] "
              << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y" && answer != "yes") {
        std::cout << "Nothing was changed\n";
        return 1;
    }
    client::Client client;
    connected_state(client);
    client.reset();
    std::cout << "WaveMix has been reset\n";
    return 0;
}

// Prints the CLI's usage to stderr and returns the bad-usage exit code (2).
int usage()
{
    std::cerr << "usage: wavemix --status\n"
                 "       wavemix --add LABEL [--icon ICON]\n"
                 "       wavemix --remove ID\n"
                 "       wavemix --assign STREAM CHANNEL\n"
                 "       wavemix --vod CHANNEL on|off\n"
                 "       wavemix --restart | --quit | --reset\n";
    return 2;
}

}  // namespace

// Checks args[1] against the CLI modes.
bool is_cli_command(const std::vector<std::string>& args)
{
    if (args.size() < 2)
        return false;
    const auto& mode = args[1];
    return mode == "--status" || mode == "--add" || mode == "--remove" || mode == "--assign" ||
           mode == "--vod" || mode == "--restart" || mode == "--quit" || mode == "--reset";
}

// Parses the mode's arguments, runs it through the daemon, and maps errors to exit
// codes (1 refused, 2 bad usage).
int run_cli(const std::vector<std::string>& args)
{
    const std::string mode = args.size() > 1 ? args[1] : "";
    try {
        if (mode == "--status" && args.size() == 2) {
            client::Client client;
            return status(client);
        }
        if (mode == "--add" && (args.size() == 3 || (args.size() == 5 && args[3] == "--icon"))) {
            client::Client client;
            connected_state(client);
            std::cout << client.add_channel(args[2], args.size() == 5 ? args[4] : "") << "\n";
            return 0;
        }
        if (mode == "--remove" && args.size() == 3) {
            client::Client client;
            connected_state(client);
            client.remove_channel(args[2]);
            return 0;
        }
        if (mode == "--assign" && args.size() == 4) {
            uint32_t stream = 0;
            const auto& text = args[2];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), stream);
            if (error != std::errc() || end != text.data() + text.size()) {
                std::cerr << "wavemix: STREAM must be a stream number from --status, not '" << text << "'\n";
                return 2;
            }
            client::Client client;
            connected_state(client);
            client.assign_app(stream, args[3]);
            return 0;
        }
        if (args.size() == 2 && mode == "--restart")
            return restart();
        if (args.size() == 2 && mode == "--quit")
            return quit();
        if (args.size() == 2 && mode == "--reset")
            return reset();
        if (mode == "--vod" && args.size() == 4 && (args[3] == "on" || args[3] == "off")) {
            client::Client client;
            connected_state(client);
            client.set_in_vod(args[2], args[3] == "on");
            return 0;
        }
    } catch (const client::ClientError& e) {
        std::cerr << "wavemix: " << e.what() << "\n";
        return 1;
    }
    return usage();
}

}  // namespace wavemix
