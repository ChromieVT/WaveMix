// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Small helpers shared by the tests: loading golden files and temporary folders.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace wavemix::test {

// Loads tests/golden/<relative>. Meson passes the folder in WAVEMIX_GOLDEN_DIR.
inline nlohmann::json load_golden(const std::string& relative)
{
    const char* dir = std::getenv("WAVEMIX_GOLDEN_DIR");
    if (!dir)
        throw std::runtime_error("WAVEMIX_GOLDEN_DIR isn't set; run the tests with `meson test`");
    std::ifstream file(std::filesystem::path(dir) / relative);
    if (!file)
        throw std::runtime_error("missing golden file " + relative);
    return nlohmann::json::parse(file);
}

// Compares JSON values whatever their key order: our code keeps insertion order
// (ordered_json), and the golden files may list keys in any order. Converting both to
// the sorted nlohmann::json type makes the comparison about content only.
template <typename A, typename B>
bool same_json(const A& a, const B& b)
{
    return nlohmann::json::parse(a.dump()) == nlohmann::json::parse(b.dump());
}

// A temporary folder that's deleted again when it goes out of scope.
class TempDir {
public:
    // Creates a fresh, empty folder under the system's temporary directory.
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "wavemix-test-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("could not create a temporary folder");
        path_ = pattern;
    }
    // Deletes the folder and everything in it.
    ~TempDir() { std::filesystem::remove_all(path_); }
    // Not copyable (copy constructor and assignment deleted): only one owner may delete the folder.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // The folder's path.
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace wavemix::test
