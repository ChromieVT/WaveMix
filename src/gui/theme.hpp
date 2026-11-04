// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// WaveMix's look, the same on every desktop: plain Adwaita as the base, with WaveMix's
// colours on top. The colours are CSS custom properties (--wm-*); a theme is one set of
// them. On top of the chosen theme goes the user's own stylesheet, if they picked one:
// a CSS file that can change a few colours or restyle anything, reloaded live whenever
// it's saved.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace wavemix::gui {

// The "theme" setting for the user's own stylesheet (setting "custom_theme"), on top
// of the default theme.
inline constexpr const char* kCustomTheme = "custom";

// A built-in theme: its id (the "theme" setting) and the name the user sees.
struct Theme {
    std::string id;
    std::string name;
};
// The built-in themes, in the order they're offered.
std::vector<Theme> builtin_themes();

// The stylesheet's rules: every colour in them is a --wm-* variable.
std::string theme_css();

// The --wm-* variables of a built-in theme, as a :root block. An unknown id gives the
// default theme's.
std::string theme_palette(const std::string& id);

// Loads the stylesheet for the whole app, in the default theme. Call once, after the
// app has started (GTK needs a display).
void apply_theme();

// Switches to built-in theme `id`, with the stylesheet in `custom_file` on top (none if
// empty). The file is watched: saved again, it's reloaded. `on_error` hears about
// mistakes in it ("line 3: ..."), or that it can't be read.
void set_theme(const std::string& id, const std::string& custom_file,
               std::function<void(const std::string&)> on_error = {});

}  // namespace wavemix::gui
