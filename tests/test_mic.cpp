// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The microphone chain's plan and module arguments (core/mic.hpp).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "core/login_conf.hpp"
#include "core/mic.hpp"
#include "golden.hpp"

using namespace wavemix;

namespace {

const core::MicPlugins kAll{
    {core::MicFilter::Noise, "/usr/lib/ladspa/librnnoise_ladspa.so"},
    {core::MicFilter::Gain, "/usr/lib/ladspa/amp.so"},
    {core::MicFilter::Gate, "/usr/lib/ladspa/lsp-plugins-ladspa.so"},
    {core::MicFilter::Expander, "/usr/lib/ladspa/lsp-plugins-ladspa.so"},
    {core::MicFilter::Equalizer, "/usr/lib/ladspa/lsp-plugins-ladspa.so"},
    {core::MicFilter::Compressor, "/usr/lib/ladspa/lsp-plugins-ladspa.so"},
    {core::MicFilter::Limiter, "/usr/lib/ladspa/lsp-plugins-ladspa.so"},
};

// A config whose mic chain is the JSON array `chain`.
core::json with_chain(const char* chain)
{
    return core::json::parse(std::string(R"({"mic": {"chain": )") + chain + "}}");
}

// What the plan runs, as "<node>=<filter>" in order.
std::vector<std::string> stages(const core::MicPlan& plan)
{
    std::vector<std::string> out;
    for (const auto& stage : plan.stages)
        out.push_back(stage.node() + "=" + std::string(core::to_string(stage.filter)));
    return out;
}

}  // namespace

// With no chain configured, the plan has the input and no filters.
TEST(Mic, TheChainStartsEmpty)
{
    const auto plan = core::plan_mic(core::json::object(), std::string("alsa_input.usb-mic"), kAll);
    ASSERT_TRUE(plan);
    EXPECT_TRUE(plan->stages.empty());
    EXPECT_EQ(plan->input, "alsa_input.usb-mic");
}

// The plan runs the filters in the user's order, numbered s1, s2, ...
TEST(Mic, TheChainKeepsTheUsersOrder)
{
    const auto plan = core::plan_mic(
        with_chain(R"([{"filter": "limiter"}, {"filter": "equalizer"}, {"filter": "gain"}])"), std::string("mic"),
        kAll);
    EXPECT_EQ(stages(*plan), (std::vector<std::string>{"s1=limiter", "s2=equalizer", "s3=gain"}));
}

// The same filter can appear in the chain more than once.
TEST(Mic, AFilterCanBeInTheChainTwice)
{
    const auto plan = core::plan_mic(with_chain(R"([{"filter": "compressor"}, {"filter": "compressor"}])"),
                                     std::string("mic"), kAll);
    EXPECT_EQ(stages(*plan), (std::vector<std::string>{"s1=compressor", "s2=compressor"}));
}

// Filters that are off, not installed or unknown don't run, but keep their positions.
TEST(Mic, FiltersOffNotInstalledOrUnknownAreLeftOut)
{
    const auto config = with_chain(R"([{"filter": "noise"}, {"filter": "gate", "on": false},
                                       {"filter": "reverb"}, {"filter": "compressor"}, "limiter"])");
    core::MicPlugins no_rnnoise = kAll;
    no_rnnoise.erase(core::MicFilter::Noise);
    const auto plan = core::plan_mic(config, std::string("mic"), no_rnnoise);
    // Positions follow the list as the user sees it: the gate, off, is still second.
    EXPECT_EQ(stages(*plan), (std::vector<std::string>{"s3=compressor"}));
    ASSERT_EQ(core::mic_chain(config).size(), 3U);
    EXPECT_FALSE(core::mic_chain(config)[1].on);
}

// A chain as long as the list allows runs every filter.
TEST(Mic, EveryFilterInTheListRuns)
{
    // One node holds the whole chain, so there's no limit below the list's own 16.
    std::string chain = "[";
    for (std::size_t i = 0; i < core::kMaxMicChain; ++i)
        chain += std::string(i ? ", " : "") + R"({"filter": "gain"})";
    const auto config = with_chain((chain + "]").c_str());
    EXPECT_EQ(core::plan_mic(config, std::string("mic"), kAll)->stages.size(), core::kMaxMicChain);
}

