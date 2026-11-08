// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/theme.hpp"

#include <gtkmm.h>

#include <sstream>

namespace wavemix::gui {

namespace {

// Each built-in theme: whether it's dark (for GTK's own widgets), and its colours.
struct Palette {
    const char* id;
    const char* name;
    bool dark;
    const char* colours;
};

// What each variable colours, in the order the palettes list them: the ones a custom
// stylesheet can change.
constexpr const char* kVariables[][2] = {
    {"--wm-bg", "the window's background"},
    {"--wm-surface", "the channel columns, the output rows and the title bar"},
    {"--wm-surface-alt", "buttons, menus and popups"},
    {"--wm-border", "borders, and the scrollbars"},
    {"--wm-text", "text, and the slider knobs"},
    {"--wm-text-dim", "captions and hints, the VOD buttons of channels left out and filters switched off"},
    {"--wm-personal", "everything about what you hear"},
    {"--wm-stream", "everything about what the stream hears"},
    {"--wm-danger", "muted, off, and warnings"},
    {"--wm-on", "switched on (the VOD buttons, the filters' power buttons), and quiet meters"},
    {"--wm-warning", "meters nearing the top"},
    {"--wm-on-accent", "text on the blue buttons"},
    {"--wm-hover", "what buttons and rows are mixed with under the pointer"},
};

constexpr Palette kPalettes[] = {
    {"dark", "Dark", true,
     "#17181c #212329 #2a2d35 #353943 #e7e9ee #9aa1ad #4f8cff #b06cff #ff5c6c #3ccf6e #f2b33d #ffffff #ffffff"},
    {"light", "Light", false,
     "#eef0f3 #ffffff #e4e7ec #cfd4db #1d2025 #5d6470 #2f6fe0 #8a46e0 #d93044 #1f9d55 #c98a0b #ffffff #000000"},
    {"midnight", "Midnight", true,
     "#000000 #0c0d10 #17191e #25282f #e7e9ee #8b929e #4f8cff #b06cff #ff5c6c #3ccf6e #f2b33d #ffffff #ffffff"},
    {"high-contrast", "High Contrast", true,
     "#1c1c1c #000000 #262626 #ffffff #ffffff #e0e0e0 #5cb3ff #d29bff #ff6b6b #4dff88 #ffd23f #000000 #ffffff"},
};

// The built-in palette with this id, or the first (Dark) if there's none.
const Palette& palette(const std::string& id)
{
    for (const auto& p : kPalettes)
        if (id == p.id)
            return p;
    return kPalettes[0];
}

// The palette's colours as (variable, value) pairs.
std::vector<std::pair<std::string, std::string>> colours(const Palette& p)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream values(p.colours);
    std::string value;
    for (const auto& variable : kVariables) {
        values >> value;
        out.emplace_back(variable[0], value);
    }
    return out;
}

// The two stylesheets: the theme (rules and palette), and the user's on top of it.
Glib::RefPtr<Gtk::CssProvider> g_theme;
Glib::RefPtr<Gtk::CssProvider> g_custom;
sigc::connection g_custom_watch;
std::string g_custom_file;
std::string g_custom_stamp;  // the file's modified time and size when it was last loaded
std::function<void(const std::string&)> g_on_error;

// Tells GTK's own widgets to draw dark or light (the setting changed in GTK 4.20).
void use_dark_widgets(bool dark)
{
    auto settings = Gtk::Settings::get_default();
#if GTK_CHECK_VERSION(4, 20, 0)
    settings->property_gtk_interface_color_scheme() =
        dark ? Gtk::InterfaceColorScheme::DARK : Gtk::InterfaceColorScheme::LIGHT;
#else
    settings->property_gtk_application_prefer_dark_theme() = dark;  // the pre-4.20 way
#endif
}

// The custom file's modified time and size, or "" if it can't be read.
std::string stamp(const std::string& path)
{
    try {
        const auto info = Gio::File::create_for_path(path)->query_info(
            G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," G_FILE_ATTRIBUTE_STANDARD_SIZE);
        return info->get_modification_date_time().format_iso8601() + " " + std::to_string(info->get_size());
    } catch (const Glib::Error&) {
        return "";
    }
}

// (Re)loads the user's stylesheet, or clears it when there's none or it can't be read.
void load_custom()
{
    g_custom_stamp = g_custom_file.empty() ? "" : stamp(g_custom_file);
    if (g_custom_file.empty()) {
        g_custom->load_from_string("");
        return;
    }
    if (g_custom_stamp.empty()) {
        g_custom->load_from_string("");
        if (g_on_error)
            g_on_error("Your theme file can't be read: " + g_custom_file);
        return;
    }
    g_custom->load_from_file(Gio::File::create_for_path(g_custom_file));  // mistakes: signal_parsing_error
}

}  // namespace

// Lists the built-in palettes as (id, name) pairs.
std::vector<Theme> builtin_themes()
{
    std::vector<Theme> out;
    for (const auto& p : kPalettes)
        out.push_back({p.id, p.name});
    return out;
}

// Writes a palette's colours as a :root block of --wm-* variables.
std::string theme_palette(const std::string& id)
{
    std::string out = ":root {\n";
    for (const auto& [variable, value] : colours(palette(id)))
        out += "  " + variable + ": " + value + ";\n";
    return out + "}\n";
}

// The fixed stylesheet; all its colours come from the --wm-* variables.
std::string theme_css()
{
    return R"css(
window.background { background-color: var(--wm-bg); color: var(--wm-text); }
/* A popover is only its bubble (contents + arrow): its own surface stays clear. */
popover.background { background-color: transparent; }
/* The title bar: 48 px, room for text that reads easily, with buttons to match. */
headerbar { background: var(--wm-surface); border-bottom: 1px solid var(--wm-border);
            box-shadow: none; color: var(--wm-text); min-height: 48px; padding: 0 8px; margin: 0; }
headerbar > windowhandle > box { min-height: 0; padding-top: 0; padding-bottom: 0; }
headerbar button, headerbar menubutton > button { min-height: 32px; min-width: 32px; padding: 0 6px; margin: 0; }
/* Adwaita spaces title bar buttons with margins above and below, which would make the
   bar taller than 48 px. */
headerbar > windowhandle > box > box > * { margin-top: 0; margin-bottom: 0; }
headerbar .title { color: var(--wm-text); font-weight: bold; }

/* Minimise / maximise / close: a round icon, never a box. The general button rules
   below would otherwise draw a square around them (and :hover beats a plain child
   selector), so every state is listed. The circle is the icon's own background. */
windowcontrols > button, windowcontrols > button:hover, windowcontrols > button:active {
  background: none; border: none; box-shadow: none; padding: 0; margin: 0 3px;
  min-width: 16px; min-height: 16px; border-radius: 9999px;
}
/* 16 px circles: the icon's own 16 px box, whose glyph is drawn smaller inside it. */
windowcontrols > button > image { background-color: var(--wm-surface-alt); border-radius: 9999px; padding: 0;
                                  min-width: 16px; min-height: 16px; }
windowcontrols > button:hover > image { background-color: color-mix(in srgb, var(--wm-surface-alt), var(--wm-hover) 15%); }
windowcontrols > button:active > image { background-color: color-mix(in srgb, var(--wm-surface-alt), var(--wm-hover) 25%); }
label { color: var(--wm-text); }
.dim { color: var(--wm-text-dim); }

.wm-column { background-color: var(--wm-surface); border: 1px solid var(--wm-border);
             border-radius: 12px; padding: 12px 10px; }
.wm-column.wm-add { background-color: transparent; border: 1px dashed var(--wm-border); }
.wm-title { font-weight: bold; font-size: 1.1em; }
.wm-caption { font-size: 0.82em; color: var(--wm-text-dim); }
.wm-header-title { font-weight: bold; font-size: 1.1em; }
.wm-profile-name { font-weight: bold; }
.wm-setup-title { font-weight: bold; font-size: 1.4em; }
.wm-header-version { font-size: 0.82em; color: var(--wm-text-dim); }
.wm-about-name { font-size: 1.8em; font-weight: bold; }
.wm-about-version { color: var(--wm-personal); font-weight: bold; padding: 4px 16px; border-radius: 9999px;
                    background-color: color-mix(in srgb, var(--wm-personal) 16%, transparent); }
.wm-about-links { background-color: var(--wm-surface); border: 1px solid var(--wm-border); border-radius: 12px;
                  padding: 4px; }
.wm-about-links > button { padding: 10px 12px; border-radius: 8px; }
/* A channel's VOD button: green while the channel is in the VOD Mix, grey when it's out. */
button.wm-vod { font-size: 0.8em; font-weight: bold; padding: 2px 14px; border-radius: 9999px; }
button.wm-vod-on { background-color: color-mix(in srgb, var(--wm-on) 22%, transparent); border-color: var(--wm-on);
                   color: var(--wm-on); }
button.wm-vod-off { background-color: color-mix(in srgb, var(--wm-text-dim) 14%, transparent);
                    border-color: var(--wm-text-dim); color: var(--wm-text-dim); }
button.wm-vod-on:hover { background-color: color-mix(in srgb, var(--wm-on) 32%, transparent); }
button.wm-vod-off:hover { background-color: color-mix(in srgb, var(--wm-text-dim) 24%, transparent); }
/* A column being dragged over: where it will land. */
.wm-column:drop(active) { border-color: var(--wm-personal); box-shadow: inset 0 0 0 1px var(--wm-personal); }
.wm-level { font-size: 0.9em; font-feature-settings: "tnum"; }
.wm-empty-title { font-size: 1.6em; font-weight: bold; }
.wm-dialog-title { font-size: 1.15em; font-weight: bold; }
.wm-dialog-buttons button { min-width: 88px; padding: 6px 14px; }

button { background-image: none; background-color: var(--wm-surface-alt); color: var(--wm-text);
         border: 1px solid var(--wm-border); border-radius: 8px; box-shadow: none; text-shadow: none; }
button:hover { background-color: color-mix(in srgb, var(--wm-surface-alt), var(--wm-hover) 12%); }
button.wm-mute:checked { background-color: color-mix(in srgb, var(--wm-danger) 25%, transparent);
                         border-color: var(--wm-danger); }
button.flat { background-color: transparent; border-color: transparent; }
button.flat:hover { background-color: var(--wm-surface-alt); }
button.suggested-action,
menubutton.suggested-action > button { background-color: var(--wm-personal); border-color: var(--wm-personal);
                                       color: var(--wm-on-accent); }
menubutton.suggested-action > button label, menubutton.suggested-action > button arrow { color: var(--wm-on-accent); }
button.destructive-action { background-color: transparent; border-color: var(--wm-danger); color: var(--wm-danger); }
menubutton.wm-add-button > button { padding: 8px 14px; }

/* Menu entries inside popovers: flat buttons that line up like a menu. */
button.wm-menu-item { padding: 4px 10px; }
button.wm-menu-item:hover { background-color: color-mix(in srgb, var(--wm-personal) 25%, transparent); }
button.wm-menu-item:disabled label { color: var(--wm-text-dim); }

entry { background-color: var(--wm-bg); color: var(--wm-text); border: 1px solid var(--wm-border);
        border-radius: 8px; }
popover > contents, popover > arrow { background-color: var(--wm-surface-alt); color: var(--wm-text);
                                      border: 1px solid var(--wm-border); }
dropdown > button { padding: 2px 6px; }
list, row { background-color: transparent; color: var(--wm-text); }
/* A drop-down's list: the popup's own colour, not the theme's lighter list background. */
popover listview { background-color: transparent; color: var(--wm-text); }
row:hover { background-color: color-mix(in srgb, var(--wm-hover) 4%, transparent); }
separator { background-color: var(--wm-border); }

/* Messages: a toast floating over the bottom of the window. */
.wm-toast { background-color: var(--wm-surface-alt); border: 1px solid var(--wm-border); border-radius: 10px;
            padding: 8px 8px 8px 14px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.5); }

/* Notifications: cards stacked over the bottom right of the window. The icon takes the
   level's colour: blue for information (the Personal colour), amber for a warning, red
   for an error. */
.wm-notification { background-color: var(--wm-surface-alt); border: 1px solid var(--wm-border); border-radius: 10px;
             padding: 10px 6px 12px 12px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.5); }
