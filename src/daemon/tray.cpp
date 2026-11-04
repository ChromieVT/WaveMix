// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "daemon/tray.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "build_config.hpp"
#include "core/log.hpp"
#include "core/names.hpp"
#include "core/paths.hpp"

namespace wavemix::daemon {

namespace {

constexpr const char* kWatcher = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";

constexpr const char* kItemXml = R"xml(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>
)xml";

constexpr const char* kMenuXml = R"xml(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg type="i" name="parentId" direction="in"/>
      <arg type="i" name="recursionDepth" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="u" name="revision" direction="out"/>
      <arg type="(ia{sv}av)" name="layout" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="a(ia{sv})" name="properties" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="name" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="eventId" direction="in"/>
      <arg type="v" name="data" direction="in"/>
      <arg type="u" name="timestamp" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" name="events" direction="in"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" name="id" direction="in"/>
      <arg type="b" name="needUpdate" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="ai" name="updatesNeeded" direction="out"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg type="a(ia{sv})" name="updatedProps"/>
      <arg type="a(ias)" name="removedProps"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg type="u" name="revision"/>
      <arg type="i" name="parent"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg type="i" name="id"/>
      <arg type="u" name="timestamp"/>
    </signal>
  </interface>
</node>
)xml";

// ---- The right-click menu ------------------------------------------------------------
//
// dbusmenu describes a menu as a tree of numbered items with properties. Ours is fixed:
// item 0 is the (invisible) root, and its children are the four entries below.

enum MenuItem { kRoot = 0, kOpen = 1, kSeparator = 2, kRestart = 3, kQuit = 4 };
constexpr int kMenuEntries[] = {kOpen, kSeparator, kRestart, kQuit};

struct MenuProp {
    int item;
    const char* name;
    const char* value;
};
constexpr MenuProp kMenuProps[] = {
    {kRoot, "children-display", "submenu"},
    {kOpen, "label", "Open WaveMix"},
    {kSeparator, "type", "separator"},
    {kRestart, "label", "Restart WaveMix"},
    {kQuit, "label", "Quit WaveMix"},
};

// True for the menu's item IDs (the root and its four entries).
bool known_item(int item)
{
    return item >= kRoot && item <= kQuit;
}

// An item's properties as a{sv}. With `names` non-empty, only those properties.
GVariant* item_props(int item, const std::vector<std::string>& names)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    for (const auto& prop : kMenuProps) {
        if (prop.item != item)
            continue;
        if (!names.empty() && std::find(names.begin(), names.end(), prop.name) == names.end())
            continue;
        g_variant_builder_add(&builder, "{sv}", prop.name, g_variant_new_string(prop.value));
    }
    return g_variant_builder_end(&builder);
}

// One item and (for the root) its children, as (ia{sv}av).
GVariant* layout(int item, const std::vector<std::string>& names)
{
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (item == kRoot) {
        for (int child : kMenuEntries)
            g_variant_builder_add(&children, "v", layout(child, names));
    }
    return g_variant_new("(i@a{sv}av)", item, item_props(item, names), &children);
}

// Copies a GVariant string array into a vector.
std::vector<std::string> string_array(GVariant* array)
{
    std::vector<std::string> out;
    GVariantIter iter;
    g_variant_iter_init(&iter, array);
    const char* value;
    while (g_variant_iter_next(&iter, "&s", &value))
        out.emplace_back(value);
    return out;
}

// ---- The icon ------------------------------------------------------------------------

constexpr int kPixmapSizes[] = {16, 22, 24, 32, 48, 64};

}  // namespace

