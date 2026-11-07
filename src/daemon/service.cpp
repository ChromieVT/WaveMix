// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "daemon/service.hpp"

#include <gio/gio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"
#include "daemon/autostart.hpp"
#include "daemon/tray.hpp"

namespace wavemix::daemon {

using core::json;

namespace {

// The interface described in docs/contract/dbus.md. Clients depend on these exact
// method names, argument names and types, so treat it as frozen: new features only
// add methods.
constexpr const char* kIntrospection = R"xml(
<node>
  <interface name="io.github.ChromieVT.WaveMix.Daemon1">
    <method name="GetState"><arg type="s" name="json" direction="out"/></method>
    <method name="SetVolume">
      <arg type="s" name="channel" direction="in"/>
      <arg type="s" name="mix" direction="in"/>
      <arg type="d" name="volume" direction="in"/>
    </method>
    <method name="SetMute">
      <arg type="s" name="channel" direction="in"/>
      <arg type="s" name="mix" direction="in"/>
      <arg type="b" name="muted" direction="in"/>
    </method>
    <method name="AddChannel">
      <arg type="s" name="label" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
      <arg type="s" name="id" direction="out"/>
    </method>
    <method name="RemoveChannel"><arg type="s" name="id" direction="in"/></method>
    <method name="RenameChannel">
      <arg type="s" name="id" direction="in"/>
      <arg type="s" name="label" direction="in"/>
      <arg type="s" name="icon" direction="in"/>
    </method>
    <method name="MoveChannel">
      <arg type="s" name="id" direction="in"/>
      <arg type="i" name="position" direction="in"/>
    </method>
    <method name="AssignApp">
      <arg type="u" name="stream" direction="in"/>
      <arg type="s" name="channel" direction="in"/>
    </method>
    <method name="ForgetAssignment"><arg type="s" name="app" direction="in"/></method>
    <method name="SetInVod">
      <arg type="s" name="channel" direction="in"/>
      <arg type="b" name="in_vod" direction="in"/>
    </method>
    <method name="SetChannelEnabled">
      <arg type="s" name="channel" direction="in"/>
      <arg type="b" name="enabled" direction="in"/>
    </method>
    <method name="SwitchProfile"><arg type="s" name="name" direction="in"/></method>
    <method name="SaveProfileAs"><arg type="s" name="name" direction="in"/></method>
    <method name="RenameProfile">
      <arg type="s" name="name" direction="in"/>
      <arg type="s" name="new_name" direction="in"/>
    </method>
    <method name="DeleteProfile"><arg type="s" name="name" direction="in"/></method>
    <method name="ExportProfile">
      <arg type="s" name="name" direction="in"/>
      <arg type="s" name="json" direction="out"/>
    </method>
    <method name="ImportProfile">
      <arg type="s" name="json" direction="in"/>
      <arg type="s" name="name" direction="out"/>
    </method>
    <method name="SetSetting">
      <arg type="s" name="key" direction="in"/>
      <arg type="s" name="json_value" direction="in"/>
    </method>
    <method name="GetMeters"><arg type="s" name="json" direction="out"/></method>
    <method name="SetMicInput"><arg type="s" name="device" direction="in"/></method>
    <method name="SetMicEnabled"><arg type="b" name="enabled" direction="in"/></method>
    <method name="SetMicVolume">
      <arg type="s" name="slider" direction="in"/>
      <arg type="d" name="volume" direction="in"/>
    </method>
    <method name="SetMicMute">
      <arg type="s" name="slider" direction="in"/>
      <arg type="b" name="muted" direction="in"/>
    </method>
    <method name="AddMicFilter">
      <arg type="s" name="filter" direction="in"/>
      <arg type="i" name="position" direction="in"/>
      <arg type="i" name="added_at" direction="out"/>
    </method>
    <method name="RemoveMicFilter"><arg type="i" name="position" direction="in"/></method>
    <method name="MoveMicFilter">
      <arg type="i" name="from" direction="in"/>
      <arg type="i" name="to" direction="in"/>
    </method>
    <method name="SetMicFilterOn">
      <arg type="i" name="position" direction="in"/>
      <arg type="b" name="on" direction="in"/>
    </method>
    <method name="SetMicFilterValue">
      <arg type="i" name="position" direction="in"/>
      <arg type="s" name="key" direction="in"/>
      <arg type="d" name="value" direction="in"/>
    </method>
    <method name="Reset"/>
    <method name="Restart"/>
    <method name="Quit"/>
    <signal name="StateChanged"/>
  </interface>
</node>
)xml";

constexpr unsigned kPumpMs = 50;           // how often the mixer gets to handle events
constexpr unsigned kSignalDelayMs = 60;    // StateChanged debounce
constexpr unsigned kActionDelayMs = 150;   // Restart/Quit: reply first, act after this
constexpr int64_t kReconnectUs = 2'000'000;  // retry PipeWire every 2 s

// The service's lines in the log (core/log.hpp).
// Writes an info line for the service.
void log(const std::string& message)
{
    core::log::info("service", message);
}

// Writes an error line for the service.
void log_error(const std::string& message)
{
    core::log::error("service", message);
}

// The D-Bus error name for a MixerError kind (…Error.Failed or …Error.Disconnected).
std::string error_name(mixer::MixerError::Kind kind)
{
    return std::string(core::kInterface) +
           (kind == mixer::MixerError::Kind::Disconnected ? ".Error.Disconnected" : ".Error.Failed");
}

}  // namespace

