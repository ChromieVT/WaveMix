// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Reading and writing WaveMix's files safely.

#pragma once

#include <filesystem>
#include <string_view>

#include <nlohmann/json.hpp>

namespace wavemix::core {

// ordered_json keeps keys in the order they were added. JSON objects have no order, so
// this is only for people: config.json keeps "version" at the top and stays readable.
using json = nlohmann::ordered_json;

// Writes `text` to `path` so that the file is always either the old version or the
// new one, never half of each: the text goes into "<path>.tmp.<pid>" first, is flushed
// to disk, and only then renamed over the real file. A crash or power cut mid-write
// therefore can't corrupt the user's config.
//
// Creates the parent folder if needed. Throws std::system_error on failure.
void atomic_write_text(const std::filesystem::path& path, std::string_view text);

// Same, for JSON: 2-space indentation and a final newline.
void atomic_write_json(const std::filesystem::path& path, const json& data);

// Loads a JSON object from `path` on top of `defaults`.
//
// - Missing file: returns `defaults`.
// - Unreadable or invalid JSON: returns `defaults` and prints a warning. A broken file
//   must never stop the daemon from starting.
// - Not a JSON object: returns `defaults`.
// - Otherwise each top-level key from the file replaces the default, except that two
//   objects are merged one level deep. So a file with only settings.personal_output
//   still gets the default value for every other setting.
json load_merged(const std::filesystem::path& path, const json& defaults);

}  // namespace wavemix::core
