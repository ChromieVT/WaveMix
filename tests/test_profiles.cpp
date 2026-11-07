// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Profiles: what a snapshot holds, applying one, cleaning up what's imported, names,
// and matching devices on another PC.

#include <gtest/gtest.h>

#include "core/config.hpp"
#include "core/profiles.hpp"

using namespace wavemix;
using core::json;

namespace {

// A config with two channels (Music disabled and out of the VOD), a route, a mic chain,
// picked devices and a theme.
json two_channel_config()
{
    json config = core::default_config();
    config["channels"] = json::array({
        {{"id", "game"}, {"label", "Game"}, {"icon", "input-gaming-symbolic"}},
        {{"id", "music"}, {"label", "Music"}, {"icon", "audio-x-generic-symbolic"}, {"in_vod", false},
         {"enabled", false}},
    });
    config["assignments"] = json::array({{{"app", "binary:spotify"}, {"label", "Spotify"}, {"channel", "music"}}});
    config["mic"] = {{"enabled", true}, {"input", "alsa_input.usb"},
                     {"chain", json::array({{{"filter", "gate"}, {"on", false}, {"settings", {{"threshold", -30}}}},
                                            {{"filter", "limiter"}}})}};
    config["settings"]["personal_output"] = "alsa_output.headset";
    config["settings"]["fallback_output"] = "alsa_output.speakers";
    config["settings"]["theme"] = "light";
    return config;
}

// Saved levels for the channels in two_channel_config(), plus the mic's.
json two_channel_state()
{
    json state = core::default_state();
    state["levels"] = {{"game:personal", {{"volume", 0.8}, {"muted", false}}},
                       {"game:stream", {{"volume", 0.5}, {"muted", true}}},
                       {"music:personal", {{"volume", 0.3}, {"muted", false}}},
                       {"mic:level", {{"volume", 0.9}, {"muted", false}}}};
    return state;
}

}  // namespace

// A snapshot holds the channels (enabled, VOD, name), their levels, the mic chain, the
// devices and the routes, but not the mic's own levels.
TEST(Profiles, SnapshotHoldsTheSetup)
{
    const json p = core::snapshot_profile(two_channel_config(), two_channel_state(), "Streaming");
    EXPECT_EQ(p["name"], "Streaming");
    ASSERT_EQ(p["channels"].size(), 2U);
    EXPECT_EQ(p["channels"][0]["enabled"], true);
    EXPECT_EQ(p["channels"][0]["in_vod"], true);
    EXPECT_EQ(p["channels"][1]["enabled"], false) << "which channels are enabled";
    EXPECT_EQ(p["channels"][1]["in_vod"], false) << "the VOD status";
    EXPECT_EQ(p["channels"][1]["label"], "Music");
    EXPECT_EQ(p["levels"]["game:stream"]["muted"], true) << "the channels' levels";
    EXPECT_FALSE(p["levels"].contains("mic:level")) << "only the channels' levels";
    EXPECT_EQ(p["mic"]["chain"][0]["settings"]["threshold"], -30) << "the filters, their order and settings";
    EXPECT_EQ(p["mic"]["chain"][1]["filter"], "limiter");
    EXPECT_EQ(p["devices"]["personal_output"], "alsa_output.headset");
    EXPECT_EQ(p["devices"]["fallback_output"], "alsa_output.speakers");
    EXPECT_EQ(p["devices"]["fallback_input"], "");
    EXPECT_EQ(p["assignments"][0]["channel"], "music");
}

// Applying a profile replaces the channels, routes, devices and channel levels, but
// keeps the preferences and the mic's own levels.
TEST(Profiles, ApplyReplacesTheSetupButNotThePreferences)
{
    json config = two_channel_config();
    json state = two_channel_state();
    json other = core::default_config();
    other["channels"] = json::array({{{"id", "voice"}, {"label", "Voice"}, {"icon", ""}}});
    json other_state = core::default_state();
    other_state["levels"] = {{"voice:personal", {{"volume", 0.25}, {"muted", false}}}};
    const json profile = core::snapshot_profile(other, other_state, "Podcast");

    core::apply_profile(config, state, profile);
    ASSERT_EQ(config["channels"].size(), 1U);
    EXPECT_EQ(config["channels"][0]["id"], "voice");
    EXPECT_TRUE(config["assignments"].empty());
    EXPECT_EQ(config["settings"]["personal_output"], "default");
    EXPECT_EQ(config["settings"]["fallback_output"], "");
    EXPECT_EQ(config["settings"]["theme"], "light") << "preferences aren't part of a profile";
    EXPECT_FALSE(state["levels"].contains("game:personal")) << "the old channels' levels go";
    EXPECT_EQ(state["levels"]["voice:personal"]["volume"], 0.25);
    EXPECT_TRUE(state["levels"].contains("mic:level")) << "the mic's own levels stay";
}

