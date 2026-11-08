// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Loading config.json and state.json: every case in config_loading.json, from missing
// files to broken JSON to partial files.

#include <gtest/gtest.h>

#include <fstream>

#include "core/config.hpp"
#include "golden.hpp"

using namespace wavemix;
namespace fs = std::filesystem;

namespace {

// Writes `text` to `path`, creating its folder first.
void write_file(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

}  // namespace

// Loading config.json and state.json matches the reference for every golden case,
// and never changes the files.
TEST(Config, LoadingMatchesReference)
{
    const auto golden = test::load_golden("config_loading.json");
    for (const auto& c : golden["cases"]) {
        SCOPED_TRACE(c["case"].get<std::string>());
        test::TempDir dir;
        const auto paths = core::make_paths(dir.path(), dir.path());

        if (c["config.json"].is_string())
            write_file(paths.config_file, c["config.json"].get<std::string>());
        if (c["state.json"].is_string())
            write_file(paths.state_file, c["state.json"].get<std::string>());

        EXPECT_TRUE(test::same_json(core::load_config(paths), c["load_config"]));
        EXPECT_TRUE(test::same_json(core::load_state(paths), c["load_state"]));

        // Loading must never create, move or delete files: check what's left.
        std::vector<std::string> files;
        if (fs::exists(paths.config_dir)) {
            for (const auto& entry : fs::directory_iterator(paths.config_dir))
                files.push_back(entry.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        EXPECT_EQ(nlohmann::json(files), c["files_after"]);
    }
}

// A saved config loads back the same, unknown keys included.
TEST(Config, SaveAndLoadRoundTrip)
{
    test::TempDir dir;
    const auto paths = core::make_paths(dir.path(), dir.path());

    auto config = core::default_config();
    config["channels"].push_back({{"id", "game"}, {"label", "Game"}, {"icon", "x"}});
    config["from_a_newer_version"] = "kept";  // unknown keys must survive
    core::save_config(paths, config);

    EXPECT_EQ(core::load_config(paths), config);
}

// Saving twice leaves only state.json, no temporary files.
TEST(Config, SavesWithoutLeavingTemporaryFiles)
{
    test::TempDir dir;
    const auto paths = core::make_paths(dir.path(), dir.path());
    core::save_state(paths, core::default_state());
    core::save_state(paths, core::default_state());

    int count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(paths.config_dir))
        ++count;
    EXPECT_EQ(count, 1);  // just state.json, no state.json.tmp.<pid>
}

// A "settings" that isn't an object is replaced by the default settings.
TEST(Config, BrokenSettingsFallBackToDefaults)
{
    // Unusable settings must not stop the daemon: load the defaults instead.
    test::TempDir dir;
    const auto paths = core::make_paths(dir.path(), dir.path());
    write_file(paths.config_file, R"({"version": 2, "settings": [1, 2]})");
    EXPECT_EQ(core::load_config(paths)["settings"], core::default_config()["settings"]);
}

// A "pulse_routing" setting left from an older WaveMix is dropped on loading.
TEST(Config, TheOldPulseRoutingSettingIsDropped)
{
    test::TempDir dir;
    const auto paths = core::make_paths(dir.path(), dir.path());
    write_file(paths.config_file, R"({"version": 2, "settings": {"pulse_routing": true, "vod_enabled": true}})");
    const auto settings = core::load_config(paths)["settings"];
    EXPECT_FALSE(settings.contains("pulse_routing"));
    EXPECT_EQ(settings["vod_enabled"], true);
}

// Channels are in the VOD unless they say "in_vod": false.
TEST(Config, ChannelsAreInTheVodUnlessLeftOut)
{
    // Files from before the VOD Mix have no in_vod: those channels are in it.
    EXPECT_TRUE(core::in_vod(core::json::parse(R"({"id": "game"})")));
    EXPECT_TRUE(core::in_vod(core::json::parse(R"({"id": "game", "in_vod": true})")));
    EXPECT_FALSE(core::in_vod(core::json::parse(R"({"id": "music", "in_vod": false})")));
}

// The VOD Mix is off by default, and only channels in it want a VOD loopback when on.
TEST(Config, TheVodMixIsOffByDefault)
{
    EXPECT_FALSE(core::vod_enabled(core::default_config()));
    const auto on = core::json::parse(R"({"settings": {"vod_enabled": true}})");
    EXPECT_TRUE(core::vod_enabled(on));
    EXPECT_TRUE(core::wants_vod_link(on, core::json::parse(R"({"id": "game"})")));
    EXPECT_FALSE(core::wants_vod_link(on, core::json::parse(R"({"id": "music", "in_vod": false})")));
    EXPECT_FALSE(core::wants_vod_link(core::default_config(), core::json::parse(R"({"id": "game"})")));
}

// In a Flatpak, the login file goes to the host's config folder, the rest to the app's.
TEST(Paths, FlatpakSplitsTheLoginFileFromOurFiles)
{
    const auto paths = core::make_paths("/sandbox/config", "/home/me/.config");
    EXPECT_EQ(paths.config_file, "/sandbox/config/wavemix/config.json");
    EXPECT_EQ(paths.state_file, "/sandbox/config/wavemix/state.json");
    EXPECT_EQ(paths.login_conf, "/home/me/.config/pipewire/pipewire-pulse.conf.d/60-wavemix.conf");
}

// self_executable() is the real test binary, not the /proc/self/exe link.
TEST(Paths, SelfExecutableIsTheRealBinary)
{
    // Starting another copy through this path must keep the program's own name in
    // process lists, so it can't be the /proc/self/exe link itself.
    const fs::path self = core::self_executable();
    EXPECT_NE(self, "/proc/self/exe");
    EXPECT_TRUE(self.is_absolute());
    EXPECT_TRUE(fs::exists(self));
    EXPECT_EQ(self.filename(), "core-tests");
}