// GIO calls plain C functions; `data` is the Service. They all run on the main loop.
struct Service::Callbacks {
    // The session bus is connected.
    static void bus_acquired(GDBusConnection* bus, const char*, gpointer data)
    {
        static_cast<Service*>(data)->on_bus_acquired(bus);
    }
    // The bus name is ours.
    static void name_acquired(GDBusConnection*, const char*, gpointer data)
    {
        static_cast<Service*>(data)->on_name_acquired();
    }
    // The bus name couldn't be had.
    static void name_lost(GDBusConnection*, const char*, gpointer data)
    {
        static_cast<Service*>(data)->on_name_lost();
    }
    // A D-Bus method call on our object.
    static void method_call(GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data)
    {
        static_cast<Service*>(data)->on_method(method, parameters, invocation);
    }
    // The pump timer fired; keeps running while pump() returns true.
    static gboolean pump(gpointer data)
    {
        return static_cast<Service*>(data)->pump() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }
    // The StateChanged debounce timer fired.
    static gboolean emit_changed(gpointer data)
    {
        auto* self = static_cast<Service*>(data);
        self->signal_timer_ = 0;
        self->emit_changed();
        return G_SOURCE_REMOVE;
    }
    // The delayed Quit fired.
    static gboolean quit(gpointer data)
    {
        static_cast<Service*>(data)->quit();
        return G_SOURCE_REMOVE;
    }
    // The delayed Restart fired.
    static gboolean restart(gpointer data)
    {
        static_cast<Service*>(data)->restart();
        return G_SOURCE_REMOVE;
    }

    static inline const GDBusInterfaceVTable kVTable = {&method_call, nullptr, nullptr, {}};
};

// Loads the mixer from the user's files and creates the main loop.
Service::Service(Options options)
    : options_(std::move(options)),
      mixer_(core::paths_from_environment(), options_.reported_version),
      loop_(g_main_loop_new(nullptr, FALSE))
{
}

// Tears down in reverse order: tray, timer, D-Bus object, bus name, loop.
Service::~Service()
{
    tray_.reset();
    if (signal_timer_)
        g_source_remove(signal_timer_);
    if (bus_ && object_id_)
        g_dbus_connection_unregister_object(bus_, object_id_);
    if (owner_id_)
        g_bus_unown_name(owner_id_);
    g_main_loop_unref(loop_);
}

// Asks for the bus name, runs the main loop until it's quit, then saves levels.
int Service::run()
{
    // DO_NOT_QUEUE: if another daemon already has the name, we're told straight away
    // (name_lost) instead of waiting in line to take over.
    owner_id_ = g_bus_own_name(G_BUS_TYPE_SESSION, std::string(core::kBusName).c_str(),
                               G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, &Callbacks::bus_acquired,
                               &Callbacks::name_acquired, &Callbacks::name_lost, this, nullptr);
    g_main_loop_run(loop_);

    // Leaving the loop any other way than Quit (Ctrl+C, say): still save levels.
    mixer_.shutdown();
    return exit_code_;
}

