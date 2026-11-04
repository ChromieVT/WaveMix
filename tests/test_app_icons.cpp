// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Finding apps' icons from their menu entries (core/app_icons.hpp). The entries below
// are shaped like real ones: a system app, a Flatpak, a Wine shortcut, a Steam
// shortcut and a game launcher's shortcut with an icon file.

#include <gtest/gtest.h>

#include <fstream>

#include "core/app_icons.hpp"
#include "golden.hpp"

using namespace wavemix;

namespace {

// Parses a test .desktop file, throwing if it isn't accepted.
core::DesktopEntry entry(std::string id, std::string_view text)
{
    auto parsed = core::parse_desktop_entry(std::move(id), text);
    if (!parsed)
        throw std::runtime_error("entry didn't parse");
    return *parsed;
}

// A fixed set of menu entries, like a real desktop's (browsers, Flatpak, Wine, Steam, ...).
const std::vector<core::DesktopEntry>& entries()
{
    static const std::vector<core::DesktopEntry> all{
        entry("firefox", "[Desktop Entry]\nType=Application\nName=Firefox\nExec=/usr/lib/firefox/firefox %u\n"
                         "Icon=firefox\n"),
        entry("spotify", "[Desktop Entry]\nType=Application\nName=Spotify\nExec=spotify --uri=%u\n"
                         "Icon=spotify-client\nStartupWMClass=spotify\n"),
        entry("com.discordapp.Discord", "[Desktop Entry]\nType=Application\nName=Discord\n"
                                        "Exec=/usr/bin/flatpak run com.discordapp.Discord\n"
                                        "Icon=com.discordapp.Discord\n"),
        entry("wine-Programs-Live2D-Viewer", "[Desktop Entry]\nType=Application\nName=Live2D Cubism Viewer 5.0\n"
                                             "Exec=env \"WINEPREFIX=/home/me/.wine\" wine \"C:\\\\x.lnk\"\n"
                                             "Icon=649D_CubismViewer5.0\nStartupWMClass=cubismviewer5.exe\n"),
        entry("Grounded 2", "[Desktop Entry]\nType=Application\nName=Grounded 2\n"
                            "Exec=steam steam://rungameid/2661300\nIcon=steam_icon_2661300\n"),
        entry("steam", "[Desktop Entry]\nType=Application\nName=Steam\nExec=/usr/bin/steam %U\nIcon=steam\n"),
        entry("the-division-2", "[Desktop Entry]\nType=Application\nName=The Division 2\n"
                                "Exec=/usr/bin/faugus-launcher --game the-division-2\n"
                                "Icon=/home/me/.local/share/faugus-launcher/icons/the-division-2.png\n"),
        entry("division", "[Desktop Entry]\nType=Application\nName=Division\nExec=division\nIcon=wrong\n"),
        entry("vlc", "[Desktop Entry]\nType=Application\nName=VLC media player\nExec=/usr/bin/vlc %U\nIcon=vlc\n"),
    };
    return all;
}

// The icon icon_for() picks for these properties from entries(), or "(none)".
std::string icon(const core::Props& props)
{
    return core::icon_for(props, entries()).value_or("(none)");
}

}  // namespace

// Only the [Desktop Entry] group's untranslated keys are read.
TEST(AppIcons, ParsesTheMainGroupOnly)
{
    const auto e = core::parse_desktop_entry("x", "# comment\n[Desktop Entry]\nName=Real\nName[de]=Echt\n"
                                                  "Icon=real\nExec=env A=1 B=2 /opt/real/bin/real-app --x\n"
                                                  "[Desktop Action new]\nName=Other\nIcon=other\n");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->name, "Real");
    EXPECT_EQ(e->icon, "real");
    EXPECT_EQ(e->exec_program, "real-app");
}

// Entries that aren't applications, or have no icon, are skipped.
TEST(AppIcons, SkipsEntriesThatArentAppsOrHaveNoIcon)
{
    EXPECT_FALSE(core::parse_desktop_entry("x", "[Desktop Entry]\nType=Link\nName=X\nIcon=x\n"));
    EXPECT_FALSE(core::parse_desktop_entry("x", "[Desktop Entry]\nType=Application\nName=X\n"));
}

// An icon name set by the app on its stream comes first.
TEST(AppIcons, TheAppsOwnIconNameWins)
{
    EXPECT_EQ(icon({{"application.icon_name", "my-icon"}, {"application.name", "Firefox"}}), "my-icon");
}