.wm-notification-title { font-weight: bold; }
.wm-notification-body { color: var(--wm-text); }
.wm-notification-info .wm-notification-icon { color: var(--wm-personal); }
.wm-notification-warning .wm-notification-icon { color: var(--wm-warning); }
.wm-notification-error .wm-notification-icon { color: var(--wm-danger); }
.wm-notification-error { border-color: color-mix(in srgb, var(--wm-danger) 50%, var(--wm-border)); }
button.wm-notification-button { background-color: transparent; border: 1px solid var(--wm-border); padding: 4px 14px; }
button.wm-notification-button:hover { background-color: color-mix(in srgb, var(--wm-hover) 8%, transparent); }

/* A channel's apps: a strip of icons between the title and the sliders. */
menubutton.wm-apps > button { background-color: var(--wm-bg); border: none; border-radius: 10px;
                              min-height: 44px; padding: 4px 8px; }
menubutton.wm-apps > button:hover { background-color: color-mix(in srgb, var(--wm-bg), var(--wm-hover) 6%); }
menubutton.wm-apps > button > box > arrow { min-width: 0; min-height: 0; -gtk-icon-size: 0; }
checkbutton.wm-app-check { padding: 3px 4px; border-radius: 6px; }
checkbutton.wm-app-check:hover { background-color: color-mix(in srgb, var(--wm-hover) 5%, transparent); }
.wm-more { font-size: 0.85em; color: var(--wm-text-dim); background-color: var(--wm-surface-alt);
           border-radius: 6px; padding: 2px 6px; }