// =====================================================================================
// Bus name and object
// =====================================================================================

// Publishes the D-Bus object from the introspection XML, quitting if that fails.
void Service::on_bus_acquired(GDBusConnection* bus)
{
    bus_ = bus;
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kIntrospection, &error);
    if (!info) {
        log_error(std::string("bad introspection XML: ") + error->message);  // a bug, not a user error
        g_error_free(error);
        g_main_loop_quit(loop_);
        return;
    }
    object_id_ = g_dbus_connection_register_object(bus, std::string(core::kObjectPath).c_str(),
                                                   info->interfaces[0], &Callbacks::kVTable, this,
                                                   nullptr, &error);
    g_dbus_node_info_unref(info);
    if (!object_id_) {
        log_error(std::string("could not publish the D-Bus object: ") + error->message);
        g_error_free(error);
        g_main_loop_quit(loop_);
    }
}

// Starts the tray (once), syncs autostart, opens the window after a non-minimised login,
// and starts the pump timer.
void Service::on_name_acquired()
{
    log("running as " + std::string(core::kBusName) + " (reporting version " +
        options_.reported_version + ")");
    if (!tray_) {
        tray_ = std::make_unique<Tray>(bus_, [this] { restart(); }, [this] { quit(); });
        const core::json settings = mixer_.state().value("settings", core::json::object());
        // The portal keeps the login entry's command line current; elsewhere the entry
        // is only written when the preference changes.
        if (core::in_flatpak())
            set_autostart(bus_, settings.value("autostart", true));
        if (options_.login && !settings.value("start_minimized", true))
            open_mixer();
    }
    // Start handling PipeWire. pump() connects (and keeps retrying) by itself.
    g_timeout_add(kPumpMs, &Callbacks::pump, this);
}

// Logs why the name couldn't be had and quits.
void Service::on_name_lost()
{
    if (!bus_) {
        log_error("can't connect to the session bus");
        exit_code_ = 1;
    } else {
        log("another WaveMix daemon is already running - exiting");
    }
    g_main_loop_quit(loop_);
}

// =====================================================================================
// The main-loop pump
// =====================================================================================

// Retries the PipeWire connection every 2 s while disconnected; otherwise lets the
// mixer poll, emitting StateChanged when something may have changed.
bool Service::pump()
{
    if (!mixer_.connected()) {
        const int64_t now = g_get_monotonic_time();
        if (now >= next_connect_us_) {
            if (mixer_.connect())
                changed();  // clients go from "Waiting for PipeWire..." to the mixer
            else
                next_connect_us_ = now + kReconnectUs;
        }
        return true;
    }
    if (mixer_.poll())
        changed();
    if (!mixer_.connected()) {
        // poll() noticed PipeWire went away (and reported a change, so clients show
        // "Waiting for PipeWire..."). Try again in a moment; it's usually restarting.
        next_connect_us_ = g_get_monotonic_time() + kReconnectUs;
    }
    return true;
}

// (Re)starts the 60 ms StateChanged debounce timer.
void Service::changed()
{
    if (signal_timer_)
        g_source_remove(signal_timer_);
    signal_timer_ = g_timeout_add(kSignalDelayMs, &Callbacks::emit_changed, this);
}

// Emits StateChanged on our object.
void Service::emit_changed()
{
    if (bus_) {
        g_dbus_connection_emit_signal(bus_, nullptr, std::string(core::kObjectPath).c_str(),
                                      std::string(core::kInterface).c_str(), "StateChanged", nullptr, nullptr);
    }
}

// =====================================================================================
// Methods
// =====================================================================================

