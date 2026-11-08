// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The login file has to match the reference byte for byte: it's what
// recreates the channels at login, and the module arguments double as the way the
// daemon recognises its own modules.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "core/links.hpp"
#include "core/login_conf.hpp"
#include "golden.hpp"

using namespace wavemix;

// Property lists, sink arguments and every loopback's module arguments match the reference.
TEST(LoginConf, ModuleArgumentsMatchReference)
{
    const auto golden = test::load_golden("ids_and_names.json");

    for (const auto& c : golden["pa_props"]) {
        core::PropList props;
        for (const auto& [key, value] : c["props"].items())
            props.emplace_back(key, value.get<std::string>());
        EXPECT_EQ(core::pa_props(props), c["formatted"].get<std::string>());
    }

    EXPECT_EQ(core::sink_args("wavemix_game", "Game (WaveMix)", "game"),
              golden["sink_args"].get<std::string>());
    // The loopbacks the service hosts (core/links.hpp).
    EXPECT_EQ(core::link_module_args("game", core::Mix::Personal, core::kPersonalSink),
              golden["link_module_args"]["personal"].get<std::string>());
    EXPECT_EQ(core::link_module_args("game", core::Mix::Stream, core::kMixSink),
              golden["link_module_args"]["stream"].get<std::string>());
    EXPECT_EQ(core::link_module_args("game", core::Mix::Vod, core::kVodSink),
              golden["link_module_args"]["vod"].get<std::string>());
    EXPECT_EQ(core::personal_out_module_args("alsa_output.pci-0000_00_1f.3.analog-stereo"),
              golden["link_module_args"]["personal_out"].get<std::string>());
    EXPECT_EQ(core::mic_monitor_module_args(), golden["link_module_args"]["mic_monitor"].get<std::string>());
}

// The tags wanted_links() gives, in order.
std::vector<std::string> tags(const std::vector<core::LinkSpec>& links)
{
    std::vector<std::string> out;
    for (const auto& link : links)
        out.push_back(link.tag);
    return out;
}

// The login file has the devices (mixes first, then channels) and none of the loopbacks.
TEST(LoginConf, OnlyTheDevicesAreInTheLoginFile)
{
    // The loopbacks are the service's: the file at login has the devices only, so
    // apps and OBS always find them.
    const auto config = core::json::parse(R"({"settings": {"vod_enabled": true}, "channels": [
        {"id": "game", "label": "Game", "icon": ""}]})");
    const std::string text = core::render_login_conf(config);
    EXPECT_EQ(text.find("module-loopback"), std::string::npos);
    EXPECT_EQ(text.find("wavemix.link"), std::string::npos);
    for (const char* sink : {"wavemix_personal_mix", "wavemix_stream_mix", "wavemix_vod_mix", "wavemix_game"})
        EXPECT_NE(text.find(std::string("sink_name=") + sink + " "), std::string::npos) << sink;
    EXPECT_LT(text.find("sink_name=wavemix_personal_mix "), text.find("sink_name=wavemix_game "));
}

// A channel left out of the VOD gets no VOD loopback; the others do.
TEST(Links, ChannelsOutOfTheVodHaveNoVodLoopback)
{
    const auto config = core::json::parse(R"({"settings": {"vod_enabled": true}, "channels": [
        {"id": "game", "label": "Game", "icon": ""},
        {"id": "music", "label": "Music", "icon": "", "in_vod": false}]})");
    EXPECT_EQ(tags(core::wanted_links(config, std::string("alsa_output.speakers"), std::nullopt)),
              (std::vector<std::string>{"game:personal", "game:stream", "game:vod", "music:personal", "music:stream",
                                        "personal:out", "mic:monitor"}));
}

// With the VOD Mix off, no channel gets a VOD loopback.
TEST(Links, NoVodLoopbacksWhenTheVodMixIsOff)
{
    const auto config = core::json::parse(R"({"channels": [{"id": "game", "label": "Game", "icon": ""}]})");
    EXPECT_EQ(tags(core::wanted_links(config, std::string("alsa_output.speakers"), std::nullopt)),
              (std::vector<std::string>{"game:personal", "game:stream", "personal:out", "mic:monitor"}));
}

