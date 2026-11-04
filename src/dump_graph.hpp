// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#pragma once

namespace wavemix {

// `wavemix --dump-graph`: connects to PipeWire, prints what WaveMix sees (outputs,
// WaveMix channels and their sliders, playing apps) and exits.
//
// It only reads; it never changes anything. It's a development aid for checking what
// the registry sees, for example against `pactl list`.
int dump_graph();

}  // namespace wavemix
