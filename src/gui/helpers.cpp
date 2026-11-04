// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/helpers.hpp"

#include <algorithm>
#include <cmath>

namespace wavemix::gui {

using core::json;

// The JSON value if it's a string, else "".
std::string str(const json& value)
{
    return value.is_string() ? value.get<std::string>() : std::string();
}

// Creates a managed label and applies the alignment, wrapping and CSS classes.
Gtk::Label* make_label(const std::string& text, std::initializer_list<const char*> classes, float xalign, bool wrap)
{
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_xalign(xalign);
    label->set_wrap(wrap);
    for (const char* c : classes)
        label->add_css_class(c);
    return label;
}

// Makes the label ellipsize and request only one character of width.
Gtk::Label* shrinkable(Gtk::Label* label)
{
    label->set_ellipsize(Pango::EllipsizeMode::END);
    label->set_width_chars(1);
    label->set_max_width_chars(1);
    return label;
}

// Creates a frameless MenuButton with this icon and tooltip.
Gtk::MenuButton* flat_menu_button(const char* icon, const std::string& tooltip)
{
    auto* button = Gtk::make_managed<Gtk::MenuButton>();
    button->set_icon_name(icon);
    button->set_has_frame(false);  // flat (the "flat" class wouldn't reach its inner button)
    button->set_tooltip_text(tooltip);
    return button;
}

// Creates the vertical box that holds a popover's entries and sets it as its child.
Gtk::Box* popover_box(Gtk::Popover& popover)
{
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_margin(6);
    popover.set_child(*box);
    return box;
}

// Appends a flat menu-entry button to `box` that closes `popover` and runs `action`.
Gtk::Button* menu_item(Gtk::Popover& popover, Gtk::Box& box, const std::string& text,
                       std::function<void()> action, bool sensitive, bool destructive)
{
    auto* button = Gtk::make_managed<Gtk::Button>();
    auto* label = make_label(text, {}, 0.0F);
    button->set_child(*label);
    button->add_css_class("flat");
    button->add_css_class("wm-menu-item");
    if (destructive)
        button->add_css_class("wm-danger");
    button->set_sensitive(sensitive);
    button->signal_clicked().connect([&popover, action = std::move(action)] {
        popover.popdown();
        action();
    });
    box.append(*button);
    return button;
}

// Picks the image for an app: its icon file, its theme icon, or the generic program icon.
Gtk::Image* app_icon(const json& app)
{
    auto* image = Gtk::make_managed<Gtk::Image>();
    const std::string icon = str(app["icon"]);
    if (!icon.empty() && icon.front() == '/')
        image->set(Gio::Icon::create(icon));  // a path makes a file icon
    else if (!icon.empty() && Gtk::IconTheme::get_for_display(Gdk::Display::get_default())->has_icon(icon))
        image->set_from_icon_name(icon);
    else
        image->set_from_icon_name("application-x-executable");
    return image;
}

// Removes children from the front until the box is empty.
void remove_children(Gtk::Box& box)
{
    while (auto* child = box.get_first_child())
        box.remove(*child);
}

// Sets the bar's range to 0..1 and its colour thresholds at -9 dB and -3 dB.
void setup_meter(Gtk::LevelBar& bar)
{
    bar.add_css_class("wm-meter");
    bar.set_min_value(0.0);
    bar.set_max_value(1.0);
    bar.add_offset_value("low", 51.0 / 60.0);
    bar.add_offset_value("high", 57.0 / 60.0);
    bar.add_offset_value("full", 1.0);
}

// Maps a linear peak to 0..1 on a -60 dB..0 dB scale.
double meter_position(double peak)
{
    if (peak <= 0.0)
        return 0.0;
    return std::clamp((20.0 * std::log10(peak) + 60.0) / 60.0, 0.0, 1.0);
}

// Jumps up to the new peak, or falls by 1.5 dB from the shown position.
double meter_fall(double shown, double peak)
{
    return std::max(meter_position(peak), shown - 1.5 / 60.0);
}

}  // namespace wavemix::gui
