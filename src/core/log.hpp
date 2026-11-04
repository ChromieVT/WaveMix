// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// WaveMix's log: one line per event, to stderr and to a log file.
//
//   2026-10-27 00:13:57.760 W plumbing: loopback music:vod didn't appear
//
// Every line has the time, a level (E error, W warning, I info, D debug) and the part
// of WaveMix it comes from. The file is what a bug report includes, so the people
// reporting problems on distributions and desktops we can't test on don't have to
// know where the journal is. It's kept small: past kMaxLogSize it's moved to
// "<file>.1" (replacing the previous one) and started again.
//
// Until init() is called (the CLI, the development tools), lines only go to stderr.
// Safe to use from any thread.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace wavemix::core::log {

enum class Level { Error, Warning, Info, Debug };

inline constexpr std::uintmax_t kMaxLogSize = 1024 * 1024;

// Starts writing to `file` too (its folder is created). Debug lines are only written
// when `verbose`. Can be called again, for example by a test.
void init(const std::filesystem::path& file, bool verbose);

// Stops writing to the file; lines go to stderr only, at the info level.
void reset();

// True when debug lines are written, for skipping work that only debug lines need.
bool verbose();

// Writes one line at `level` from `component` to stderr and, after init(), to the log file.
void write(Level level, std::string_view component, std::string_view message);

// Writes an error line (E).
inline void error(std::string_view component, std::string_view message) { write(Level::Error, component, message); }
// Writes a warning line (W).
inline void warning(std::string_view component, std::string_view message) { write(Level::Warning, component, message); }
// Writes an info line (I).
inline void info(std::string_view component, std::string_view message) { write(Level::Info, component, message); }
// Writes a debug line (D); dropped unless init() was called with `verbose`.
inline void debug(std::string_view component, std::string_view message) { write(Level::Debug, component, message); }

// One line as written, without the newline. Separate so it can be tested.
std::string format_line(std::chrono::system_clock::time_point time, Level level, std::string_view component,
                        std::string_view message);

}  // namespace wavemix::core::log