// GIO calls these on the main loop.
struct Tray::Callbacks {
    // A method call on the item object.
    static void item_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant*, GDBusMethodInvocation* invocation, gpointer data)
    {
        static_cast<Tray*>(data)->on_item_method(method, invocation);
    }
    // A property read on the item object.
    static GVariant* item_property(GDBusConnection*, const char*, const char*, const char*,
                                   const char* name, GError**, gpointer data)
    {
        return static_cast<Tray*>(data)->item_property(name);
    }
    // A method call on the menu object.
    static void menu_method(GDBusConnection*, const char*, const char*, const char*, const char* method,
                            GVariant* params, GDBusMethodInvocation* invocation, gpointer data)
    {
        static_cast<Tray*>(data)->on_menu_method(method, params, invocation);
    }
    // A property read on the menu object.
    static GVariant* menu_property(GDBusConnection*, const char*, const char*, const char*,
                                   const char* name, GError**, gpointer data)
    {
        return static_cast<Tray*>(data)->menu_property(name);
    }
    // A tray host appeared: registers with it.
    static void watcher_appeared(GDBusConnection*, const char*, const char*, gpointer data)
    {
        static_cast<Tray*>(data)->register_with_watcher();
    }
    // The tray host went away: we'll register again when one comes back.
    static void watcher_vanished(GDBusConnection*, const char*, gpointer data)
    {
        auto* self = static_cast<Tray*>(data);
        if (self->registered_)
            core::log::info("tray", "tray host went away; will re-register when it's back");
        self->registered_ = false;
    }
    // The reply to RegisterStatusNotifierItem: logs whether it worked.
    static void registered(GObject* bus, GAsyncResult* result, gpointer data)
    {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, &error);
        if (reply) {
            g_variant_unref(reply);
            static_cast<Tray*>(data)->registered_ = true;
            core::log::info("tray", "tray icon registered");
        } else {
            core::log::warning("tray", std::string("registration failed: ") + error->message);
            g_error_free(error);
        }
    }
    // Runs a delayed menu action (Restart or Quit) once.
    static gboolean later(gpointer data)
    {
        auto* action = static_cast<std::function<void()>*>(data);
        (*action)();
        return G_SOURCE_REMOVE;
    }

    static inline const GDBusInterfaceVTable kItemVTable = {&item_method, &item_property, nullptr, {}};
    static inline const GDBusInterfaceVTable kMenuVTable = {&menu_method, &menu_property, nullptr, {}};
};

// Loads the icon at each size as ARGB32 pixmaps, publishes the item and menu objects,
// and watches for a tray to register with.
Tray::Tray(GDBusConnection* bus, std::function<void()> on_restart, std::function<void()> on_quit)
    : bus_(bus), on_restart_(std::move(on_restart)), on_quit_(std::move(on_quit))
{
    // The icon three ways, so every tray host can show it:
    //   IconName       the app ID; works when the icon is installed in the host's theme
    //   IconThemePath  our own icon folder, for hosts that look there (native installs)
    //   IconPixmap     the pixels themselves; works even when the host can't see any of
    //                  our files, e.g. from inside a Flatpak sandbox
    const std::string icon_dir = core::icon_theme_dir();
    if (!core::in_flatpak())
        icon_theme_path_ = icon_dir;
    for (int size : kPixmapSizes) {
        if (icon_dir.empty())
            break;
        const std::string path = icon_dir + "/hicolor/" + std::to_string(size) + "x" + std::to_string(size) +
                                 "/apps/" + std::string(core::kAppId) + ".png";
        GdkPixbuf* loaded = gdk_pixbuf_new_from_file(path.c_str(), nullptr);
        if (!loaded)
            continue;
        GdkPixbuf* rgba = gdk_pixbuf_add_alpha(loaded, FALSE, 0, 0, 0);  // a copy, always with alpha
        g_object_unref(loaded);

        Pixmap pixmap;
        pixmap.width = gdk_pixbuf_get_width(rgba);
        pixmap.height = gdk_pixbuf_get_height(rgba);
        const int stride = gdk_pixbuf_get_rowstride(rgba);
        const guchar* pixels = gdk_pixbuf_get_pixels(rgba);
        pixmap.argb.reserve(static_cast<size_t>(pixmap.width * pixmap.height * 4));
        for (int y = 0; y < pixmap.height; ++y) {
            for (int x = 0; x < pixmap.width; ++x) {
                const guchar* p = pixels + y * stride + x * 4;  // R G B A
                pixmap.argb.insert(pixmap.argb.end(), {p[3], p[0], p[1], p[2]});
            }
        }
        g_object_unref(rgba);
        pixmaps_.push_back(std::move(pixmap));
    }

    GDBusNodeInfo* item = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
    GDBusNodeInfo* menu = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    item_id_ = g_dbus_connection_register_object(bus_, kItemPath, item->interfaces[0],
                                                 &Callbacks::kItemVTable, this, nullptr, nullptr);
    menu_id_ = g_dbus_connection_register_object(bus_, kMenuPath, menu->interfaces[0],
                                                 &Callbacks::kMenuVTable, this, nullptr, nullptr);
    g_dbus_node_info_unref(item);
    g_dbus_node_info_unref(menu);

    // Register whenever a tray (watcher) is there: now, or when one starts later.
    watch_id_ = g_bus_watch_name_on_connection(bus_, kWatcher, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               &Callbacks::watcher_appeared, &Callbacks::watcher_vanished,
                                               this, nullptr);
}

