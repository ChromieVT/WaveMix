// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/names.hpp"

namespace wavemix::core {

// The D-Bus / link-tag spelling of a mix: "personal", "stream" or "vod".
std::string_view to_string(Mix mix)
{
    switch (mix) {
    case Mix::Personal:
        return "personal";
    case Mix::Stream:
        return "stream";
    case Mix::Vod:
        return "vod";
    }
    return "";
}

// Turns "personal" / "stream" / "vod" into a Mix; nullopt for any other text.
std::optional<Mix> parse_mix(std::string_view text)
{
    if (text == "personal")
        return Mix::Personal;
    if (text == "stream")
        return Mix::Stream;
    if (text == "vod")
        return Mix::Vod;
    return std::nullopt;
}

// The channel's own sink name: "wavemix_" + the channel ID.
std::string base_sink(std::string_view channel_id)
{
    return std::string(kPrefix) + std::string(channel_id);
}

// The link tag for one of a channel's loopbacks: "<id>:<mix>".
std::string link_tag(std::string_view channel_id, Mix mix)
{
    return std::string(channel_id) + ":" + std::string(to_string(mix));
}

// The node name of a channel's loopback: "wavemix_<id>_<mix>_link".
std::string link_node_name(std::string_view channel_id, Mix mix)
{
    return base_sink(channel_id) + "_" + std::string(to_string(mix)) + "_link";
}

// The media.name of a channel's loopback: "WaveMix <id>:<mix>" (frozen, see the header).
std::string link_media_name(std::string_view channel_id, Mix mix)
{
    return "WaveMix " + link_tag(channel_id, mix);
}

}  // namespace wavemix::core
