// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The mixer window. It shows the daemon's state (GetState) and turns clicks into
// daemon calls; it keeps no state of its own beyond what's on screen. Whenever the
// daemon reports a change, the window asks for the state again and redraws.

#pragma once

#include <gtkmm.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "client/client.hpp"
#include "core/json_file.hpp"

namespace wavemix::gui {

class ChannelColumn;
class MicColumn;
class OutputsPanel;

class MixerWindow : public Gtk::ApplicationWindow {
public:
    // Add it to the application with Gtk::Application::add_window() before showing it.
    // Builds the window and shows the daemon's current state.
    explicit MixerWindow(client::Client& client);
    // Stops listening to the client and stops the timers.
    ~MixerWindow() override;

    // ---- Used by the widgets -------------------------------------------------------------
    // The daemon client the widgets send their calls through.
    client::Client& client() { return client_; }
    // The latest GetState, which the widgets read from.
    const core::json& state() const { return state_; }

    // Shows a message in the bar at the top of the window.
    void show_error(const std::string& message);

    // Runs a daemon call, shows its error if it fails, then refreshes.
    void call(const std::function<void()>& action);

    // Moves a stream's app into a channel ("" sends it back to the personal output).
    void assign(uint32_t stream, const std::string& channel);
    // Adds a channel with this label and icon.
    void add_channel(const std::string& label, const std::string& icon);
    // Renames a channel, asking for the new name first.
    void rename_channel(const core::json& channel);
    // Removes a channel, asking for confirmation first.
    void remove_channel(const core::json& channel);
    // A column was dragged onto another: moves the channel to that place.
    void drop_channel(const std::string& id, int position);
    // Asks where to save a bug report, then writes it.
    void save_bug_report();

private:
    // Asks for the state again shortly (once, for a burst of changes).
    void queue_refresh();
    // Fetches the state and updates or rebuilds the window from it.
    void refresh();
    // Rebuilds the columns: when channels were added, removed, renamed or reordered.
    void rebuild();
    // The ☰ menu in the title bar.
    Gtk::MenuButton* make_main_menu();
    // `left`, if given, goes at the start of the bottom row, across from Close.
    Gtk::Grid& new_form(const std::string& title, Gtk::Widget* left = nullptr);
    // Opens the Preferences dialog.
    void open_preferences();
    // Opens the dialog with the names to pick in OBS.
    void open_obs_sources();
    // Opens the About window.
    void open_about();
    // Opens a web page in the browser.
    void open_link(const std::string& url);
    // Adds the theme rows (built-in theme, custom CSS file) to a form.
    void add_theme_rows(Gtk::Grid& grid, int& row);
    // Adds a fallback device picker (output or mic) to a form, for the `key` setting.
    void add_fallback_row(Gtk::Grid& grid, int& row, const std::string& title, const std::string& key,
                          const core::json& devices);
    // Resets WaveMix to the defaults, asking first.
    void reset_wavemix();
    // Restarts the background service.
    void restart_service();
    // Quits the background service, asking first.
    void quit_service();
    // A toast; it goes away after `seconds`, or stays until hidden when that's 0.
    void show_message(const std::string& text, unsigned seconds = 0);
    // Hides the toast.
    void hide_message();

    // Dialogs: one at a time, modal over the window, closed by their buttons, the
    // window's close button or Escape.
    // Creates the dialog (closing any other) and returns it.
    Gtk::Window& new_dialog(const std::string& title);
    // Closes the open dialog, if any.
    void close_dialog();
    // A question with Cancel and `yes` buttons; `on_yes` runs only if the user picks
    // `yes`, `on_no` (if any) after Cancel. Cancel is the default, so Enter never does
    // the destructive thing.
    // `title` is the dialog's short window title, so the question isn't shown twice.
    void confirm(const std::string& title, const std::string& question, const std::string& detail,
                 const std::string& yes, std::function<void()> on_yes, std::function<void()> on_no = {});

    client::Client& client_;
    core::json state_ = core::json::object();

    // What the columns were built from: (id, label, icon, in_vod) per channel. A change here
    // means rebuilding the columns; anything else is just an update.
    std::vector<std::tuple<std::string, std::string, std::string, bool>> structure_;
    bool built_ = false;
    bool built_with_vod_ = false;  // whether the columns were built with VOD buttons
    bool built_with_mic_ = false;  // whether the daemon sent a mic when they were built
    std::map<std::string, ChannelColumn*> columns_;
    MicColumn* mic_ = nullptr;  // owned by body_; none while the mic is disabled or disconnected
    // Every column's top row and footer, each kept the same height so the sliders
    // between them, and their labels, line up.
    Glib::RefPtr<Gtk::SizeGroup> tops_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::VERTICAL);
    Glib::RefPtr<Gtk::SizeGroup> footers_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::VERTICAL);

    Gtk::Overlay overlay_;
    Gtk::Box outer_{Gtk::Orientation::VERTICAL};
    Gtk::Revealer toast_;
    Gtk::Label message_;
    sigc::connection toast_timer_;
    OutputsPanel* outputs_ = nullptr;  // owned by outer_
    Gtk::Box body_{Gtk::Orientation::VERTICAL};
    sigc::connection refresh_pending_;
    std::pair<std::string, std::string> theme_shown_;  // the theme and custom file in use
    sigc::connection meter_timer_;  // asks for the meters while the window is on screen
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // for replies after it's gone
    // Passes GetMeters' peaks on to the columns.
    void show_meters(const core::json& peaks);

    // The dialog that's open, if any (rename or settings). Kept here so it lives as
    // long as it's shown.
    std::unique_ptr<Gtk::Window> dialog_;
};

}  // namespace wavemix::gui
