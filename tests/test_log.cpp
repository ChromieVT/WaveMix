// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The log (core/log.hpp): the line format, levels, and keeping the file small.

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>

#include "core/log.hpp"
#include "golden.hpp"

using namespace wavemix;
namespace fs = std::filesystem;

namespace {

// A whole file's contents, or "" if it can't be read.
std::string read(const fs::path& file)
{
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

// A log line is "date time.ms L component: message".
TEST(Log, LineFormat)
{
    const auto line = core::log::format_line(std::chrono::system_clock::now(), core::log::Level::Warning,
                                             "plumbing", "loopback music:vod didn't appear");
    EXPECT_TRUE(std::regex_match(
        line, std::regex(R"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} W plumbing: loopback music:vod didn't appear)")))
        << line;
}

// Debug lines are only written once init() was called with verbose on.
TEST(Log, DebugOnlyWhenVerbose)
{
    test::TempDir dir;
    const fs::path file = dir.path() / "state" / "wavemix.log";  // the folder is created

    core::log::init(file, false);
    core::log::info("mixer", "an info line");
    core::log::debug("mixer", "a quiet debug line");
    core::log::error("mixer", "an error line");
    EXPECT_FALSE(core::log::verbose());
    std::string text = read(file);
    EXPECT_NE(text.find(" I mixer: an info line"), std::string::npos);
    EXPECT_NE(text.find(" E mixer: an error line"), std::string::npos);
    EXPECT_EQ(text.find("a quiet debug line"), std::string::npos);

    core::log::init(file, true);
    core::log::debug("mixer", "a verbose debug line");
    EXPECT_NE(read(file).find(" D mixer: a verbose debug line"), std::string::npos);
    core::log::reset();
}

// A file that's already full is moved to "<file>.1" and a new one is started.
TEST(Log, AFullFileMovesAside)
{
    test::TempDir dir;
    const fs::path file = dir.path() / "wavemix.log";
    std::ofstream(file) << std::string(core::log::kMaxLogSize, 'x');  // already full

    core::log::init(file, false);
    core::log::info("mixer", "the first line of the new file");
    core::log::reset();

    EXPECT_EQ(fs::file_size(dir.path() / "wavemix.log.1"), core::log::kMaxLogSize) << "the old one is kept, once";
    const std::string fresh = read(file);
    EXPECT_NE(fresh.find("the first line of the new file"), std::string::npos);
    EXPECT_LT(fresh.size(), 200U);
}

// A log file deleted while it's open is created again on the next line.
TEST(Log, AFileDeletedMeanwhileIsStartedAgain)
{
    test::TempDir dir;
    const fs::path file = dir.path() / "wavemix.log";
    core::log::init(file, false);
    core::log::info("mixer", "before");
    fs::remove(file);  // the other process rotated it, or the user deleted it
    core::log::info("mixer", "after");
    core::log::reset();
    EXPECT_NE(read(file).find("after"), std::string::npos);
}
