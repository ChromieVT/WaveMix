// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The D-Bus client for the WaveMix daemon, shared by the mixer window and the CLI.
//
// Everything a client does goes through the daemon (docs/contract/dbus.md): this class
// starts it if needed, calls its methods, and reports StateChanged. Errors come back
// as ClientError with the daemon's message, ready to show to the user.
//
// Calls are synchronous (they return within milliseconds) except the slider ones,
// which are asynchronous so dragging a slider never waits on the bus.

#pragma once

#include <functional>
#include <stdexcept>
#include <string>

#include "core/json_file.hpp"

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GVariant GVariant;

namespace wavemix::client {

class ClientError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Client {
public:
    // Connects to the session bus. With `autostart`, also makes sure the daemon is
    // running (see ensure_daemon). Throws ClientError if there's no session bus.
    explicit Client(bool autostart = true);
    // Stops listening for changes and drops the bus connection.
    ~Client();

    // Not copyable (copy constructor and assignment deleted): it owns the bus connection and its subscriptions.
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // True if a daemon owns the bus name right now.
    bool running() const;

    // Starts the daemon if it isn't running: D-Bus activation first (the installed
    // service file), and if that fails, by running `wavemix --daemon` ourselves. Waits
    // up to `timeout_seconds` for it to appear.
    void ensure_daemon(double timeout_seconds = 5.0);

    // ---- The daemon's methods --------------------------------------------------------
    // GetState: the daemon's whole state as JSON.
    core::json state();
    // AddChannel: creates a channel and returns its new ID.
    std::string add_channel(const std::string& label, const std::string& icon = "");
    // RemoveChannel: deletes a channel.
    void remove_channel(const std::string& id);
    // RenameChannel: changes a channel's label and, if given, its icon.
    void rename_channel(const std::string& id, const std::string& label, const std::string& icon = "");
    // MoveChannel: moves a channel to `position` in the list.
    void move_channel(const std::string& id, int position);
    // AssignApp: routes the stream with this serial into a channel.
    void assign_app(uint32_t stream, const std::string& channel);
    // SetInVod: includes a channel in the VOD Mix or leaves it out.
    void set_in_vod(const std::string& channel, bool in_vod);
    // SetSetting: changes one setting (the value travels JSON-encoded).
    void set_setting(const std::string& key, const core::json& value);
    // SetMicInput: picks the mic's input device ("default" or a source name).
    void set_mic_input(const std::string& device);
    // SetMicEnabled: turns WaveMix Mic on or off.
    void set_mic_enabled(bool enabled);
    // AddMicFilter: inserts a filter in the mic chain (-1: at the end) and returns where it went.
    int add_mic_filter(const std::string& filter, int position = -1);
    // RemoveMicFilter: removes the filter at `position`.
    void remove_mic_filter(int position);
    // MoveMicFilter: moves a filter from one position to another.
    void move_mic_filter(int from, int to);
    // SetMicFilterOn: switches a filter on or off.
    void set_mic_filter_on(int position, bool on);
    // Reset: every preference and all the configuration back to the defaults.
    void reset();
    // Restart: asks the daemon to restart itself.
    void restart();
    // Quit: asks the daemon to exit.
    void quit();

    // Slider calls: sent without waiting. If the daemon refuses, `on_error` gets its
    // message (on the main loop).
    using ErrorHandler = std::function<void(const std::string&)>;
    // SetVolume: a channel's Personal or Stream level.
    void set_volume(const std::string& channel, const std::string& mix, double volume, ErrorHandler on_error = {});
    // SetMute: mutes or unmutes a channel's Personal or Stream mix.
    void set_mute(const std::string& channel, const std::string& mix, bool muted, ErrorHandler on_error = {});
    // SetMicVolume: one of the mic's sliders.
    void set_mic_volume(const std::string& slider, double volume, ErrorHandler on_error = {});
    // SetMicMute: mutes or unmutes one of the mic's sliders.
    void set_mic_mute(const std::string& slider, bool muted, ErrorHandler on_error = {});
    // SetMicFilterValue: one setting of a filter in the mic chain.
    void set_mic_filter_value(int position, const std::string& key, double value, ErrorHandler on_error = {});

    // The mic's meters (GetMeters), sent without waiting: `on_reply` gets them on the main
    // loop, or nothing if the daemon didn't answer.
    void get_meters(std::function<void(const core::json&)> on_reply);

    // Calls `callback` (on the main loop) whenever the daemon reports a change, and
    // whenever it appears or disappears (a restart, say). One callback per client.
    void on_changed(std::function<void()> callback);

private:
    struct Callbacks;  // the C callbacks; defined in client.cpp

    // Calls a method and returns its reply (or nullptr for none). Throws ClientError.
    GVariant* call(const char* method, GVariant* parameters, const char* reply_type = nullptr,
                   int timeout_ms = 5000);
    // Calls a method without waiting; failures go to `on_error` on the main loop.
    void call_async(const char* method, GVariant* parameters, ErrorHandler on_error);

    GDBusConnection* bus_ = nullptr;
    std::function<void()> on_changed_;
    unsigned signal_id_ = 0;
    unsigned watch_id_ = 0;
};

// The message from a D-Bus error, without GDBus's "GDBus.Error:<name>: " prefix.
std::string clean_error(const char* message);

}  // namespace wavemix::client