// Stops watching for the tray and unpublishes the item and menu objects.
Tray::~Tray()
{
    g_bus_unwatch_name(watch_id_);
    g_dbus_connection_unregister_object(bus_, item_id_);
    g_dbus_connection_unregister_object(bus_, menu_id_);
}

// Sends RegisterStatusNotifierItem (asynchronously) to the watcher.
void Tray::register_with_watcher()
{
    g_dbus_connection_call(bus_, kWatcher, kWatcherPath, kWatcher, "RegisterStatusNotifierItem",
                           g_variant_new("(s)", g_dbus_connection_get_unique_name(bus_)), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &Callbacks::registered, this);
}

// Packs every loaded pixmap into an a(iiay) variant.
GVariant* Tray::pixmaps_variant() const
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    for (const auto& pixmap : pixmaps_) {
        GVariant* bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixmap.argb.data(), pixmap.argb.size(), 1);
        g_variant_builder_add(&builder, "(ii@ay)", pixmap.width, pixmap.height, bytes);
    }
    return g_variant_builder_end(&builder);
}

// Answers the StatusNotifierItem properties: names, status, icon (three ways), tooltip
// and the menu's path.
GVariant* Tray::item_property(const std::string& name) const
{
    const std::string icon(core::kAppId);
    if (name == "Category") return g_variant_new_string("Hardware");
    if (name == "Id") return g_variant_new_string("wavemix");
    if (name == "Title") return g_variant_new_string("WaveMix");
    if (name == "Status") return g_variant_new_string("Active");
    if (name == "WindowId") return g_variant_new_int32(0);
    if (name == "IconName") return g_variant_new_string(icon.c_str());
    if (name == "IconThemePath") return g_variant_new_string(icon_theme_path_.c_str());
    if (name == "IconPixmap") return pixmaps_variant();
    if (name == "OverlayIconName" || name == "AttentionIconName") return g_variant_new_string("");
    if (name == "ToolTip")
        return g_variant_new("(s@a(iiay)ss)", icon.c_str(), pixmaps_variant(), "WaveMix", "Click to open the mixer");
    // Left click = Activate (open the mixer), right click = the menu.
    if (name == "ItemIsMenu") return g_variant_new_boolean(FALSE);
    if (name == "Menu") return g_variant_new_object_path(kMenuPath);
    return nullptr;
}

