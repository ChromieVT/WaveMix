// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The mixer window's application: one window per session (starting `wavemix` again
// just raises it), the dark theme, and the "quit" action the daemon uses to close the
// window when the user quits WaveMix.

#pragma once

namespace wavemix::gui {

// Runs the window until it's closed. Returns the exit code.
int run_window();

}  // namespace wavemix::gui
