// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "daemon/autostart.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "core/json_file.hpp"
#include "core/log.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"

namespace fs = std::filesystem;

namespace wavemix::daemon {

namespace {

// Asks the Background portal (asynchronously) to add or remove the login entry,
// logging a warning if it refuses.
void request_background(GDBusConnection* bus, bool enabled)
{
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string("wavemix_autostart"));
    g_variant_builder_add(&options, "{sv}", "reason",
                          g_variant_new_string("Keeps your WaveMix channels and app routing working "
                                               "from the moment you log in."));
    g_variant_builder_add(&options, "{sv}", "autostart", g_variant_new_boolean(enabled));
    const gchar* const commandline[] = {"wavemix", "--daemon", "--login", nullptr};
    g_variant_builder_add(&options, "{sv}", "commandline", g_variant_new_strv(commandline, -1));
    g_variant_builder_add(&options, "{sv}", "dbus-activatable", g_variant_new_boolean(FALSE));

    g_dbus_connection_call(
        bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Background", "RequestBackground", g_variant_new("(sa{sv})", "", &options),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 10000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (reply) {
                g_variant_unref(reply);
            } else {
                core::log::warning("autostart", std::string("the Background portal refused (you can add "
                                                            "WaveMix to your startup apps instead): ") +
                                                    error->message);
                g_error_free(error);
            }
        },
        nullptr);
}

// Writes the user's autostart .desktop file, with Hidden=true when turned off.
void write_entry(bool enabled)
{
    const char* config_home = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const fs::path base = config_home && *config_home ? fs::path(config_home)
                                                      : fs::path(home ? home : "/") / ".config";
    const fs::path file = base / "autostart" / (std::string(core::kAppId) + ".Daemon.desktop");

    std::string text = "[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=WaveMix service\n"
                       "Comment=Routes apps into WaveMix channels and keeps their levels\n"
                       "Exec=" + core::self_executable() + " --daemon --login\n"
                       "Icon=" + std::string(core::kAppId) + "\n"
                       "NoDisplay=true\n";
    text += enabled ? "X-GNOME-Autostart-enabled=true\n" : "Hidden=true\nX-GNOME-Autostart-enabled=false\n";
    try {
        core::atomic_write_text(file, text);
        core::log::info("autostart", std::string(enabled ? "on" : "off") + ": " + file.string());
    } catch (const std::system_error& e) {
        core::log::error("autostart", "could not write " + file.string() + ": " + e.what());
    }
}

}  // namespace

// Uses the portal inside a Flatpak, otherwise writes the autostart entry directly.
void set_autostart(GDBusConnection* bus, bool enabled)
{
    if (core::in_flatpak())
        request_background(bus, enabled);
    else
        write_entry(enabled);
}

}  // namespace wavemix::daemon
