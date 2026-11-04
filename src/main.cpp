// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// wavemix                          open the mixer window (or raise it if it's open)
// wavemix --daemon                 run the background service (normally started for you)
// wavemix --status                 print channels, levels and apps
// wavemix --add LABEL [--icon I]   add a channel
// wavemix --remove ID              remove a channel
// wavemix --assign STREAM CHANNEL  route a playing stream (#number from --status) to a
//                                  channel; CHANNEL "" sends it back to the default output
// wavemix --vod CHANNEL on|off     put a channel in the VOD Mix or leave it out
// wavemix --restart                restart the background service
// wavemix --quit                   quit the background service
// wavemix --reset                  reset every preference and all the configuration
// wavemix --bug-report             print a bug report (redirect it to a file)
//
// Development options:
// wavemix --daemon --report-version V
//                                  report version V in GetState, so a window of another
//                                  version doesn't replace this daemon (for testing)
// wavemix --dump-graph             print what WaveMix sees in PipeWire, change nothing
// wavemix --version                print the version
//
// The window and the service write a log (~/.local/state/wavemix/wavemix.log, see
// core/log.hpp). --verbose, or WAVEMIX_VERBOSE=1 in the environment (for a service
// started by D-Bus), adds debug lines.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "build_config.hpp"
#include "cli.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "daemon/service.hpp"
#include "dump_graph.hpp"
#include "gui/application.hpp"
#include "report/bug_report.hpp"

namespace {

// True if --verbose is on the command line or WAVEMIX_VERBOSE is set (and not "0").
bool verbose_requested(const std::vector<std::string>& args)
{
    const char* env = std::getenv("WAVEMIX_VERBOSE");
    if (env && *env && std::string_view(env) != "0")
        return true;
    return std::find(args.begin(), args.end(), "--verbose") != args.end();
}

// The window and the service log to the same file; the CLI only prints.
void start_log(const std::vector<std::string>& args, const char* what)
{
    wavemix::core::log::init(wavemix::core::log_file(), verbose_requested(args));
    wavemix::core::log::info("main", std::string("WaveMix " WAVEMIX_VERSION " ") + what + " starting");
}

// Prints the full usage, every mode included, and the version.
void print_usage()
{
    std::cout << "Usage: wavemix                          open the mixer window\n"
                 "       wavemix --status                 show channels, levels and apps\n"
                 "       wavemix --add LABEL [--icon I]   add a channel\n"
                 "       wavemix --remove ID              remove a channel\n"
                 "       wavemix --assign STREAM CHANNEL  route a playing stream to a channel\n"
                 "       wavemix --vod CHANNEL on|off     put a channel in the VOD Mix or leave it out\n"
                 "       wavemix --restart                restart the background service\n"
                 "       wavemix --quit                   quit the background service\n"
                 "       wavemix --reset                  reset every preference and all the configuration\n"
                 "       wavemix --bug-report > FILE      write a bug report to attach to a GitHub issue\n"
                 "       wavemix --daemon                 run the background service\n"
                 "       wavemix --verbose                with the window or --daemon: log debug lines too\n"
                 "       wavemix --dump-graph | --version | --help\n"
                 "\n"
                 "WaveMix " WAVEMIX_VERSION "\n";
}

// --daemon: parses the service's options (keeping argv for Restart), starts the log
// and runs the service until it quits.
int run_daemon(const std::vector<std::string>& args)
{
    wavemix::daemon::Options options;
    options.reported_version = WAVEMIX_VERSION;
    options.argv.push_back(args[0]);  // so Restart starts us again the same way
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--login") {
            options.login = true;
            continue;  // not passed on to a Restart: that isn't a login
        }
        options.argv.push_back(args[i]);
        if (args[i] == "--daemon" || args[i] == "--verbose")
            continue;
        if (args[i] == "--report-version" && i + 1 < args.size()) {
            options.reported_version = args[++i];
            options.argv.push_back(args[i]);
            continue;
        }
        std::cerr << "wavemix: unknown option for --daemon: " << args[i] << "\n";
        return 2;
    }
    start_log(args, "service");
    wavemix::daemon::Service service(std::move(options));
    return service.run();
}

}  // namespace

// Picks the mode from argv[1]: the window, the service, a CLI command, or one of the
// development and help options.
int main(int argc, char* argv[])
{
    const std::vector<std::string> args(argv, argv + argc);
    const std::string_view command = argc > 1 ? argv[1] : "";

    if (command.empty() || (command == "--verbose" && argc == 2)) {
        start_log(args, "window");
        return wavemix::gui::run_window();
    }
    if (command == "--daemon")
        return run_daemon(args);
    if (wavemix::is_cli_command(args))
        return wavemix::run_cli(args);
    if (command == "--version") {
        std::cout << "wavemix " WAVEMIX_VERSION "\n";
        return 0;
    }
    if (command == "--dump-graph")
        return wavemix::dump_graph();
    if (command == "--bug-report") {
        std::cout << wavemix::report::bug_report();
        return 0;
    }
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }
    std::cerr << "wavemix: unknown option '" << command << "'\n";
    print_usage();
    return 2;
}