/* Level meters: green, amber near the top, red at the very top. */
levelbar.wm-meter trough { background-color: var(--wm-bg); border: none; border-radius: 3px; min-height: 6px; }
levelbar.wm-meter block { border-radius: 3px; min-height: 6px; }
levelbar.wm-meter block.filled.low { background-color: var(--wm-on); }
levelbar.wm-meter block.filled.high { background-color: var(--wm-warning); }
levelbar.wm-meter block.filled.full { background-color: var(--wm-danger); }
levelbar.wm-meter block.empty { background-color: transparent; }

/* The mic's filters window: the list on the left, the selected filter's settings on
   the right. Filters that aren't running are dimmed. */
.wm-filters-list { padding: 12px; }
.wm-filters-settings { padding: 14px 6px 12px 16px; }
list.wm-filters-rows { background-color: var(--wm-bg); border: 1px solid var(--wm-border); border-radius: 8px; }
list.wm-filters-rows > row { padding: 6px 8px; border-radius: 6px; }
list.wm-filters-rows > row:selected { background-color: color-mix(in srgb, var(--wm-personal) 25%, transparent); }
list.wm-filters-rows > row:drop(active) { box-shadow: inset 0 0 0 1px var(--wm-personal); }
/* A filter's power button: green while it's on, grey when it's off. */
button.wm-power { border-radius: 9999px; min-width: 20px; min-height: 20px; padding: 1px; -gtk-icon-size: 12px;
                  background-color: color-mix(in srgb, var(--wm-text-dim) 14%, transparent);
                  border-color: var(--wm-text-dim); color: var(--wm-text-dim); }
