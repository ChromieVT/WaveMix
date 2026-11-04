// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Channel IDs and every derived name must match the reference exactly:
// they end up in sink names, saved levels and users' OBS scenes.

#include <gtest/gtest.h>

#include "core/ids.hpp"
#include "core/names.hpp"
#include "golden.hpp"

using namespace wavemix;

namespace {

// The ids_and_names.json golden file, loaded once.
const nlohmann::json& golden()
{
    static const auto data = test::load_golden("ids_and_names.json");
    return data;
}

}  // namespace

// make_id() gives the reference ID for every label and set of existing IDs.
TEST(Ids, MakeIdMatchesReference)
{
    for (const auto& c : golden()["make_id"]) {
        std::set<std::string, std::less<>> existing;
        for (const auto& id : c["existing"])
            existing.insert(id.get<std::string>());
        EXPECT_EQ(core::make_id(c["label"].get<std::string>(), existing), c["id"].get<std::string>())
            << "label: \"" << c["label"].get<std::string>() << "\"";
    }
}

// valid_id() accepts and rejects exactly the reference IDs.
TEST(Ids, ValidIdMatchesReference)
{
    for (const auto& c : golden()["valid_id"]) {
        EXPECT_EQ(core::valid_id(c["id"].get<std::string>()), c["valid"].get<bool>())
            << "id: \"" << c["id"].get<std::string>() << "\"";
    }
}

// Every derived name (sink, link tag, node name, media.name) matches the reference.
TEST(Names, DerivedNamesMatchReference)
{
    for (const auto& [id, expected] : golden()["names"].items()) {
        EXPECT_EQ(core::base_sink(id), expected["base_sink"].get<std::string>());
        for (core::Mix mix : core::kLinks) {
            const auto& link = expected["links"][std::string(core::to_string(mix))];
            EXPECT_EQ(core::link_tag(id, mix), link["tag"].get<std::string>());
            EXPECT_EQ(core::link_node_name(id, mix), link["node_name"].get<std::string>());
            EXPECT_EQ(core::link_media_name(id, mix), link["media_name"].get<std::string>());
        }
    }
}

// The frozen constants (sink names, D-Bus names, presets, ...) match the reference.
TEST(Names, ConstantsMatchReference)
{
    const auto& c = golden()["constants"];
    EXPECT_EQ(core::kPrefix, c["PREFIX"].get<std::string>());
    EXPECT_EQ(core::kMixSink, c["MIX_SINK"].get<std::string>());
    EXPECT_EQ(core::kMixLabel, c["MIX_LABEL"].get<std::string>());
    EXPECT_EQ(core::kVodSink, c["VOD_SINK"].get<std::string>());
    EXPECT_EQ(core::kVodLabel, c["VOD_LABEL"].get<std::string>());
    EXPECT_EQ(core::kMaxChannels, c["MAX_CHANNELS"].get<int>());
    EXPECT_EQ(core::kDefaultIcon, c["DEFAULT_ICON"].get<std::string>());
    EXPECT_EQ(core::kAppId, c["APP_ID"].get<std::string>());
    EXPECT_EQ(core::kBusName, c["BUS_NAME"].get<std::string>());
    EXPECT_EQ(core::kObjectPath, c["OBJECT_PATH"].get<std::string>());
    EXPECT_EQ(core::kInterface, c["INTERFACE"].get<std::string>());

    ASSERT_EQ(core::kPresets.size(), c["PRESETS"].size());
    for (size_t i = 0; i < core::kPresets.size(); ++i) {
        EXPECT_EQ(core::kPresets[i].label, c["PRESETS"][i]["label"].get<std::string>());
        EXPECT_EQ(core::kPresets[i].icon, c["PRESETS"][i]["icon"].get<std::string>());
    }
}

// parse_mix() knows the three mixes and nothing else.
TEST(Names, ParseMix)
{
    EXPECT_EQ(core::parse_mix("personal"), core::Mix::Personal);
    EXPECT_EQ(core::parse_mix("stream"), core::Mix::Stream);
    EXPECT_EQ(core::parse_mix("vod"), core::Mix::Vod);
    EXPECT_EQ(core::parse_mix("loud"), std::nullopt);
    EXPECT_EQ(core::parse_mix(""), std::nullopt);
}
