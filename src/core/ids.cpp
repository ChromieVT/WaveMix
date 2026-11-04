// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/ids.hpp"

namespace wavemix::core {

namespace {

// True for the characters an ID may start with: a-z and 0-9.
bool is_lower_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Lowercases ASCII A-Z only; every other byte is returned as is.
char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

// Checks `id` against ^[a-z0-9][a-z0-9_]{0,31}$.
bool valid_id(std::string_view id)
{
    if (id.empty() || id.size() > 32 || !is_lower_alnum(id.front()))
        return false;
    for (char c : id.substr(1)) {
        if (!is_lower_alnum(c) && c != '_')
            return false;
    }
    return true;
}

// Builds a unique ID from a label (see the header for the exact rules).
std::string make_id(std::string_view label, const std::set<std::string, std::less<>>& existing)
{
    // Only ASCII is lowercased. Every byte of a UTF-8 character is >= 0x80, so
    // non-ASCII letters simply become "_" and are trimmed or collapsed below
    // ("ÄÖÜ" -> "channel"). IDs end up in sink names, which are safest kept ASCII.
    std::string base;
    for (char c : label) {
        c = ascii_lower(c);
        if (is_lower_alnum(c))
            base += c;
        else if (base.empty() || base.back() != '_')
            base += '_';  // a whole run of other characters becomes a single "_"
    }

    // Trim "_" from both ends first, then cut to 24 characters. (A cut can leave a
    // trailing "_"; that's still a valid ID.)
    const auto first = base.find_first_not_of('_');
    if (first == std::string::npos) {
        base.clear();
    } else {
        base = base.substr(first, base.find_last_not_of('_') - first + 1);
    }
    base = base.substr(0, 24);
    if (base.empty())
        base = "channel";

    std::string id = base;
    for (int n = 2; existing.contains(id); ++n)
        id = base + "_" + std::to_string(n);
    return id;
}

}  // namespace wavemix::core