button.wm-power:checked { background-color: color-mix(in srgb, var(--wm-on) 22%, transparent);
                          border-color: var(--wm-on); color: var(--wm-on); }
button.wm-power:hover { background-color: color-mix(in srgb, var(--wm-text-dim) 24%, transparent); }
button.wm-power:checked:hover { background-color: color-mix(in srgb, var(--wm-on) 32%, transparent); }
/* A setting's value box beside its slider: compact, the numbers right-aligned. */
entry.wm-value { min-height: 24px; padding: 0 6px; }
.wm-filter-idle { opacity: 0.6; }
.wm-filter-name { font-weight: bold; }
.wm-filter-why { font-size: 0.82em; color: var(--wm-danger); }
.wm-drag-handle { color: var(--wm-text-dim); }

/* The Outputs rows below the channels. */
.wm-outputs { margin: 0 12px 12px 12px; }
.wm-section { font-size: 0.82em; font-weight: bold; color: var(--wm-text-dim); margin: 2px 2px 0 2px; }
.wm-output-row { background-color: var(--wm-surface); border: 1px solid var(--wm-border); border-radius: 12px;
                 padding: 8px 14px; }
.wm-output-title { font-weight: bold; }
.wm-device { color: var(--wm-text-dim); }

scale trough { background-color: var(--wm-bg); border: none; border-radius: 6px; min-width: 8px; min-height: 8px; }
scale slider { background-color: var(--wm-text); border: none; min-width: 20px; min-height: 20px;
               border-radius: 10px; box-shadow: 0 1px 3px rgba(0, 0, 0, 0.5); }