// Answers the dbusmenu properties: protocol version, text direction and status.
GVariant* Tray::menu_property(const std::string& name) const
{
    if (name == "Version") return g_variant_new_uint32(3);
    if (name == "TextDirection") return g_variant_new_string("ltr");
    if (name == "Status") return g_variant_new_string("normal");
    if (name == "IconThemePath") return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

// Opens the mixer on any click (Activate, SecondaryActivate or ContextMenu).
void Tray::on_item_method(const std::string& method, GDBusMethodInvocation* invocation)
{
    // Any click opens the mixer; the menu comes from the Menu property instead.
    if (method == "Activate" || method == "SecondaryActivate" || method == "ContextMenu")
        open_mixer();
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

// Opens the mixer, or schedules Restart / Quit so the click is answered first.
void Tray::menu_clicked(int item)
{
    if (item == kOpen) {
        open_mixer();
        return;
    }
    // Restart and Quit run a moment later, after the click has been answered.
    std::function<void()>* action = item == kRestart ? &on_restart_ : item == kQuit ? &on_quit_ : nullptr;
    if (action && *action)
        g_timeout_add(100, &Callbacks::later, action);
}

// Implements dbusmenu: the fixed layout and item properties, and "clicked" events
// (single or grouped) that run menu_clicked().
void Tray::on_menu_method(const std::string& method, GVariant* params, GDBusMethodInvocation* invocation)
{
    if (method == "GetLayout") {
        gint32 parent = 0, depth = 0;
        GVariant* names = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        const auto wanted = string_array(names);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u@(ia{sv}av))", 1u, layout(parent, wanted)));
    } else if (method == "GetGroupProperties") {
        GVariant* ids = nullptr;
        GVariant* names = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &names);
        const auto wanted = string_array(names);
        std::vector<int> items;
        GVariantIter iter;
        g_variant_iter_init(&iter, ids);
        gint32 id;
        while (g_variant_iter_next(&iter, "i", &id))
            items.push_back(id);
        if (items.empty())
            items = {kRoot, kOpen, kSeparator, kRestart, kQuit};  // empty = all of them
        g_variant_unref(ids);
        g_variant_unref(names);

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        for (int item : items)
            if (known_item(item))
                g_variant_builder_add(&builder, "(i@a{sv})", item, item_props(item, wanted));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &builder));
    } else if (method == "GetProperty") {
        gint32 item = 0;
        const char* name = nullptr;
        g_variant_get(params, "(i&s)", &item, &name);
        GVariant* props = g_variant_ref_sink(item_props(item, {name}));
        // lookup_value hands us our own reference; the fallback is made one too, so
        // either way there's exactly one reference to drop at the end.
        GVariant* value = g_variant_lookup_value(props, name, nullptr);
        if (!value)
            value = g_variant_ref_sink(g_variant_new_string(""));
        g_variant_unref(props);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        g_variant_unref(value);
    } else if (method == "Event") {
        gint32 item = 0;
        const char* event = nullptr;
        GVariant* data = nullptr;
        guint32 timestamp = 0;
        g_variant_get(params, "(i&s@vu)", &item, &event, &data, &timestamp);
        g_variant_unref(data);
        const bool clicked = std::string(event) == "clicked";
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (clicked)
            menu_clicked(item);
    } else if (method == "EventGroup") {
        GVariantIter* events = nullptr;
        g_variant_get(params, "(a(isvu))", &events);
        gint32 item;
        const char* event;
        GVariant* data;
        guint32 timestamp;
        std::vector<int> clicked;
        while (g_variant_iter_next(events, "(i&s@vu)", &item, &event, &data, &timestamp)) {
            if (std::string(event) == "clicked")
                clicked.push_back(item);
            g_variant_unref(data);
        }
        g_variant_iter_free(events);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        for (int i : clicked)
            menu_clicked(i);
    } else if (method == "AboutToShow") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (method == "AboutToShowGroup") {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                                      g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

// Spawns the mixer window (this binary, or WAVEMIX_WINDOW_COMMAND) in its own session.
void open_mixer()
{
    // Override for testing with another window build.
    const char* override_command = std::getenv("WAVEMIX_WINDOW_COMMAND");
    gchar** argv = nullptr;
    GSpawnFlags flags = static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL);
    if (override_command && *override_command) {
        if (!g_shell_parse_argv(override_command, nullptr, &argv, nullptr)) {
            core::log::error("tray", "can't parse WAVEMIX_WINDOW_COMMAND");
            return;
        }
        flags = static_cast<GSpawnFlags>(flags | G_SPAWN_SEARCH_PATH);
    } else {
        const std::string self = core::self_executable();
        const gchar* window[] = {self.c_str(), nullptr};  // `wavemix` with no arguments
        argv = g_strdupv(const_cast<gchar**>(window));
    }

    // setsid(): the window gets its own session, so it isn't tied to the daemon's
    // lifetime or terminal (closing one mustn't close the other).
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, flags, [](gpointer) { setsid(); }, nullptr, nullptr, &error)) {
        core::log::error("tray", std::string("could not open the mixer: ") + error->message);
        g_error_free(error);
    }
    g_strfreev(argv);
}

}  // namespace wavemix::daemon
