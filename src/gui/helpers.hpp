// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Small building blocks the window and its widgets share, so every label, menu and
// app icon looks and behaves the same everywhere.

#pragma once

#include <gtkmm.h>

#include <functional>
#include <initializer_list>
#include <string>

#include "core/json_file.hpp"

namespace wavemix::gui {

// A JSON value as text, or "" if it isn't a string.
std::string str(const core::json& value);

// A managed label with these CSS classes, horizontal alignment and wrapping.
Gtk::Label* make_label(const std::string& text, std::initializer_list<const char*> classes = {},
                       float xalign = 0.5F, bool wrap = false);

// An ellipsizing label that never asks for more width than it's given.
Gtk::Label* shrinkable(Gtk::Label* label);

// A flat icon button that opens a popover.
Gtk::MenuButton* flat_menu_button(const char* icon, const std::string& tooltip);

// The box a popover's menu entries go in.
Gtk::Box* popover_box(Gtk::Popover& popover);

// A menu entry inside a popover: a flat button that closes the popover, then acts.
Gtk::Button* menu_item(Gtk::Popover& popover, Gtk::Box& box, const std::string& text,
                       std::function<void()> action, bool sensitive = true, bool destructive = false);

// An app's icon, as GetState gives it: a theme icon name or an icon file. Anything the
// icon theme doesn't have gets the generic program icon, never GTK's "missing" picture.
Gtk::Image* app_icon(const core::json& app);

// Removes every child widget from a box.
void remove_children(Gtk::Box& box);

// Level meters (the mic's and the channels'): a bar from -60 dB to 0 dB, green up to
// -9 dB, amber up to -3 dB, red above. (GTK's own thresholds would show ordinary speech
// in amber.)
void setup_meter(Gtk::LevelBar& bar);

// A meter's position for a peak (linear, 1.0 is full scale): -60 dB to 0 dB across it.
double meter_position(double peak);

// Where a meter goes next, from where it is: up to a new peak at once, otherwise back
// down by 1.5 dB per update, like a mixing desk's, so speech reads as a level rather
// than a flicker.
double meter_fall(double shown, double peak);

}  // namespace wavemix::gui
