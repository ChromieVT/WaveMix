// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The pieces of the mixer window:
//
//   MicColumn       the mic, first: input, Monitor and Mic sliders, its filters
//   ChannelColumn   one channel: title + menu, Personal and Stream sliders, its apps
//     LevelControl  one slider with its % and mute button
//   AddColumn       the "Add channel" button with the presets
//   OutputsPanel    the Personal, Stream and VOD mix rows below the channels
//
// None of them change anything themselves: every action goes to the daemon through
// the window, and the window redraws from the daemon's state.

#pragma once

#include <gtkmm.h>

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/json_file.hpp"

namespace wavemix::gui {

class MixerWindow;

// A box whose content is `width` pixels wide, so every column has the same size.
// (CSS padding and borders come on top, as for any GTK 4 widget.) It never grows
// wider because the labels inside are set to shrink and ellipsize: none of them asks
// for more than a character's width.
class FixedWidthBox : public Gtk::Box {
public:
    // Creates the box with a fixed content width, orientation and spacing.
    FixedWidthBox(int width, Gtk::Orientation orientation, int spacing);
};

class LevelControl : public Gtk::Box {
public:
    // `mix` is "personal", "stream" or "vod". Channel "" is a mix's master: the output
    // device for "personal", the Stream or VOD Mix for the others. Vertical (a channel
    // column, with `caption` above) or horizontal (an output row, without one).
    LevelControl(MixerWindow& window, std::string channel, std::string mix, const std::string& caption,
                 Gtk::Orientation orientation = Gtk::Orientation::VERTICAL);

    // One of the mic's sliders: "monitor" (hearing yourself, coloured like the Personal
    // mix) or "mic" (WaveMix Mic, what OBS records, coloured like the Stream mix).
    struct MicSlider {
        std::string name;
    };
    // Builds one of the mic's sliders, which go through the mic's own D-Bus methods.
    LevelControl(MixerWindow& window, MicSlider slider, const std::string& caption);

    // Shows a level from GetState ({"present", "volume", "muted"}).
    void update(const core::json& level);

private:
    // The slider moved: sends the new level unless it's our own update.
    void on_value_changed();
    // The mute button toggled: sends the mute unless it's our own update.
    void on_mute_toggled();
    // Shows the muted or unmuted icon on the mute button.
    void show_mute_icon(bool muted);

    MixerWindow& window_;
    std::string channel_;
    std::string mix_;
    std::string mic_slider_;  // set for the mic's sliders, which have their own methods
    Glib::RefPtr<Gtk::Adjustment> adjustment_;
    Gtk::Scale scale_;
    Gtk::Label percent_;
    Gtk::ToggleButton mute_;
    bool updating_ = false;  // true while we set the widgets from GetState
    std::chrono::steady_clock::time_point last_touched_;  // the user's last move
};

// GetState lists every audio stream; an app with several (a game, a browser with two
// tabs playing) is shown once. Returns arrays of streams, one per app.
core::json group_by_app(const core::json& streams);

class ChannelColumn : public FixedWidthBox {
public:
    // `index` is the column's place, where a channel dropped onto it goes.
    ChannelColumn(MixerWindow& window, const core::json& channel, int index);

    // Shows the latest levels and apps for this channel.
    void update(const core::json& channel, const core::json& all_channels);

    // Shows the loudest sample played into the channel since the last update (GetMeters).
    void show_meter(double peak);

    // The row above the sliders (the apps strip) and the one below them (the VOD
    // button, or nothing). The window gives every column's the same height, so the
    // sliders and their labels line up.
    // The row above the sliders (see above).
    Gtk::Box& top() { return *top_; }
    // The row below the sliders (see above).
    Gtk::Box& footer() { return *footer_; }

private:
    // The channel's menu: Rename and Remove.
    Gtk::Popover* make_menu();
    // Rebuilds the apps popover from the latest state.
    void fill_apps_popover();

    MixerWindow& window_;
    core::json channel_;
    LevelControl personal_;
    LevelControl stream_;
    Gtk::MenuButton* apps_button_ = nullptr;  // the icon strip; owned by the column
    Gtk::Box* apps_strip_ = nullptr;          // the icons inside it
    Gtk::Popover* apps_popover_ = nullptr;    // owned by apps_button_
    Gtk::Box* top_ = nullptr;
    Gtk::Box* footer_ = nullptr;
    Gtk::LevelBar meter_;  // what plays into the channel, before its sliders
    double meter_shown_ = 0.0;
};

// The mic, the first column (src/gui/mic_column.cpp):
//   title + menu      turning the mic off and on
//   input strip       which microphone: its icon where a channel has its apps; a
//                     click lists the inputs, with a checkbox to pick one
//   Monitor and Mic   hearing yourself, and WaveMix Mic's level (what OBS records)
//   Filters           the chain, in a window of its own (beside the mixer, so the
//                     sliders stay in reach while tuning), laid out like OBS's filters:
//                     the list on the left (each with its switch; add, remove and move
//                     buttons below), the selected filter's settings on the right
class MicColumn : public FixedWidthBox {
public:
    // Builds the mic column: title and menu, input strip and meter, Monitor and Mic sliders,
    // and the Filters button.
    explicit MicColumn(MixerWindow& window);
    // Closes the filters window, if it was opened.
    ~MicColumn() override;