// Unpacks the call's arguments, calls the matching Mixer method, and replies with its
// result, or with a D-Bus error when it throws.
void Service::on_method(const std::string& method, GVariant* params, GDBusMethodInvocation* invocation)
{
    // Every reply path goes through here: exceptions must never cross into GIO's C code.
    try {
        if (method == "GetState") {
            const std::string text = mixer_.state().dump();
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", text.c_str()));
            return;
        }
        if (method == "GetMeters") {
            // Asked many times a second while the window shows them: no logging, no
            // StateChanged.
            const std::string text = mixer_.meters().dump();
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", text.c_str()));
            return;
        }
        if (core::log::verbose()) {
            // Every change a client asks for, with its arguments (GetState is too frequent).
            gchar* text = params ? g_variant_print(params, FALSE) : g_strdup("()");
            core::log::debug("service", "call " + method + text);
            g_free(text);
        }
        if (method == "Restart" || method == "Quit") {
            // Reply first, then act: the caller shouldn't see its connection drop.
            g_dbus_method_invocation_return_value(invocation, nullptr);
            g_timeout_add(kActionDelayMs, method == "Restart" ? &Callbacks::restart : &Callbacks::quit, this);
            return;
        }

        GVariant* result = nullptr;
        const char* a = nullptr;
        const char* b = nullptr;
        const char* c = nullptr;
        if (method == "SetVolume") {
            double volume = 0;
            g_variant_get(params, "(&s&sd)", &a, &b, &volume);
            mixer_.set_volume(a, b, volume);
        } else if (method == "SetMute") {
            gboolean muted = FALSE;
            g_variant_get(params, "(&s&sb)", &a, &b, &muted);
            mixer_.set_mute(a, b, muted);
        } else if (method == "AddChannel") {
            g_variant_get(params, "(&s&s)", &a, &b);
            result = g_variant_new("(s)", mixer_.add_channel(a, b).c_str());
        } else if (method == "RemoveChannel") {
            g_variant_get(params, "(&s)", &a);
            mixer_.remove_channel(a);
        } else if (method == "RenameChannel") {
            g_variant_get(params, "(&s&s&s)", &a, &b, &c);
            mixer_.rename_channel(a, b, c);
        } else if (method == "MoveChannel") {
            int position = 0;
            g_variant_get(params, "(&si)", &a, &position);
            mixer_.move_channel(a, position);
        } else if (method == "AssignApp") {
            guint32 stream = 0;
            g_variant_get(params, "(u&s)", &stream, &a);
            mixer_.assign_app(stream, a);
        } else if (method == "ForgetAssignment") {
            g_variant_get(params, "(&s)", &a);
            mixer_.forget_assignment(a);
        } else if (method == "SetInVod") {
            gboolean in_vod = TRUE;
            g_variant_get(params, "(&sb)", &a, &in_vod);
            mixer_.set_in_vod(a, in_vod);
        } else if (method == "SetChannelEnabled") {
            gboolean enabled = TRUE;
            g_variant_get(params, "(&sb)", &a, &enabled);
            mixer_.set_channel_enabled(a, enabled);
        } else if (method == "SwitchProfile") {
            g_variant_get(params, "(&s)", &a);
            mixer_.switch_profile(a);
        } else if (method == "SaveProfileAs") {
            g_variant_get(params, "(&s)", &a);
            mixer_.save_profile_as(a);
        } else if (method == "RenameProfile") {
            g_variant_get(params, "(&s&s)", &a, &b);
            mixer_.rename_profile(a, b);
        } else if (method == "DeleteProfile") {
            g_variant_get(params, "(&s)", &a);
            mixer_.delete_profile(a);
        } else if (method == "ExportProfile") {
            g_variant_get(params, "(&s)", &a);
            result = g_variant_new("(s)", mixer_.export_profile(a).c_str());
        } else if (method == "ImportProfile") {
            g_variant_get(params, "(&s)", &a);
            result = g_variant_new("(s)", mixer_.import_profile(a).c_str());
        } else if (method == "SetMicInput") {
            g_variant_get(params, "(&s)", &a);
            mixer_.set_mic_input(a);
        } else if (method == "SetMicEnabled") {
            gboolean enabled = TRUE;
            g_variant_get(params, "(b)", &enabled);
            mixer_.set_mic_enabled(enabled);
        } else if (method == "SetMicVolume") {
            double volume = 0;
            g_variant_get(params, "(&sd)", &a, &volume);
            mixer_.set_mic_volume(a, volume);
        } else if (method == "SetMicMute") {
            gboolean muted = FALSE;
            g_variant_get(params, "(&sb)", &a, &muted);
            mixer_.set_mic_mute(a, muted);
        } else if (method == "AddMicFilter") {
            int position = -1;
            g_variant_get(params, "(&si)", &a, &position);
            result = g_variant_new("(i)", mixer_.add_mic_filter(a, position));
        } else if (method == "RemoveMicFilter") {
            int position = 0;
            g_variant_get(params, "(i)", &position);
            mixer_.remove_mic_filter(position);
        } else if (method == "MoveMicFilter") {
            int from = 0;
            int to = 0;
            g_variant_get(params, "(ii)", &from, &to);
            mixer_.move_mic_filter(from, to);
        } else if (method == "SetMicFilterOn") {
            int position = 0;
            gboolean on = TRUE;
            g_variant_get(params, "(ib)", &position, &on);
            mixer_.set_mic_filter_on(position, on);
        } else if (method == "SetMicFilterValue") {
            int position = 0;
            double value = 0;
            g_variant_get(params, "(i&sd)", &position, &a, &value);
            mixer_.set_mic_filter_value(position, a, value);
        } else if (method == "SetSetting") {
            g_variant_get(params, "(&s&s)", &a, &b);
            // The value arrives JSON-encoded ("\"default\"", "null", "true").
            const json value = json::parse(b, nullptr, /*allow_exceptions=*/false);
            if (value.is_discarded())
                throw mixer::MixerError(mixer::MixerError::Kind::Failed, "json_value isn't valid JSON");
            mixer_.set_setting(a, value);
            if (std::string_view(a) == "autostart" && value.is_boolean())
                set_autostart(bus_, value.get<bool>());
        } else if (method == "Reset") {
            mixer_.reset();
            set_autostart(bus_, core::default_config()["settings"]["autostart"].get<bool>());
        }
        changed();
        g_dbus_method_invocation_return_value(invocation, result);
    } catch (const mixer::MixerError& e) {
        g_dbus_method_invocation_return_dbus_error(invocation, error_name(e.kind()).c_str(), e.what());
    } catch (const std::exception& e) {
        // Anything else (a module pipewire-pulse refused to load, ...) is a plain failure.
        g_dbus_method_invocation_return_dbus_error(
            invocation, error_name(mixer::MixerError::Kind::Failed).c_str(), e.what());
    }
}