// Native PipeWire apps' "application.icon-name" works too.
TEST(AppIcons, PipeWireSpellsItIconName)
{
    EXPECT_EQ(icon({{"application.icon-name", "vlc"}, {"application.name", "VLC media player (LibVLC 3.0.24)"}}),
              "vlc");
}

// A stream named with a game's full title matches the shortcut with the shorter name.
TEST(AppIcons, FullTitlesMatchTheShortcutsShorterName)
{
    // The longest matching name wins: "The Division 2", not "Division".
    EXPECT_EQ(icon({{"application.name", "Tom Clancy's The Division 2"}}),
              "/home/me/.local/share/faugus-launcher/icons/the-division-2.png");
    EXPECT_EQ(icon({{"application.name", "VLC media player (LibVLC 3.0.24)"}}), "vlc");
    // Too short to trust: "Steam" mustn't claim every name starting with it.
    EXPECT_EQ(icon({{"application.name", "Steamworks Common Redistributables"}}), "(none)");
}

// A Flatpak app is matched by its app ID.
TEST(AppIcons, FlatpakAppsByTheirAppId)
{
    EXPECT_EQ(icon({{"pipewire.access.portal.app_id", "com.discordapp.Discord"}, {"application.name", "x"}}),
              "com.discordapp.Discord");
}

// Apps are matched by program or name against the entry's ID, Exec and Name.
TEST(AppIcons, ByProgramOrName)
{
    EXPECT_EQ(icon({{"application.name", "Firefox"}, {"application.process.binary", "firefox"}}), "firefox");
    EXPECT_EQ(icon({{"application.name", "Spotify"}}), "spotify-client");          // entry ID
    EXPECT_EQ(icon({{"application.name", "Discord"}}), "com.discordapp.Discord");  // last part of the ID
    EXPECT_EQ(icon({{"application.process.binary", "steam"}}), "steam");
}

// Wine programs are matched by StartupWMClass, not by Wine's own binary.
TEST(AppIcons, WineProgramsByWindowClass)
{
    // Wine's own binary says nothing; the program name matches StartupWMClass.
    EXPECT_EQ(icon({{"application.name", "CubismViewer5.exe"}, {"application.process.binary", "wine64-preloader"}}),
              "649D_CubismViewer5.0");
}

// Steam games are matched by their shortcut's Name.
TEST(AppIcons, SteamGamesByName)
{
    EXPECT_EQ(icon({{"application.name", "Grounded2.exe"}}), "steam_icon_2661300");
}

// A stream nothing matches gets no icon.
TEST(AppIcons, NoMatch)
{
    EXPECT_EQ(icon({{"application.name", "speech-dispatcher-dummy"}, {"application.process.binary", "sd_dummy"}}),
              "(none)");
}

// Windows programs without an entry get Wine's icon, unless an entry matches.
TEST(AppIcons, WindowsProgramsWithoutAnEntryGetWinesIcon)
{
    // Overwatch through Steam's Proton, with no desktop shortcut for it.
    EXPECT_EQ(icon({{"application.name", "Overwatch"}, {"application.process.binary", "wine-preloader"}}), "wine");
    EXPECT_EQ(icon({{"application.name", "Setup.exe"}}), "wine");
    // An entry still wins over the fallback.
    EXPECT_EQ(icon({{"application.name", "CubismViewer5.exe"}, {"application.process.binary", "wine64-preloader"}}),
              "649D_CubismViewer5.0");
}

// load_desktop_entries() reads subfolders, and an earlier folder wins on the same ID.
TEST(AppIcons, LoadsFoldersUserFirstWithSubfolders)
{
    test::TempDir user, system;
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << text;
    };
    write(user.path() / "spotify.desktop", "[Desktop Entry]\nType=Application\nName=Spotify\nIcon=mine\n");
    write(user.path() / "wine/Programs/Game.desktop", "[Desktop Entry]\nType=Application\nName=Game\nIcon=game\n");
    write(user.path() / "gone.desktop",
          "[Desktop Entry]\nType=Application\nName=Gone\nIcon=/nowhere/at/all/gone.png\n");
    write(system.path() / "spotify.desktop", "[Desktop Entry]\nType=Application\nName=Spotify\nIcon=system\n");

    const auto loaded = core::load_desktop_entries({user.path(), system.path(), "/no/such/folder"});
    ASSERT_EQ(loaded.size(), 2u) << "one Spotify (the user's), the Wine game; the unreadable icon file is skipped";
    std::map<std::string, std::string> by_id;
    for (const auto& e : loaded)
        by_id[e.id] = e.icon;
    EXPECT_EQ(by_id["spotify"], "mine");
    EXPECT_EQ(by_id["wine-Programs-Game"], "game");
}
