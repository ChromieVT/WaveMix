// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "core/app_icons.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace wavemix::core {

namespace {

// Strips spaces, tabs and carriage returns from both ends.
std::string trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

// Lowercases ASCII letters.
std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// How names are compared: "CubismViewer5.exe", "cubismviewer5.exe" and
// "Cubism Viewer 5" all become "cubismviewer5".
std::string loose(std::string_view text)
{
    std::string name = lower(text);
    if (name.size() > 4 && name.ends_with(".exe"))
        name.resize(name.size() - 4);
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c))
            out += static_cast<char>(c);
    }
    return out;
}

// The program an Exec= line runs, without its folder: `env "A=b" wine "x.lnk"` gives
// "wine", `/usr/bin/spotify --uri=%u` gives "spotify".
std::string exec_program(std::string_view exec)
{
    std::vector<std::string> words;
    std::string word;
    bool quoted = false;
    for (char c : exec) {
        if (c == '"') {
            quoted = !quoted;
        } else if ((c == ' ' || c == '\t') && !quoted) {
            if (!word.empty())
                words.push_back(std::exchange(word, {}));
        } else {
            word += c;
        }
    }
    if (!word.empty())
        words.push_back(word);

    for (std::size_t i = 0; i < words.size(); ++i) {
        // Skip `env` and the VAR=value settings that follow it.
        if (words[i] == "env" || (i > 0 && words[0] == "env" && words[i].find('=') != std::string::npos))
            continue;
        const auto slash = words[i].find_last_of("/\\");
        return slash == std::string::npos ? words[i] : words[i].substr(slash + 1);
    }
    return {};
}

// A property's value, or "" if it isn't set.
std::string prop(const Props& props, std::string_view key)
{
    const auto it = props.find(key);
    return it == props.end() ? std::string() : it->second;
}

// Programs that run other programs: their name says nothing about which app it is.
bool is_launcher(std::string_view binary)
{
    static const std::set<std::string, std::less<>> kLaunchers{
        "wine", "wine64", "wine-preloader", "wine64-preloader", "wineserver", "python", "python3",
        "java", "sh", "bash", "electron", "node",
    };
    return kLaunchers.contains(lower(binary));
}

}  // namespace

// Reads the [Desktop Entry] group's Type, Name, Icon, StartupWMClass and Exec keys.
std::optional<DesktopEntry> parse_desktop_entry(std::string id, std::string_view text)
{
    DesktopEntry entry;
    entry.id = std::move(id);
    std::string type = "Application";
    bool in_main_group = false;

    std::istringstream lines{std::string(text)};
    for (std::string line; std::getline(lines, line);) {
        const std::string clean = trim(line);
        if (clean.empty() || clean.front() == '#')
            continue;
        if (clean.front() == '[') {
            in_main_group = clean == "[Desktop Entry]";
            continue;
        }
        const auto eq = clean.find('=');
        if (!in_main_group || eq == std::string::npos)
            continue;
        // Exact keys only: "Name[de]" and friends are translations.
        const std::string key = trim(std::string_view(clean).substr(0, eq));
        const std::string value = trim(std::string_view(clean).substr(eq + 1));
        if (key == "Type")
            type = value;
        else if (key == "Name")
            entry.name = value;
        else if (key == "Icon")
            entry.icon = value;
        else if (key == "StartupWMClass")
            entry.wm_class = value;
        else if (key == "Exec")
            entry.exec_program = exec_program(value);
    }
    if (type != "Application" || entry.icon.empty())
        return std::nullopt;
    return entry;
}

// Walks each folder for .desktop files, giving each its spec-defined ID. The first file
// with an ID wins, even if it's then dropped for having no icon that can be read.
std::vector<DesktopEntry> load_desktop_entries(const std::vector<fs::path>& dirs)
{
    std::vector<DesktopEntry> entries;
    std::set<std::string> seen;
    for (const auto& dir : dirs) {
        std::error_code error;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            const fs::path& file = it->path();
            if (file.extension() != ".desktop" || !it->is_regular_file(error))
                continue;
            std::string id = fs::relative(file, dir, error).generic_string();
            std::replace(id.begin(), id.end(), '/', '-');
            id.resize(id.size() - std::string_view(".desktop").size());
            if (error || !seen.insert(id).second)
                continue;  // the same ID in an earlier folder wins

            std::ifstream in(file);
            std::stringstream text;
            text << in.rdbuf();
            auto entry = parse_desktop_entry(id, text.str());
            if (!entry)
                continue;
            if (entry->icon.front() == '/') {
                // An icon file. Inside a Flatpak the host's /usr is at /run/host/usr; an
                // icon somewhere the sandbox can't read is no use, so the entry is too.
                if (!fs::exists(entry->icon, error)) {
                    const std::string host = "/run/host" + entry->icon;
                    if (!fs::exists(host, error))
                        continue;
                    entry->icon = host;
                }
            }
            entries.push_back(std::move(*entry));
        }
    }
    return entries;
}