// =====================================================================================
// Quit and Restart
// =====================================================================================

// Activates the window's "quit" action over D-Bus, without starting a window.
void Service::close_window()
{
    // The mixer window is a separate process (a GApplication); ask it to quit through
    // its "quit" action. NO_AUTO_START: if no window is open, don't start one.
    if (!bus_)
        return;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, std::string(core::kAppId).c_str(), "/io/github/ChromieVT/WaveMix", "org.gtk.Actions",
        "Activate", g_variant_new_parsed("('quit', @av [], @a{sv} {})"), nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr);
    if (reply)
        g_variant_unref(reply);  // no reply just means no window was open
}

// Saves levels, closes the window and leaves the main loop.
void Service::quit()
{
    log("quitting (channels stay as they are until the next login)");
    close_window();
    mixer_.shutdown();
    g_main_loop_quit(loop_);
}

// Saves levels, flushes the bus and execs this binary again with the same arguments.
void Service::restart()
{
    log("restarting");
    mixer_.shutdown();
    if (bus_)
        g_dbus_connection_flush_sync(bus_, nullptr, nullptr);

    // Replace this process with a fresh copy of itself, same arguments. The bus
    // connection closes on exec, which releases the name for the new process.
    std::vector<char*> argv;
    for (auto& arg : options_.argv)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv(core::self_executable().c_str(), argv.data());
    log_error(std::string("could not restart: ") + std::strerror(errno));
    g_main_loop_quit(loop_);
}

}  // namespace wavemix::daemon