// Each channel's Personal loopback, and the mic's Monitor, play into the Personal Mix,
// whose own loopback goes to the output device.
TEST(Links, ChannelsPlayIntoThePersonalMix)
{
    // Each channel's Personal loopback plays into the Personal Mix, which has the one
    // loopback to the output device; the Monitor plays into it too.
    const auto config = core::json::parse(R"({"channels": [{"id": "game", "label": "Game", "icon": ""}]})");
    const auto links = core::wanted_links(config, std::string("alsa_output.speakers"), std::nullopt);
    ASSERT_EQ(tags(links), (std::vector<std::string>{"game:personal", "game:stream", "personal:out", "mic:monitor"}));
    EXPECT_NE(links[0].args.find(R"(target.object = "wavemix_personal_mix")"), std::string::npos);
    EXPECT_NE(links[0].args.find(R"(target.object = "wavemix_game" node.passive = true)"), std::string::npos);
    EXPECT_NE(links[2].args.find(R"(target.object = "alsa_output.speakers")"), std::string::npos);
    EXPECT_NE(links[3].args.find(R"(target.object = "wavemix_personal_mix")"), std::string::npos);
    EXPECT_NE(links[3].args.find(R"(target.object = "wavemix_mic")"), std::string::npos);
}

// Without an output device only "personal:out" is missing; with the mic off and no
// channels nothing is wanted.
TEST(Links, WithoutAnOutputOnlyThePersonalMixsLoopbackIsMissing)
{
    const auto config = core::json::parse(R"({"channels": [{"id": "game", "label": "Game", "icon": ""}]})");
    EXPECT_EQ(tags(core::wanted_links(config, std::nullopt)),
              (std::vector<std::string>{"game:personal", "game:stream", "mic:monitor"}));
    // The mic alone still wants the Personal Mix played, and its Monitor.
    EXPECT_EQ(tags(core::wanted_links(core::json::object(), std::string("out"))),
              (std::vector<std::string>{"personal:out", "mic:monitor"}));
    EXPECT_TRUE(core::wanted_links(core::json::parse(R"({"mic": {"enabled": false}})"), std::string("out")).empty());
}

// A "pulse_routing" setting left from an older WaveMix changes nothing: the service
// runs every loopback, and the login file has only the devices.
TEST(Links, AnOldPulseRoutingSettingChangesNothing)
{
    const auto config = core::json::parse(R"({"settings": {"pulse_routing": true},
        "channels": [{"id": "game", "label": "Game", "icon": ""}]})");

    EXPECT_EQ(tags(core::wanted_links(config, std::string("alsa_output.speakers"))),
              (std::vector<std::string>{"game:personal", "game:stream", "personal:out", "mic:monitor"}));
    EXPECT_EQ(core::render_login_conf(config).find("module-loopback"), std::string::npos);
}

// Rendering each golden scenario's final config gives exactly its login file.
TEST(LoginConf, RenderedFileMatchesReference)
{
    // Every scenario that ends with a login file on disk: rendering its final config
    // must give exactly the same text.
    int checked = 0;
    for (const char* name : {"startup_sync", "dbus_methods", "channel_limit"}) {
        SCOPED_TRACE(name);
        const auto scenario = test::load_golden(std::string("scenarios/") + name + ".json");
        const auto& expected = scenario["output"]["60-wavemix.conf"];
        if (!expected.is_string())
            continue;
        const auto config = core::json::parse(scenario["output"]["config.json"].dump());
        EXPECT_EQ(core::render_login_conf(config), expected.get<std::string>());
        ++checked;
    }
    EXPECT_GE(checked, 2);  // guard against the golden files changing under us
}

// With no channels, the login file still has the Stream Mix (and no VOD or Personal Mix).
TEST(LoginConf, NoChannelsKeepsTheStreamMix)
{
    // OBS records the Stream Mix, so it's there even with nothing playing into it.
    const auto text = core::render_login_conf(
        core::json::object({{"channels", core::json::array()}, {"mic", {{"enabled", false}}}}));
    EXPECT_EQ(text.rfind("# Generated by WaveMix", 0), 0U);
    EXPECT_NE(text.find("sink_name=wavemix_stream_mix "), std::string::npos);
    EXPECT_EQ(text.find("wavemix_vod_mix"), std::string::npos) << "the VOD Mix is off by default";
    EXPECT_EQ(text.find("wavemix_personal_mix"), std::string::npos);
}