// Collects the "applications" folders under every XDG data folder plus Flatpak's and
// the host's, without duplicates.
std::vector<fs::path> application_dirs()
{
    std::vector<fs::path> dirs;
    const auto add = [&](const fs::path& share) {
        if (!share.empty() && std::find(dirs.begin(), dirs.end(), share / "applications") == dirs.end())
            dirs.push_back(share / "applications");
    };
    const char* home = std::getenv("HOME");
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        add(data_home);
    if (home && *home) {
        // Inside a Flatpak, XDG_DATA_HOME is the app's own folder, not the user's.
        add(fs::path(home) / ".local/share");
    }

    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    std::string list = data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share";
    std::istringstream parts(list);
    for (std::string part; std::getline(parts, part, ':');)
        add(part);

    if (home && *home)
        add(fs::path(home) / ".local/share/flatpak/exports/share");
    add("/var/lib/flatpak/exports/share");
    add("/run/host/usr/local/share");
    add("/run/host/usr/share");
    return dirs;
}

// Matches the stream to a menu entry by the five rules described in the header.
std::optional<std::string> icon_for(const Props& props, const std::vector<DesktopEntry>& entries)
{
    // 1. The app says. PulseAudio apps spell it icon_name, native PipeWire ones icon-name.
    for (const char* key : {"application.icon_name", "application.icon-name"}) {
        if (auto icon = prop(props, key); !icon.empty())
            return icon;
    }

    // 2. A Flatpak app: its app ID is its menu entry's ID.
    for (const char* key : {"pipewire.access.portal.app_id", "application.id"}) {
        const std::string app_id = lower(prop(props, key));
        if (app_id.empty())
            continue;
        for (const auto& entry : entries) {
            if (lower(entry.id) == app_id)
                return entry.icon;
        }
    }

    // 3. Its program or name, against each part of an entry, most specific first.
    std::vector<std::string> wanted;
    if (auto binary = prop(props, "application.process.binary"); !binary.empty() && !is_launcher(binary))
        wanted.push_back(loose(binary));
    if (auto name = prop(props, "application.name"); !name.empty())
        wanted.push_back(loose(name));
    std::erase_if(wanted, [](const std::string& w) { return w.empty(); });

    const auto last_part = [](const std::string& id) {
        const auto dot = id.rfind('.');
        return dot == std::string::npos ? id : id.substr(dot + 1);
    };
    const std::array<std::function<bool(const DesktopEntry&, const std::string&)>, 4> rules{
        [&](const DesktopEntry& e, const std::string& w) { return loose(e.id) == w || loose(last_part(e.id)) == w; },
        [](const DesktopEntry& e, const std::string& w) { return !e.wm_class.empty() && loose(e.wm_class) == w; },
        [](const DesktopEntry& e, const std::string& w) {
            return !e.exec_program.empty() && !is_launcher(e.exec_program) && loose(e.exec_program) == w;
        },
        [](const DesktopEntry& e, const std::string& w) { return !e.name.empty() && loose(e.name) == w; },
    };
    for (const auto& rule : rules) {
        for (const auto& w : wanted) {
            for (const auto& entry : entries) {
                if (rule(entry, w))
                    return entry.icon;
            }
        }
    }

    // 4. Last, a name that starts or ends with an entry's name: games often call their
    // audio by their full title ("Tom Clancy's The Division 2") while the launcher's
    // shortcut has the short one ("The Division 2"). Short names would match too much,
    // so they don't count, and the longest match wins.
    constexpr std::size_t kMinPartial = 6;
    const DesktopEntry* best = nullptr;
    std::size_t best_length = 0;
    for (const auto& w : wanted) {
        for (const auto& entry : entries) {
            const std::string name = loose(entry.name);
            if (name.size() >= kMinPartial && name.size() > best_length && name.size() < w.size() &&
                (w.starts_with(name) || w.ends_with(name))) {
                best = &entry;
                best_length = name.size();
            }
        }
    }
    if (best)
        return best->icon;

    // 5. Nothing matched, but it's a Windows program (Wine, or Proton for Steam games that
    // have no desktop shortcut): at least show that, with Wine's icon.
    const std::string binary = lower(prop(props, "application.process.binary"));
    const bool windows = binary.starts_with("wine") || binary.ends_with(".exe") ||
                         lower(prop(props, "application.name")).ends_with(".exe");
    if (windows)
        return std::string("wine");
    return std::nullopt;
}

}  // namespace wavemix::core
