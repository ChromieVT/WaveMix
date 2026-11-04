// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Channel IDs: the short, stable name a channel is known by (sink names, link tags,
// config.json). The user sees the label; the ID never changes after it's made, even
// when the channel is renamed.

#pragma once

#include <set>
#include <string>
#include <string_view>

namespace wavemix::core {

// True if `id` matches ^[a-z0-9][a-z0-9_]{0,31}$.
bool valid_id(std::string_view id);

// Makes a new ID from a label: "Voice Chat" -> "voice_chat".
//
// Lowercase, turn every run of characters outside a-z/0-9 into one "_", trim "_" from
// both ends, keep 24 characters, fall back to "channel" if nothing is left, then add
// _2, _3, ... until it isn't in `existing`.
std::string make_id(std::string_view label, const std::set<std::string, std::less<>>& existing);

}  // namespace wavemix::core