    // Shows GetState's "mic".
    void update(const core::json& mic);

    // Shows GetMeters' peaks (the window asks for them while it's on screen).
    void show_meters(const core::json& peaks);

    // The rows above and below the sliders: the input strip with its meter, and the
    // Filters button (see ChannelColumn::top).
    // The row above the sliders (see above).
    Gtk::Box& top() { return *top_; }
    // The row below the sliders (see above).
    Gtk::Box& footer() { return *footer_; }

private:
    // The mic's menu: Disable mic.
    Gtk::Popover* make_menu();
    // Rebuilds the input popover: every input, with a checkbox to pick one.
    void fill_inputs_popover();
    // Rebuilds the filters window (list, meters, buttons, settings) when the chain's shape changed.
    void fill_filters();
    // One filter's row in the list: power switch, name, why it isn't running, drag handle.
    Gtk::Widget* filter_row(const core::json& step, int position);
    // Fills the settings pane for the selected filter.
    void show_settings();
    // One setting of a filter: a slider and its value, sent live to the daemon.
    Gtk::Widget* control_row(const core::json& control, double value, int position, Gtk::ScrolledWindow& page);
    // Moves the selected filter up (-1) or down (+1).
    void move_selected(int by);

    MixerWindow& window_;
    core::json mic_ = core::json::object();
    Gtk::Label* status_ = nullptr;    // "No microphone found"
    Gtk::MenuButton* input_button_ = nullptr;  // the strip with the mic's icon
    Gtk::Popover* input_popover_ = nullptr;    // owned by input_button_
    LevelControl monitor_;
    LevelControl level_;
    // Opens the filters window, building it the first time.
    void open_filters();

    Gtk::Box* top_ = nullptr;
    Gtk::Box* footer_ = nullptr;
    Gtk::Button* filters_button_ = nullptr;
    std::unique_ptr<Gtk::Window> filters_window_;  // made when first opened
    std::string chain_shown_;  // what the filters window was built from
    int selected_ = 0;         // the filter whose settings are shown, by position
    Gtk::ListBox* list_ = nullptr;        // in the filters window
    Gtk::Box* settings_pane_ = nullptr;   // in the filters window

    // The meters: WaveMix Mic's in the column, and the mic before and after the chain in
    // the filters window.
    Gtk::LevelBar mic_meter_;
    Gtk::LevelBar* input_meter_ = nullptr;   // in the filters window
    Gtk::LevelBar* output_meter_ = nullptr;  // in the filters window
    Gtk::Label* input_db_ = nullptr;
    Gtk::Label* output_db_ = nullptr;
    std::map<std::string, double> shown_;  // what each meter shows, falling back between peaks
};

// The Outputs rows below the channels:
//   Personal mix  where you hear your channels, and their master level (not the device's)
//   Stream mix    what OBS records for the stream, and its master level
//   VOD mix       the same for OBS's VOD track
class OutputsPanel : public Gtk::Box {
public:
    // Builds the three output rows.
    explicit OutputsPanel(MixerWindow& window);

    // Shows the latest levels and outputs from GetState.
    void update(const core::json& state);

private:
    // Adds a row (icon, title, middle widget, slider) and returns it.
    Gtk::Box* add_row(const std::string& title, const char* icon, Gtk::Widget& middle, LevelControl& level);
    // Rebuilds the output device list when it changed.
    void fill_devices(const core::json& state);

    MixerWindow& window_;
    LevelControl personal_;
    LevelControl stream_;
    LevelControl vod_;
    Gtk::DropDown devices_;
    Glib::RefPtr<Gtk::SizeGroup> middles_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);
    std::vector<std::string> device_ids_;  // what each entry of devices_ stands for
    std::string devices_shown_;            // the list devices_ was built from
    bool updating_ = false;                // true while we set devices_ from GetState
    Gtk::Box* vod_row_ = nullptr;          // hidden while the VOD Mix is off
};

class AddColumn : public FixedWidthBox {
public:
    // `used_labels`: presets already in use aren't offered again. `big` is the
    // version shown in the middle of an empty window.
    // Builds the button and its popover of presets and custom name.
    AddColumn(MixerWindow& window, const std::set<std::string>& used_labels, bool big = false);

private:
    MixerWindow& window_;
};

}  // namespace wavemix::gui