// running_config() leaves the disabled channels out.
TEST(Profiles, RunningConfigLeavesOutDisabledChannels)
{
    const json running = core::running_config(two_channel_config());
    ASSERT_EQ(running["channels"].size(), 1U);
    EXPECT_EQ(running["channels"][0]["id"], "game");
}

// An import is cleaned: the name trimmed, bad and repeated channels and their routes
// dropped, levels clamped; non-profiles and newer formats are refused.
TEST(Profiles, CleaningAnImport)
{
    const json raw = {
        {"wavemix_profile", 1},
        {"name", "  From the other PC  "},
        {"channels", json::array({{{"id", "game"}, {"label", "Game"}},
                                  {{"id", "game"}, {"label", "Again"}},
                                  {{"id", "Bad ID!"}},
                                  "not a channel"})},
        {"levels", {{"game:personal", {{"volume", 9.0}, {"muted", "yes"}}}, {"other:personal", {{"volume", 0.5}}}}},
        {"assignments", json::array({{{"app", "binary:a"}, {"channel", "game"}},
                                     {{"app", "binary:b"}, {"channel", "nope"}}})},
        {"mic", {{"input", "alsa_input.x"}, {"chain", json::array({{{"filter", "gate"}}, 42})}}},
    };
    const auto p = core::clean_profile(raw);
    ASSERT_TRUE(p);
    EXPECT_EQ((*p)["name"], "From the other PC");
    ASSERT_EQ((*p)["channels"].size(), 1U) << "repeated and bad IDs are dropped";
    EXPECT_EQ((*p)["levels"]["game:personal"]["volume"], 1.5) << "clamped";
    EXPECT_EQ((*p)["levels"]["game:personal"]["muted"], false);
    EXPECT_FALSE((*p)["levels"].contains("other:personal"));
    ASSERT_EQ((*p)["assignments"].size(), 1U) << "routes to missing channels are dropped";
    EXPECT_EQ((*p)["mic"]["chain"].size(), 1U);
    EXPECT_EQ((*p)["devices"]["personal_output"], "default");

    EXPECT_FALSE(core::clean_profile(json::array()));
    EXPECT_FALSE(core::clean_profile({{"wavemix_profile", 2}})) << "from a newer WaveMix";
}

// Names are trimmed and refused when blank, too long or with control characters, and
// compared without case.
TEST(Profiles, Names)
{
    EXPECT_EQ(core::clean_profile_name("  Stream  "), "Stream");
    EXPECT_EQ(core::clean_profile_name("   "), "");
    EXPECT_EQ(core::clean_profile_name(std::string(41, 'x')), "");
    EXPECT_EQ(core::clean_profile_name("a\nb"), "");

    json config = two_channel_config();
    core::ensure_profiles(config, two_channel_state());
    EXPECT_EQ(config["profile"], "Default");
    ASSERT_EQ(config["profiles"].size(), 1U);
    EXPECT_EQ(core::unique_profile_name(config, "default"), "default (2)") << "names are compared without case";
    EXPECT_EQ(core::unique_profile_name(config, "Gaming"), "Gaming");
    EXPECT_NE(core::find_profile(config, "DEFAULT"), nullptr);
}

// ensure_profiles() drops broken, unnamed and repeated profiles, and uses the stored
// spelling of the one in use.
TEST(Profiles, EnsureKeepsGoodProfilesOnly)
{
    json config = two_channel_config();
    config["profile"] = "streaming";
    config["profiles"] = json::array({
        core::snapshot_profile(two_channel_config(), two_channel_state(), "Streaming"),
        core::snapshot_profile(two_channel_config(), two_channel_state(), "STREAMING"),
        {{"name", ""}},
        "junk",
    });
    core::ensure_profiles(config, two_channel_state());
    ASSERT_EQ(config["profiles"].size(), 1U);
    EXPECT_EQ(config["profile"], "Streaming") << "spelled as the profile is";
}

// Devices this PC has are kept; the others fall back to the default, or to none.
TEST(Profiles, MatchingDevicesOnAnotherPc)
{
    json p = core::snapshot_profile(two_channel_config(), two_channel_state(), "Moved");
    p["devices"]["fallback_input"] = "alsa_input.usb";
    core::match_devices(p, {"alsa_output.speakers"}, {"alsa_input.usb"});
    EXPECT_EQ(p["devices"]["personal_output"], "default") << "not on this PC: the system default";
    EXPECT_EQ(p["devices"]["fallback_output"], "alsa_output.speakers") << "on this PC: kept";
    EXPECT_EQ(p["devices"]["fallback_input"], "alsa_input.usb");
    EXPECT_EQ(p["mic"]["input"], "alsa_input.usb");
    core::match_devices(p, {}, {});
    EXPECT_EQ(p["devices"]["fallback_output"], "");
    EXPECT_EQ(p["mic"]["input"], "default");
}
