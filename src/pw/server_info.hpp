// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Which PipeWire and WirePlumber are running, for bug reports.
//
// The server's version comes with its core object, WirePlumber's with its client
// object (WirePlumber connects to PipeWire like any other program). Both only say it
// once bound, so this binds them, waits for their details and lets go again.

#pragma once

#include <string>

namespace wavemix::pw {

class Connection;

struct ServerInfo {
    std::string library;      // the libpipewire WaveMix is linked against
    std::string server;       // the PipeWire server, "" if it didn't say
    std::string wireplumber;  // "" if WirePlumber isn't connected (or another manager runs)
};

// Binds the core and WirePlumber's client, waits for their details and reports the
// library, server and WirePlumber versions.
ServerInfo server_info(Connection& connection);

}  // namespace wavemix::pw