// The chain's delay adds up only the filters that actually run.
TEST(Mic, TheDelayIsWhatTheRunningFiltersAddUpTo)
{
    // Noise removal 10 ms, the limiter 5.3 ms, the rest nothing; a filter that's off
    // or not installed adds nothing either.
    const auto config = with_chain(R"([{"filter": "noise"}, {"filter": "gate"}, {"filter": "compressor"},
                                       {"filter": "limiter"}, {"filter": "limiter", "on": false}])");
    EXPECT_NEAR(core::mic_chain_delay_ms(*core::plan_mic(config, std::string("mic"), kAll)), 15.3, 1e-9);
    core::MicPlugins no_rnnoise = kAll;
    no_rnnoise.erase(core::MicFilter::Noise);
    EXPECT_NEAR(core::mic_chain_delay_ms(*core::plan_mic(config, std::string("mic"), no_rnnoise)), 5.3, 1e-9);
    EXPECT_EQ(core::mic_chain_delay_ms(*core::plan_mic(core::json::object(), std::string("mic"), kAll)), 0.0);
}

// There's no plan while the mic is off or has no input.
TEST(Mic, NoChainWhenOffOrWithoutAnInput)
{
    EXPECT_FALSE(core::plan_mic(core::json::object(), std::nullopt, kAll));
    EXPECT_FALSE(core::plan_mic(core::json::parse(R"({"mic": {"enabled": false}})"), std::string("mic"), kAll));
}

// The chain is one filter-chain module, from the input to WaveMix Mic's feed, with
// every filter linked to the next.
TEST(Mic, TheChainIsOneFilterChainFromTheInputToTheFeed)
{
    const auto plan = core::plan_mic(
        with_chain(R"([{"filter": "gate"}, {"filter": "compressor"}, {"filter": "limiter"}])"),
        std::string("usb_mic"), kAll);
    const auto [module, args] = core::mic_chain_module(*plan);
    EXPECT_EQ(module, "libpipewire-module-filter-chain");
    for (const char* part :
         {R"({ type = ladspa name = s1 plugin = "/usr/lib/ladspa/lsp-plugins-ladspa.so" label = "http://lsp-plug.in/plugins/ladspa/gate_mono" })",
          R"({ type = ladspa name = s3 plugin = "/usr/lib/ladspa/lsp-plugins-ladspa.so" label = "http://lsp-plug.in/plugins/ladspa/limiter_mono" })",
          R"({ output = "s1:Output" input = "s2:Input" })", R"({ output = "s2:Output" input = "s3:Input" })",
          R"(capture.props = { node.name = "wavemix_mic_chain_in" target.object = "usb_mic" node.passive = true)",
          R"(playback.props = { node.name = "wavemix_mic_chain")", R"(target.object = "wavemix_mic_feed")"})
        EXPECT_NE(args.find(part), std::string::npos) << part;
}

// With no filters, the chain is a plain loopback from the input to the feed.
TEST(Mic, WithNoFiltersTheChainIsALoopback)
{
    const auto plan = core::plan_mic(core::json::object(), std::string("usb_mic"), {});
    const auto [module, args] = core::mic_chain_module(*plan);
    EXPECT_EQ(module, "libpipewire-module-loopback");
    EXPECT_EQ(args.find("filter.graph"), std::string::npos);
    EXPECT_NE(args.find(R"(target.object = "usb_mic")"), std::string::npos);
    EXPECT_NE(args.find(R"(target.object = "wavemix_mic_feed")"), std::string::npos);
}

