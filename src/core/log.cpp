// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/log.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

namespace wavemix::core::log {

namespace {

struct Sink {
    std::mutex mutex;
    fs::path path;  // empty: stderr only
    std::ofstream file;
    bool verbose = false;
};

// The one process-wide log destination, created on first use.
Sink& sink()
{
    static Sink instance;
    return instance;
}

// The single-letter code a level is shown as in a log line (E, W, I or D).
char letter(Level level)
{
    switch (level) {
    case Level::Error:
        return 'E';
    case Level::Warning:
        return 'W';
    case Level::Info:
        return 'I';
    case Level::Debug:
        return 'D';
    }
    return '?';
}

// Opens the file for appending. Called with the lock held.
void open_file(Sink& s)
{
    s.file.close();
    s.file.clear();
    std::error_code error;
    fs::create_directories(s.path.parent_path(), error);
    s.file.open(s.path, std::ios::app);
}

// Moves a full file aside and starts a new one. Called with the lock held. The window
// and the service share the file: whichever finds it full rotates it, and the other
// notices the file is gone and opens the new one (see write()).
void rotate_if_full(Sink& s)
{
    std::error_code error;
    const auto size = fs::file_size(s.path, error);
    if (error || size < kMaxLogSize)
        return;
    s.file.close();
    fs::rename(s.path, s.path.string() + ".1", error);
    open_file(s);
}

}  // namespace

// Points the log at `file` and opens it, replacing any earlier init().
void init(const fs::path& file, bool verbose)
{
    Sink& s = sink();
    std::lock_guard lock(s.mutex);
    s.path = file;
    s.verbose = verbose;
    open_file(s);
}

// Goes back to stderr-only logging at the info level.
void reset()
{
    Sink& s = sink();
    std::lock_guard lock(s.mutex);
    s.file.close();
    s.path.clear();
    s.verbose = false;
}

// Whether debug lines are currently being written.
bool verbose()
{
    Sink& s = sink();
    std::lock_guard lock(s.mutex);
    return s.verbose;
}

// Builds "YYYY-MM-DD HH:MM:SS.mmm L component: message" in local time.
std::string format_line(std::chrono::system_clock::time_point time, Level level, std::string_view component,
                        std::string_view message)
{
    using namespace std::chrono;
    const std::time_t seconds = system_clock::to_time_t(time);
    std::tm local{};
    localtime_r(&seconds, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    const auto millis = duration_cast<milliseconds>(time.time_since_epoch()).count() % 1000;
    char ms[8];
    std::snprintf(ms, sizeof ms, ".%03d", static_cast<int>(millis));

    std::string line = std::string(stamp) + ms + " " + letter(level) + " ";
    line.append(component);
    line += ": ";
    line.append(message);
    return line;
}

// Formats the line and writes it to stderr and the log file, reopening or rotating the
// file as needed. Debug lines are skipped unless verbose.
void write(Level level, std::string_view component, std::string_view message)
{
    Sink& s = sink();
    std::lock_guard lock(s.mutex);
    if (level == Level::Debug && !s.verbose)
        return;
    const std::string line = format_line(std::chrono::system_clock::now(), level, component, message);
    std::cerr << line << std::endl;

    if (s.path.empty())
        return;
    std::error_code error;
    if (!fs::exists(s.path, error))
        open_file(s);  // rotated by the other process, or deleted by the user
    rotate_if_full(s);
    if (s.file) {
        s.file << line << '\n';
        s.file.flush();  // a crash right after must still leave the line in the file
    }
}

}  // namespace wavemix::core::log
