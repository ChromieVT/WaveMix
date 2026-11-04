// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The WaveMix daemon: the Mixer, published on the session bus.
//
//   Bus name  io.github.ChromieVT.WaveMix.Daemon
//   Object    /io/github/ChromieVT/WaveMix/Daemon
//   Interface io.github.ChromieVT.WaveMix.Daemon1
//
// The interface is described in docs/contract/dbus.md. This class only translates:
// D-Bus calls in, Mixer calls, replies and errors out, and a debounced StateChanged
// signal whenever something clients show may have changed.
//
// It's written against the GIO C API directly rather than giomm: the tray protocol's
// nested types (a(iiay), (ia{sv}av), ...) are one format string in GIO and a page of
// templates in giomm.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mixer/mixer.hpp"

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusMethodInvocation GDBusMethodInvocation;
typedef struct _GMainLoop GMainLoop;
typedef struct _GVariant GVariant;

namespace wavemix::daemon {

class Tray;

struct Options {
    // What GetState reports as "version". Normally this build's version. A window
    // replaces a daemon whose version differs from its own (that's how an update takes
    // over), so for testing with a window of another version, the --report-version
    // switch reports that version instead.
    std::string reported_version;

    // How this process was started, so Restart can start it again the same way.
    std::vector<std::string> argv;

    // Started by the login entry (--login): open the window too, unless the user
    // prefers starting minimised.
    bool login = false;
};

class Service {
public:
    // Loads the mixer (config and state) and creates the main loop; doesn't touch the bus yet.
    explicit Service(Options options);
    // Removes the tray, the pending signal, the D-Bus object and the bus name.
    ~Service();

    // Not copyable (copy constructor and assignment deleted): it owns the bus name and main loop.
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // Owns the bus name and runs until Quit (or until another daemon turns out to own
    // the name already). Returns the process exit code.
    int run();

private:
    struct Callbacks;  // the C callbacks GIO calls; defined in service.cpp

    // GIO calls these through Callbacks.
    // The session bus is connected: publishes the D-Bus object.
    void on_bus_acquired(GDBusConnection* bus);
    // The bus name is ours: creates the tray, applies autostart and starts pumping.
    void on_name_acquired();
    // The bus name couldn't be had (another daemon, or no bus): quits.
    void on_name_lost();
    // Handles one D-Bus method call and sends its reply or error.
    void on_method(const std::string& method, GVariant* parameters, GDBusMethodInvocation* invocation);
    // Called every 50 ms: connects to PipeWire if needed, and lets the mixer handle events.
    bool pump();

    // Emits StateChanged ~60 ms after the last call, so a burst of changes (a slider
    // being dragged, say) produces one signal instead of dozens.
    void changed();
    // Sends the StateChanged signal now.
    void emit_changed();

    // Saves levels, closes the window and leaves the main loop.
    void quit();
    // Saves levels and replaces this process with a fresh copy of itself.
    void restart();
    // Asks the mixer window, if one is open, to quit.
    void close_window();

    Options options_;
    mixer::Mixer mixer_;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* bus_ = nullptr;
    unsigned owner_id_ = 0;
    unsigned object_id_ = 0;
    unsigned signal_timer_ = 0;
    std::unique_ptr<Tray> tray_;
    int exit_code_ = 0;
    int64_t next_connect_us_ = 0;  // when to retry PipeWire (monotonic microseconds)
};

}  // namespace wavemix::daemon