scale.wm-personal highlight { background-color: var(--wm-personal); border-radius: 6px; }
scale.wm-stream highlight { background-color: var(--wm-stream); border-radius: 6px; }

.wm-danger, .wm-danger label { color: var(--wm-danger); }
.wm-personal-text { color: var(--wm-personal); }
.wm-stream-text { color: var(--wm-stream); }
scrollbar { background-color: transparent; }
scrollbar slider { background-color: var(--wm-border); border-radius: 6px; min-width: 6px; min-height: 6px; }
)css";
}

// Forces Adwaita, installs the theme and custom-stylesheet providers, and loads the
// default theme.
void apply_theme()
{
    // Our own colours are drawn on top of plain Adwaita, whatever theme the desktop
    // uses, so WaveMix looks the same everywhere.
    Gtk::Settings::get_default()->property_gtk_theme_name() = "Adwaita";

    const auto display = Gdk::Display::get_default();
    g_theme = Gtk::CssProvider::create();
    Gtk::StyleProvider::add_provider_for_display(display, g_theme, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    // The user's stylesheet comes after the theme's, so its rules and colours win.
    g_custom = Gtk::CssProvider::create();
    g_custom->signal_parsing_error().connect(
        [](const Glib::RefPtr<const Gtk::CssSection>& section, const Glib::Error& error) {
            if (g_on_error)
                g_on_error("Your theme has a mistake on line " +
                           std::to_string(section->get_start_location().get_lines() + 1) + ": " + error.what());
        });
    Gtk::StyleProvider::add_provider_for_display(display, g_custom, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    set_theme(kPalettes[0].id, "");
}

// Loads the palette and rules for `id`, then watches and loads `custom_file` on top.
void set_theme(const std::string& id, const std::string& custom_file, std::function<void(const std::string&)> on_error)
{
    const Palette& p = palette(id);
    use_dark_widgets(p.dark);
    g_theme->load_from_string(theme_palette(p.id) + theme_css());

    g_on_error = std::move(on_error);
    if (custom_file != g_custom_file) {
        g_custom_file = custom_file;
        g_custom_watch.disconnect();
        if (!custom_file.empty()) {
            // Checked once a second, and reloaded when it's been saved. (Not a file
            // monitor: inside the Flatpak, a file picked through the portal is reached
            // through the portal's own file system, which doesn't report changes.)
            g_custom_watch = Glib::signal_timeout().connect_seconds(
                [] {
                    if (stamp(g_custom_file) != g_custom_stamp)
                        load_custom();
                    return true;
                },
                1);
        }
    }
    load_custom();
}

}  // namespace wavemix::gui
