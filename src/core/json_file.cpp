// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/json_file.hpp"

#include "core/log.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace wavemix::core {

namespace {

// Throws a std::system_error for the current errno, with `what` as the message.
[[noreturn]] void throw_errno(const std::string& what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

// Writes all of `text` to an open file descriptor, retrying short writes.
void write_all(int fd, std::string_view text, const fs::path& path)
{
    while (!text.empty()) {
        const ssize_t n = ::write(fd, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw_errno("writing " + path.string());
        }
        text.remove_prefix(static_cast<size_t>(n));
    }
}

}  // namespace

// Writes to a temporary file, fsyncs it and renames it over `path` (see the header).
void atomic_write_text(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp." + std::to_string(::getpid());

    // Plain POSIX calls rather than std::ofstream, because we need fsync(): without it
    // the rename can reach the disk before the data does, and a power cut leaves an
    // empty file behind.
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        throw_errno("creating " + tmp.string());
    try {
        write_all(fd, text, tmp);
        if (::fsync(fd) != 0)
            throw_errno("flushing " + tmp.string());
    } catch (...) {
        ::close(fd);
        ::unlink(tmp.c_str());
        throw;
    }
    ::close(fd);

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        ::unlink(tmp.c_str());
        throw std::system_error(ec, "replacing " + path.string());
    }
}

// Writes `data` as 2-space-indented JSON plus a newline, atomically.
void atomic_write_json(const fs::path& path, const json& data)
{
    atomic_write_text(path, data.dump(2) + "\n");
}

// Loads `path` and merges its top-level keys onto `defaults` (see the header).
json load_merged(const fs::path& path, const json& defaults)
{
    json data = defaults;

    std::ifstream file(path);
    if (!file) {
        if (fs::exists(path))
            log::warning("config", "ignoring unreadable " + path.string());
        return data;  // a missing file is normal: first run
    }
    std::stringstream text;
    text << file.rdbuf();

    // Parse without exceptions: invalid JSON comes back as a "discarded" value.
    const json loaded = json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    if (loaded.is_discarded()) {
        log::warning("config", "ignoring unreadable " + path.string() + ": not valid JSON");
        return data;
    }
    if (!loaded.is_object())
        return data;

    for (const auto& [key, value] : loaded.items()) {
        if (value.is_object() && data.contains(key) && data[key].is_object())
            data[key].update(value);  // one level deep: keeps defaults the file doesn't set
        else
            data[key] = value;
    }
    return data;
}

}  // namespace wavemix::core