// The login file has WaveMix Mic's devices while the mic is on, with or without channels.
TEST(Mic, TheLoginFileHasWaveMixMic)
{
    // WaveMix Mic's two devices, last; the chain is the service's.
    const auto config = core::json::parse(R"({"channels": [{"id": "game", "label": "Game", "icon": ""}],
                                              "mic": {"chain": [{"filter": "noise"}, {"filter": "gate"}]}})");
    const std::string text = core::render_login_conf(config);
    const auto game = text.find("sink_name=wavemix_game ");
    const auto feed = text.find("module-null-sink sink_name=wavemix_mic_feed channels=1 ");
    const auto mic = text.find("module-remap-source source_name=wavemix_mic master=wavemix_mic_feed.monitor ");
    ASSERT_NE(feed, std::string::npos);
    ASSERT_NE(mic, std::string::npos);
    EXPECT_LT(game, feed);
    EXPECT_LT(feed, mic);
    EXPECT_EQ(text.find("ladspa"), std::string::npos) << "the filters run in the service";

    // The mic doesn't need any channels, and it's on unless turned off.
    const std::string mic_only = core::render_login_conf(core::json::object());
    EXPECT_NE(mic_only.find("source_name=wavemix_mic "), std::string::npos);
    EXPECT_NE(mic_only.find("sink_name=wavemix_personal_mix "), std::string::npos) << "the Monitor plays into it";
    EXPECT_NE(mic_only.find("sink_name=wavemix_stream_mix "), std::string::npos) << "the Stream Mix is always there";

    // No channels and the mic off: only the mixes OBS records, so it never loses them.
    const std::string bare = core::render_login_conf(
        core::json::parse(R"({"settings": {"vod_enabled": true}, "mic": {"enabled": false}})"));
    EXPECT_NE(bare.find("sink_name=wavemix_stream_mix "), std::string::npos);
    EXPECT_NE(bare.find("sink_name=wavemix_vod_mix "), std::string::npos);
    EXPECT_EQ(bare.find("wavemix_personal_mix"), std::string::npos) << "nothing plays into it";
    EXPECT_EQ(bare.find("wavemix_mic"), std::string::npos);
}

// Adding, moving, switching off and removing filters edit config.json as expected.
TEST(Mic, FiltersAreAddedRemovedAndMoved)
{
    core::json config = core::json::object();  // no "mic" section yet
    EXPECT_EQ(core::add_mic_filter(config, "compressor", -1), 0U);
    EXPECT_EQ(core::add_mic_filter(config, "limiter", 99), 1U);  // out of range: the end
    EXPECT_EQ(core::add_mic_filter(config, "gate", 0), 0U);
    const auto names = [&] {
        std::vector<std::string> out;
        for (const auto& step : core::mic_chain(config))
            out.emplace_back(core::to_string(step.filter));
        return out;
    };
    EXPECT_EQ(names(), (std::vector<std::string>{"gate", "compressor", "limiter"}));

    core::move_mic_filter(config, 2, 0);
    EXPECT_EQ(names(), (std::vector<std::string>{"limiter", "gate", "compressor"}));
    core::move_mic_filter(config, 0, 50);  // clamped to the end
    EXPECT_EQ(names(), (std::vector<std::string>{"gate", "compressor", "limiter"}));

    core::set_mic_filter_on(config, 1, false);
    EXPECT_FALSE(core::mic_chain(config)[1].on);
    core::remove_mic_filter(config, 0);
    EXPECT_EQ(names(), (std::vector<std::string>{"compressor", "limiter"}));
    EXPECT_EQ(config["mic"]["chain"][0], (core::json{{"filter", "compressor"}, {"on", false}}));
}

// Edits with bad input throw with a message for the user and leave the config alone.
TEST(Mic, BadEditsAreRefusedAndChangeNothing)
{
    core::json config = with_chain(R"([{"filter": "gate"}])");
    const core::json before = config;
    const auto message = [&](auto edit) {
        try {
            edit();
        } catch (const std::invalid_argument& e) {
            return std::string(e.what());
        }
        return std::string("no error");
    };
    EXPECT_EQ(message([&] { core::add_mic_filter(config, "reverb", 0); }), "no filter called 'reverb'");
    EXPECT_EQ(message([&] { core::remove_mic_filter(config, 1); }), "no filter at position 1");
    EXPECT_EQ(message([&] { core::move_mic_filter(config, -1, 0); }), "no filter at position -1");
    EXPECT_EQ(message([&] { core::set_mic_filter_on(config, 3, true); }), "no filter at position 3");
    EXPECT_EQ(message([&] { core::set_mic_filter_value(config, 0, "ratio", 4); }),
              "the gate has no setting 'ratio'");
    EXPECT_EQ(message([&] { core::set_mic_filter_value(config, 0, "threshold", NAN); }),
              "the value must be a number");
    EXPECT_EQ(config, before);

    for (std::size_t i = 1; i < core::kMaxMicChain; ++i)
        core::add_mic_filter(config, "gain", -1);
    EXPECT_EQ(message([&] { core::add_mic_filter(config, "gain", -1); }), "the mic chain holds up to 16 filters");
}

