// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Fixed names WaveMix uses on D-Bus and in PipeWire.
//
// Almost everything here is visible to users or to other programs: OBS scenes point at
// the Stream Mix sink by name, WirePlumber remembers volumes by a loopback's media.name,
// and scripts call the D-Bus interface. Changing any of these breaks existing setups,
// so treat them as frozen. See docs/contract/files.md#names.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace wavemix::core {

// ---- D-Bus -------------------------------------------------------------------------

inline constexpr std::string_view kAppId = "io.github.ChromieVT.WaveMix";
inline constexpr std::string_view kBusName = "io.github.ChromieVT.WaveMix.Daemon";
inline constexpr std::string_view kObjectPath = "/io/github/ChromieVT/WaveMix/Daemon";
inline constexpr std::string_view kInterface = "io.github.ChromieVT.WaveMix.Daemon1";

// Where WaveMix lives: the About dialog and the bug report point here.
inline constexpr std::string_view kWebsite = "https://github.com/ChromieVT/WaveMix-Dev";

// ---- Channels ----------------------------------------------------------------------

// Every sink WaveMix creates starts with this, which is also how we tell our own
// sinks apart from hardware.
inline constexpr std::string_view kPrefix = "wavemix_";

// The sink everything "on stream" is mixed into. OBS captures its monitor.
inline constexpr std::string_view kMixSink = "wavemix_stream_mix";
inline constexpr std::string_view kMixLabel = "WaveMix Stream Mix";

// The same as the Stream Mix minus the channels left out of the VOD (Music, typically),
// for OBS's VOD track. OBS captures its monitor as a second desktop audio source.
inline constexpr std::string_view kVodSink = "wavemix_vod_mix";
inline constexpr std::string_view kVodLabel = "WaveMix VOD Mix";

// What you hear: every channel at its Personal level, and the mic's Monitor. One
// loopback plays it to your output device, so its master (this sink's own volume and
// mute) only affects what goes through WaveMix: apps that aren't routed keep playing
// at the device's own volume.
inline constexpr std::string_view kPersonalSink = "wavemix_personal_mix";
inline constexpr std::string_view kPersonalLabel = "WaveMix Personal Mix";

// The wavemix.link tag of the loopback from the Personal Mix to your output device.
inline constexpr std::string_view kPersonalOutTag = "personal:out";

// 5 presets + 5 custom. The plumbing was tested up to 16, so this leaves headroom
// under the usual 1024 open-file limit of the PipeWire processes.
inline constexpr int kMaxChannels = 10;

inline constexpr std::string_view kDefaultIcon = "audio-volume-high-symbolic";

struct Preset {
    std::string_view label;
    std::string_view icon;
};

inline constexpr std::array<Preset, 5> kPresets{{
    {"System", "computer-symbolic"},
    {"Voice", "audio-headset-symbolic"},
    {"Game", "input-gaming-symbolic"},
    {"Music", "audio-x-generic-symbolic"},
    {"Browser", "web-browser-symbolic"},
}};

// ---- Mixes and links ---------------------------------------------------------------
//
// Each channel is one sink plus up to three loopbacks reading from its monitor:
//
//   wavemix_<id> ─┬─ loopback "<id>:personal" ─► wavemix_personal_mix (Personal slider)
//                 ├─ loopback "<id>:stream"   ─► wavemix_stream_mix   (Stream slider)
//                 └─ loopback "<id>:vod"      ─► wavemix_vod_mix      (follows Stream)
//
//   wavemix_personal_mix ─── loopback "personal:out" ─► your output device
//
// The VOD loopback has no slider of its own: it always has the Stream loopback's level
// and mute. It only exists while the channel is "in the VOD".

enum class Mix { Personal, Stream, Vod };

// The two sliders each channel has, in the order they're created: Personal first.
inline constexpr std::array<Mix, 2> kMixes{Mix::Personal, Mix::Stream};

// Every loopback a channel can have, in the order they're created.
inline constexpr std::array<Mix, 3> kLinks{Mix::Personal, Mix::Stream, Mix::Vod};

// "personal" / "stream" / "vod", as used on D-Bus and in link tags.
std::string_view to_string(Mix mix);
// Parses "personal" / "stream" / "vod" back into a Mix; nullopt for anything else.
std::optional<Mix> parse_mix(std::string_view text);

// The sink apps play into: wavemix_<id>
std::string base_sink(std::string_view channel_id);

// "<id>:personal". Stored as the wavemix.link property on the loopback's playback
// stream, and used as the key for saved levels in state.json.
std::string link_tag(std::string_view channel_id, Mix mix);

// wavemix_<id>_personal_link
std::string link_node_name(std::string_view channel_id, Mix mix);

// "WaveMix <id>:personal". WirePlumber remembers stream volumes by media.name, so
// this must never change, or everyone's levels reset at their next login.
std::string link_media_name(std::string_view channel_id, Mix mix);

}  // namespace wavemix::core
