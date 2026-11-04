// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#pragma once

#include <string>
#include <vector>

namespace wavemix {

// The command-line modes, all of which talk to the daemon (starting it if needed,
// except for --quit):
//
//   wavemix --status                 channels, levels and apps
//   wavemix --add LABEL [--icon I]   add a channel; prints its ID
//   wavemix --remove ID              remove a channel
//   wavemix --assign STREAM CHANNEL  route a playing stream (its #number from --status)
//                                    to a channel; CHANNEL "" sends it back to the default
//   wavemix --vod CHANNEL on|off     put a channel in the VOD Mix or leave it out
//   wavemix --restart                restart the service (or start it)
//   wavemix --quit                   quit the service (it isn't started for this)
//   wavemix --reset                  every preference and all the configuration back to
//                                    the defaults, after asking
//
// `args` is the whole command line, argv[0] included. Returns the exit code: 0 on
// success, 1 if the daemon refused (the reason goes to stderr), 2 for bad usage.
int run_cli(const std::vector<std::string>& args);

// True if `args` asks for one of the modes above.
bool is_cli_command(const std::vector<std::string>& args);

}  // namespace wavemix
