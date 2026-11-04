// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Recognising apps, and the slider <-> gain conversion. The cases mirror the streams
// in scenarios/two_channels.json.

#include <gtest/gtest.h>

#include <array>

#include "core/apps.hpp"
#include "golden.hpp"

using namespace wavemix;

// app_key() prefers the binary to the name, and ignores empty values.
TEST(Apps, KeyPrefersTheBinary)
{
    EXPECT_EQ(core::app_key({{"application.process.binary", "spotify"}, {"application.name", "Spotify"}}),
              "binary:spotify");
    EXPECT_EQ(core::app_key({{"application.name", "Firefox"}}), "name:Firefox");
    EXPECT_EQ(core::app_key({{"media.name", "Notification sound"}}), std::nullopt);
    // An empty value counts as missing.
    EXPECT_EQ(core::app_key({{"application.process.binary", ""}, {"application.name", "VLC"}}), "name:VLC");
}

// app_name() takes the first naming property that's set, else "Stream <id>".
TEST(Apps, NameFallsBackInOrder)
{
    EXPECT_EQ(core::app_name({{"application.name", "Discord"}, {"media.name", "x"}}, 1), "Discord");
    EXPECT_EQ(core::app_name({{"application.process.binary", "mpv"}}, 1), "mpv");
    EXPECT_EQ(core::app_name({{"media.name", "Notification sound"}}, 1), "Notification sound");
    EXPECT_EQ(core::app_name({}, 1005), "Stream 1005");
}

// WaveMix's own nodes and other loopbacks count as internal; ordinary apps don't.
TEST(Apps, InternalStreamsAreRecognised)
{
    EXPECT_TRUE(core::is_internal({{"node.name", "wavemix_something_internal"}}));
    EXPECT_TRUE(core::is_internal({{"wavemix.link", "game:personal"}}));
    EXPECT_TRUE(core::is_internal({{"node.name", "output.loopback-x"}}));
    EXPECT_TRUE(core::is_internal({{"media.name", "loopback-123-45"}}));
    EXPECT_FALSE(core::is_internal({{"node.name", "Firefox"}, {"media.name", "YouTube"}}));
}

// The golden scenario's unrouted apps are the three streams that aren't internal.
TEST(Apps, UnroutedAppsInGoldenAreTheOnesWeWouldList)
{
    // In that scenario, exactly these three playing streams are unrouted apps;
    // the two internal ones next to them are hidden.
    const auto state = test::load_golden("scenarios/two_channels.json")["get_state"];
    std::vector<std::string> names;
    for (const auto& app : state["unrouted_apps"])
        names.push_back(app["name"].get<std::string>());
    EXPECT_EQ(names, (std::vector<std::string>{"Discord", "Firefox", "Notification sound"}));
}

// Sliders use PulseAudio's cubic scale, averaging per-channel slider values.
TEST(Volume, SliderUsesTheCubicScale)
{
    // Unity gain is 100%.
    EXPECT_DOUBLE_EQ(core::slider_from_gains(std::array{1.0F, 1.0F}), 1.0);
    // Half the slider is an eighth of the gain.
    EXPECT_NEAR(core::slider_from_gains(std::array{0.125F, 0.125F}), 0.5, 1e-6);
    EXPECT_NEAR(core::gain_from_slider(0.5), 0.125F, 1e-6);
    // Like pulse, unequal channels average their slider values, not their gains.
    EXPECT_NEAR(core::slider_from_gains(std::array{1.0F, 0.0F}), 0.5, 1e-6);
    EXPECT_DOUBLE_EQ(core::slider_from_gains({}), 0.0);
}

// A slider value survives the trip to gain and back.
TEST(Volume, RoundTripsThroughGain)
{
    for (double s : {0.0, 0.2, 0.35, 0.8, 1.0, 1.5})
        EXPECT_NEAR(core::slider_from_gains(std::array{core::gain_from_slider(s)}), s, 1e-4);
}

// Volumes clamp to 0..150% (NaN to 0) and round to 4 decimals.
TEST(Volume, ClampAndRound)
{
    EXPECT_DOUBLE_EQ(core::clamp_volume(2.0), 1.5);
    EXPECT_DOUBLE_EQ(core::clamp_volume(-1.0), 0.0);
    EXPECT_DOUBLE_EQ(core::clamp_volume(std::nan("")), 0.0);
    EXPECT_DOUBLE_EQ(core::round4(0.123456), 0.1235);
}