// Settings are clamped to their range, and every default is filled in and in range.
TEST(Mic, SettingsAreClampedAndDefaultsFilledIn)
{
    core::json config = with_chain(R"([{"filter": "compressor", "settings": {"ratio": 400, "bogus": 1}}])");
    EXPECT_EQ(core::mic_chain(config)[0].settings, (std::map<std::string, double>{{"ratio", 20}}));

    core::set_mic_filter_value(config, 0, "threshold", -100);
    const auto settings = core::mic_step_settings(core::mic_chain(config)[0]);
    EXPECT_EQ(settings, (core::json{{"threshold", -60}, {"ratio", 20}}));
    EXPECT_EQ(core::mic_step_settings({core::MicFilter::Limiter, true, {}}), (core::json{{"ceiling", -1}}));

    // Every filter offers something to set, and every default is in its range.
    for (core::MicFilter filter : core::kMicFilters) {
        EXPECT_FALSE(core::mic_controls(filter).empty()) << core::to_string(filter);
        for (const auto& control : core::mic_controls(filter))
            EXPECT_TRUE(control.min <= control.fallback && control.fallback <= control.max) << control.key;
    }
    EXPECT_EQ(core::mic_controls(core::MicFilter::Equalizer).size(), 24U);  // 8 bands
}

// Settings turn into the plugins' own port names, units and fixed values.
TEST(Mic, SettingsBecomeThePluginsPortValues)
{
    const auto values = [](core::MicFilter filter, std::map<std::string, double> settings = {}) {
        return core::mic_port_values({filter, true, std::move(settings)});
    };
    const auto near = [](const core::MicPortValues& got, const core::MicPortValues& want) {
        ASSERT_EQ(got.size(), want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            EXPECT_EQ(got[i].first, want[i].first);
            EXPECT_NEAR(got[i].second, want[i].second, 1e-4) << got[i].first;
        }
    };
    // dB become the plugins' linear gain.
    near(values(core::MicFilter::Gain, {{"gain", 6}}), {{"Gain", 1.99526}});
    near(values(core::MicFilter::Gate), {{"Curve threshold (G)", 0.063096}, {"Reduction (G)", 0.01}});
    near(values(core::MicFilter::Compressor, {{"ratio", 4}}), {{"Attack threshold (G)", 0.177828}, {"Ratio", 4}});
    // Fixed values: RNNoise without its 100 ms look back, the expander downward, the
    // limiter without its gain boost.
    near(values(core::MicFilter::Noise), {{"VAD Threshold (%)", 50}, {"Retroactive VAD Grace (ms)", 0}});
    near(values(core::MicFilter::Expander), {{"Expander mode", 0}, {"Attack threshold (G)", 0.177828}, {"Ratio", 3}});
    near(values(core::MicFilter::Limiter, {{"ceiling", -20}}), {{"Gain boost", 0}, {"Threshold (G)", 0.1}});

    // The equalizer: every band a bell, numbered from 0 in the plugin.
    const auto eq = values(core::MicFilter::Equalizer, {{"band_2_gain", 12}, {"band_2_frequency", 300}});
    ASSERT_EQ(eq.size(), 32U);
    near({eq.begin() + 4, eq.begin() + 8},
         {{"Filter type 1", 1}, {"Frequency 1 (Hz)", 300}, {"Gain 1 (G)", 3.98107}, {"Quality factor 1", 1}});
}

// A plugin in ~/.ladspa is found.
TEST(Mic, PluginsAreFoundInTheUsersLadspaFolder)
{
    // Where a plugin downloaded by hand goes (RNNoise isn't packaged everywhere).
    test::TempDir home;
    std::filesystem::create_directories(home.path() / ".ladspa");
    std::ofstream(home.path() / ".ladspa" / "librnnoise_ladspa.so") << "";
    const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    setenv("HOME", home.path().c_str(), 1);
    const auto found = core::find_mic_plugins();
    setenv("HOME", old_home.c_str(), 1);
    // A system-wide copy would come after it, so this one is found either way.
    ASSERT_TRUE(found.contains(core::MicFilter::Noise));
    EXPECT_EQ(found.at(core::MicFilter::Noise), (home.path() / ".ladspa" / "librnnoise_ladspa.so").string());
}

// Filter names round-trip through to_string() and parse_mic_filter().
TEST(Mic, FilterNames)
{
    for (core::MicFilter filter : core::kMicFilters)
        EXPECT_EQ(core::parse_mic_filter(core::to_string(filter)), filter);
    EXPECT_EQ(core::parse_mic_filter("reverb"), std::nullopt);
    EXPECT_EQ(core::mic_plugin(core::MicFilter::Gain).label, "amp_mono");
}
