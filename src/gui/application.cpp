// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/application.hpp"

#include <gtkmm.h>

#include <chrono>
#include <memory>
#include <thread>

#include "build_config.hpp"
#include "core/log.hpp"
#include "client/client.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"
#include "gui/theme.hpp"
#include "gui/window.hpp"

namespace wavemix::gui {

namespace {

// Lets GTK handle anything pending (D-Bus replies, signals) without blocking.
void run_pending_events()
{
    auto context = Glib::MainContext::get_default();
    while (context->pending())
        context->iteration(false);
}

class Application : public Gtk::Application {
public:
    // Registers under WaveMix's app ID, so a second start reaches this instance.
    Application() : Gtk::Application(std::string(core::kAppId)) {}

protected:
    // Adds the "quit" action, applies the theme and adds the icon search paths.
    void on_startup() override
    {
        Gtk::Application::on_startup();

        // The daemon closes the window through this action when the user quits
        // WaveMix (from the tray, say). While we're replacing an outdated daemon
        // ourselves, its quit request is ignored.
        add_action("quit", [this] {
            if (!ignore_quit_)
                quit();
        });

        apply_theme();

        // Wherever WaveMix runs from (installed under any prefix, or a build folder), make
        // sure GTK can find its icon even if that folder isn't in the icon theme's path.
        auto theme = Gtk::IconTheme::get_for_display(Gdk::Display::get_default());
        if (const auto icons = core::icon_theme_dir(); !icons.empty())
            theme->add_search_path(icons);

        // Other apps' icons, for the app lists: Flatpak apps keep theirs in Flatpak's
        // exports, and some older apps in pixmaps. Inside a Flatpak, the host's /usr is
        // at /run/host/usr. Folders that don't exist are simply skipped by GTK.
        const std::string home = Glib::get_home_dir();
        for (const std::string& dir : {home + "/.local/share/flatpak/exports/share/icons",
                                       std::string("/var/lib/flatpak/exports/share/icons"),
                                       std::string("/run/host/usr/share/pixmaps"),
                                       std::string("/usr/share/pixmaps")})
            theme->add_search_path(dir);
    }

    // Raises the existing window, or connects to the daemon (replacing an outdated one)
    // and opens the mixer window.
    void on_activate() override
    {
        if (window_) {
            window_->present();  // started again: just bring the window up
            return;
        }
        try {
            client_ = std::make_unique<client::Client>();
        } catch (const client::ClientError& e) {
            core::log::error("gui", e.what());
            quit();
            return;
        }
        replace_outdated_service();
        window_ = std::make_unique<MixerWindow>(*client_);
        add_window(*window_);
        window_->present();
    }

private:
    // After an update, the background service from the previous version can still be
    // running (a Flatpak update doesn't restart running apps). If its version differs
    // from ours, stop it and start the one that was just installed.
    void replace_outdated_service()
    {
        std::string running;
        try {
            running = client_->state().value("version", std::string());
        } catch (const client::ClientError&) {
            return;  // not answering: nothing to replace
        }
        if (running == WAVEMIX_VERSION)
            return;
        core::log::info("gui", "service is version " + (running.empty() ? std::string("unknown") : running) +
                                   ", window is " WAVEMIX_VERSION "; restarting the service");

        ignore_quit_ = true;  // the old service's Quit also asks open windows to close
        try {
            client_->quit();
        } catch (const client::ClientError&) {
            ignore_quit_ = false;
            return;  // too old to know how to quit; carry on with it
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (client_->running() && std::chrono::steady_clock::now() < deadline) {
            run_pending_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        client_->ensure_daemon();
        run_pending_events();
        ignore_quit_ = false;
    }

    std::unique_ptr<client::Client> client_;
    std::unique_ptr<MixerWindow> window_;
    bool ignore_quit_ = false;
};

}  // namespace

// Creates the application and runs it with no GTK arguments of our own.
int run_window()
{
    auto app = Glib::make_refptr_for_instance<Application>(new Application());
    // Our own arguments were handled already; GTK gets none.
    char name[] = "wavemix";
    char* argv[] = {name, nullptr};
    return app->run(1, argv);
}

}  // namespace wavemix::gui
