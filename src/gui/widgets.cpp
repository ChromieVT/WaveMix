// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/widgets.hpp"

#include <cmath>

#include "core/names.hpp"
#include "gui/helpers.hpp"
#include "gui/window.hpp"

namespace wavemix::gui {

namespace {

// Sliders go up to 100%. The daemon accepts up to 150% (for scripts and other mixers),
// but a mixer where most of the travel is "louder than the source" is hard to use.
constexpr double kSliderMax = 100.0;

// After the user moves a slider, ignore GetState's value for it for a moment: the
// state can be a step behind, and applying it would make the slider jump back.
constexpr auto kTouchHold = std::chrono::milliseconds(600);

}  // namespace

using core::json;


// =====================================================================================
// FixedWidthBox
// =====================================================================================

// Fixes the box's width with a size request; its height is free.
FixedWidthBox::FixedWidthBox(int width, Gtk::Orientation orientation, int spacing)
    : Gtk::Box(orientation, spacing)
{
    // (A measure_vfunc override wouldn't work here: a GTK 4 box is measured by its
    // layout manager, which never calls it.)
    set_size_request(width, -1);
}

// =====================================================================================
// LevelControl
// =====================================================================================

// Builds a slider, its percentage and mute button for one of a channel's or a mix's
// levels, stacked (vertical) or in a row (horizontal).
LevelControl::LevelControl(MixerWindow& window, std::string channel, std::string mix, const std::string& caption,
                           Gtk::Orientation orientation)
    : Gtk::Box(orientation == Gtk::Orientation::VERTICAL ? Gtk::Orientation::VERTICAL
                                                         : Gtk::Orientation::HORIZONTAL,
               orientation == Gtk::Orientation::VERTICAL ? 6 : 10),
      window_(window),
      channel_(std::move(channel)),
      mix_(std::move(mix)),
      adjustment_(Gtk::Adjustment::create(0, 0, kSliderMax, 1, 5)),
      scale_(adjustment_, orientation),
      percent_("–")
{
    const bool vertical = orientation == Gtk::Orientation::VERTICAL;
    const char* kind = mix_ == "personal" ? "wm-personal" : "wm-stream";

    scale_.add_css_class(kind);
    scale_.set_draw_value(false);
    if (vertical) {
        scale_.set_inverted(true);  // 0% at the bottom, like a mixing desk
        scale_.set_vexpand(true);
        scale_.set_size_request(-1, 200);
    } else {
        scale_.set_hexpand(true);
        scale_.set_size_request(220, -1);
        percent_.set_width_chars(4);  // so the slider doesn't shift as the number changes
    }
    adjustment_->signal_value_changed().connect(sigc::mem_fun(*this, &LevelControl::on_value_changed));

    percent_.add_css_class("wm-level");
    mute_.add_css_class("flat");
    mute_.add_css_class("wm-mute");
    mute_.set_tooltip_text("Mute");
    show_mute_icon(false);
    mute_.signal_toggled().connect(sigc::mem_fun(*this, &LevelControl::on_mute_toggled));

    if (vertical) {
        auto* title = make_label(caption, {"wm-caption", mix_ == "personal" ? "wm-personal-text" : "wm-stream-text"});
        for (Gtk::Widget* widget : {static_cast<Gtk::Widget*>(title), static_cast<Gtk::Widget*>(&scale_),
                                    static_cast<Gtk::Widget*>(&percent_), static_cast<Gtk::Widget*>(&mute_)}) {
            widget->set_halign(Gtk::Align::CENTER);
            append(*widget);
        }
    } else {
        // A row: slider, %, mute (the caption is the row's title, outside this control).
        for (Gtk::Widget* widget : {static_cast<Gtk::Widget*>(&scale_), static_cast<Gtk::Widget*>(&percent_),
                                    static_cast<Gtk::Widget*>(&mute_)}) {
            widget->set_valign(Gtk::Align::CENTER);
            append(*widget);
        }
    }
}

// Builds one of the mic's sliders: Monitor in the Personal colour, Mic in the Stream colour.
LevelControl::LevelControl(MixerWindow& window, MicSlider slider, const std::string& caption)
    : LevelControl(window, "", slider.name == "monitor" ? "personal" : "stream", caption)
{
    mic_slider_ = std::move(slider.name);
}

// Shows the daemon's level, unless the user moved the slider a moment ago.
void LevelControl::update(const json& level)
{
    updating_ = true;
    const bool present = level.is_object() && level.value("present", false);
    set_sensitive(present);
    if (!present) {
        percent_.set_text("–");  // the loopback isn't there (yet)
    } else {
        const double percent = std::round(level.value("volume", 0.0) * 100);
        if (std::chrono::steady_clock::now() - last_touched_ > kTouchHold)
            adjustment_->set_value(percent);
        percent_.set_text(std::to_string(static_cast<int>(adjustment_->get_value())) + "%");
        const bool muted = level.value("muted", false);
        mute_.set_active(muted);
        show_mute_icon(muted);
    }
    updating_ = false;
}

// Shows the muted or unmuted speaker icon on the mute button.
void LevelControl::show_mute_icon(bool muted)
{
    mute_.set_icon_name(muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
}

// The slider moved: updates the % and, if the user moved it, sends the new level.
void LevelControl::on_value_changed()
{
    percent_.set_text(std::to_string(static_cast<int>(std::round(adjustment_->get_value()))) + "%");
    if (updating_)
        return;  // we're showing the daemon's value, not the user's
    last_touched_ = std::chrono::steady_clock::now();
    const auto on_error = [this](const std::string& error) { window_.show_error(error); };
    if (!mic_slider_.empty())
        window_.client().set_mic_volume(mic_slider_, adjustment_->get_value() / 100.0, on_error);
    else
        window_.client().set_volume(channel_, mix_, adjustment_->get_value() / 100.0, on_error);
}

// The mute button toggled: updates its icon and, if the user clicked it, sends the mute.
void LevelControl::on_mute_toggled()
{
    show_mute_icon(mute_.get_active());
    if (updating_)
        return;
    const auto on_error = [this](const std::string& error) { window_.show_error(error); };
    if (!mic_slider_.empty())
        window_.client().set_mic_mute(mic_slider_, mute_.get_active(), on_error);
    else
        window_.client().set_mute(channel_, mix_, mute_.get_active(), on_error);
}

// =====================================================================================
// Apps
// =====================================================================================

// Groups streams by app key, keeping the first-seen order (see the comment below).
json group_by_app(const json& streams)
{
    // Streams of the same app (same key) become one group, in the order they first
    // appear. Moving one moves all of them anyway: routes are remembered per app.
    // Streams without a key can't be told apart from other apps, so they stay alone.
    json groups = json::array();
    std::map<std::string, size_t> where;
    for (const auto& stream : streams) {
        const std::string key = str(stream.value("key", json()));
        if (!key.empty()) {
            if (const auto it = where.find(key); it != where.end()) {
                groups[it->second].push_back(stream);
                continue;
            }
            where[key] = groups.size();
        }
        groups.push_back(json::array({stream}));
    }
    return groups;
}

// =====================================================================================
// ChannelColumn
// =====================================================================================

// Builds a channel's column: title and menu (draggable to reorder), apps strip, meter,
// the Personal and Stream sliders and, with the VOD Mix on, the VOD button.
ChannelColumn::ChannelColumn(MixerWindow& window, const json& channel, int index)
    : FixedWidthBox(163, Gtk::Orientation::VERTICAL, 10),  // 185 px with the column's padding
      window_(window),
      channel_(channel),
      personal_(window, str(channel["id"]), "personal", "Personal"),
      stream_(window, str(channel["id"]), "stream", "Stream")
{
    add_css_class("wm-column");

    // Title and the channel menu.
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* title = shrinkable(make_label(str(channel["label"]), {"wm-title"}, 0.0F));
    title->set_hexpand(true);
    auto* menu = flat_menu_button("open-menu-symbolic", "Channel options");
    menu->set_popover(*make_menu());
    head->append(*title);
    head->append(*menu);

    // Drag the title row onto another column to move this channel there. The daemon saves
    // the new order (MoveChannel), so it's the same after a restart.
    head->set_cursor(Gdk::Cursor::create("grab"));  // GTK's CSS has no cursor property
    title->set_tooltip_text("Drag to move " + str(channel["label"]));
    const std::string id = str(channel["id"]);
    auto drag = Gtk::DragSource::create();
    drag->set_actions(Gdk::DragAction::MOVE);
    drag->signal_prepare().connect(
        [id](double, double) {
            Glib::Value<Glib::ustring> value;
            value.init(value.value_type());
            value.set(id);
            return Gdk::ContentProvider::create(value);
        },
        false);
    drag->signal_drag_begin().connect([this, drag](const Glib::RefPtr<Gdk::Drag>&) {
        drag->set_icon(Gtk::WidgetPaintable::create(*this), 0, 0);  // the column follows the pointer
    });
    head->add_controller(drag);

    auto drop = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
    drop->signal_drop().connect(
        [this, index](const Glib::ValueBase& value, double, double) {
            Glib::Value<Glib::ustring> dropped;
            dropped.init(value.gobj());
            window_.drop_channel(dropped.get(), index);
            return true;
        },
        false);
    add_controller(drop);

    // The two sliders, side by side.
    auto* sliders = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    sliders->set_homogeneous(true);
    sliders->append(personal_);
    sliders->append(stream_);

    // The apps, as a strip of icons between the title and the sliders, like a mixing
    // desk's scribble strip. Clicking it lists them, to move them or add more.
    apps_button_ = Gtk::make_managed<Gtk::MenuButton>();
    apps_button_->add_css_class("wm-apps");
    apps_button_->set_tooltip_text("Apps in " + str(channel["label"]));
    auto* strip = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    apps_strip_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    apps_strip_->set_hexpand(true);
    apps_strip_->set_halign(Gtk::Align::CENTER);
    strip->append(*apps_strip_);
    apps_button_->set_child(*strip);
    apps_popover_ = Gtk::make_managed<Gtk::Popover>();
    // Filled each time it opens, so it always lists what's playing right now.
    apps_popover_->signal_show().connect(sigc::mem_fun(*this, &ChannelColumn::fill_apps_popover));
    apps_button_->set_popover(*apps_popover_);

    // The apps strip, then the channel's meter: what its apps play into it, before its
    // sliders, so it's clear whether sound is getting this far when nothing's heard.
    // Laid out like the Mic column's strip and meter (the window keeps the rows the same
    // height), so the slider labels line up, with or without the mic.
    setup_meter(meter_);
    meter_.set_tooltip_text("What plays into " + str(channel["label"]) + ", before its sliders");
    top_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    top_->append(*apps_button_);
    top_->append(meter_);

    footer_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    footer_->set_halign(Gtk::Align::CENTER);
    append(*head);
    append(*top_);
    append(*sliders);
    append(*footer_);

    // Disabled: no devices, so nothing to move or hear. Only its menu still works, to
    // enable it again.
    if (!channel.value("enabled", true)) {
        add_css_class("wm-disabled");
        title->set_tooltip_text(str(channel["label"]) + " is disabled: enable it in its menu");
        for (Gtk::Widget* part : std::initializer_list<Gtk::Widget*>{top_, sliders, footer_}) {
            part->set_sensitive(false);
            part->set_opacity(0.4);
        }
    }

    // At the bottom of every column, when the VOD Mix is on: whether the whole channel
    // goes to it. Green when it does, red when it's left out; a click switches.
    const json& settings = window_.state().value("settings", json::object());
    if (!settings.value("vod_enabled", false))
        return;
    const bool in_vod = channel.value("in_vod", true);
    auto* vod = Gtk::make_managed<Gtk::Button>("VOD");
    vod->add_css_class("wm-vod");
    vod->add_css_class(in_vod ? "wm-vod-on" : "wm-vod-off");
    vod->set_valign(Gtk::Align::CENTER);
    vod->set_tooltip_text(in_vod ? "In the VOD Mix. Click to leave this channel out of the VOD."
                                 : "Left out of the VOD Mix (still on stream). Click to put it back.");
    vod->signal_clicked().connect([this, id, in_vod] {
        window_.call([&, id, in_vod] { window_.client().set_in_vod(id, !in_vod); });
    });

    footer_->append(*vod);
}

// The channel's menu: Rename and Remove.
Gtk::Popover* ChannelColumn::make_menu()
{
    // Moving a channel isn't in here: columns are dragged by their title row instead.
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* box = popover_box(*popover);
    const std::string id = str(channel_["id"]);
    menu_item(*popover, *box, "Rename…", [this] { window_.rename_channel(channel_); });
    const bool enabled = channel_.value("enabled", true);
    menu_item(*popover, *box, enabled ? "Disable channel" : "Enable channel",
              [this, enabled] { window_.set_channel_enabled(channel_, !enabled); });
    box->append(*Gtk::make_managed<Gtk::Separator>());
    menu_item(*popover, *box, "Remove channel", [this] { window_.remove_channel(channel_); }, true, true);
    return popover;
}

// Rebuilds the apps popover: this channel's apps (ticked) and every other playing app,
// each with a checkbox that moves it here or back out.
void ChannelColumn::fill_apps_popover()
{
    // One list, like Wave Link: ticked apps are in this channel, the rest are every other
    // app that's playing. Ticking moves an app here; unticking takes it off stream. No
    // menus inside: a popover opened from a popover doesn't always close properly.
    auto* outer = popover_box(*apps_popover_);
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_max_content_height(360);
    scroller->set_propagate_natural_height(true);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    box->set_size_request(260, -1);
    scroller->set_child(*box);
    outer->append(*scroller);

    const json& state = window_.state();
    const std::string id = str(channel_["id"]);
    std::map<std::string, std::string> labels;  // channel id -> label, to say where others are
    json others = json::array();
    for (const auto& ch : state.value("channels", json::array())) {
        labels[str(ch["id"])] = str(ch["label"]);
        if (str(ch["id"]) != id)
            for (const auto& app : ch.value("apps", json::array()))
                others.push_back(app);
    }
    for (const auto& app : state.value("unrouted_apps", json::array()))
        others.push_back(app);

    const auto add_app = [&](const json& streams, bool here) {
        const json& app = streams.front();
        auto* check = Gtk::make_managed<Gtk::CheckButton>();
        check->add_css_class("wm-app-check");
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->append(*app_icon(app));
        auto* name = make_label(str(app["name"]), {}, 0.0F);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        name->set_max_width_chars(28);
        row->append(*name);
        const std::string where = str(app["channel"]);
        if (!here && !where.empty())
            row->append(*make_label(labels[where], {"dim"}, 0.0F));  // playing in another channel
        check->set_child(*row);
        std::string tip = str(app["name"]);
        if (const auto media = str(app["media"]); !media.empty() && media != tip)
            tip += "\n" + media;
        if (streams.size() > 1)
            tip += "\n" + std::to_string(streams.size()) + " audio streams";
        check->set_tooltip_text(tip);
        check->set_active(here);
        const auto index = app["index"].get<uint32_t>();
        check->signal_toggled().connect([this, check, index, id] {
            // Ticked: into this channel. Unticked: back to the personal output.
            window_.assign(index, check->get_active() ? id : std::string());
        });
        box->append(*check);
    };

    box->append(*make_label("Apps in " + str(channel_["label"]), {"wm-caption"}, 0.0F));
    const json mine = group_by_app(channel_.value("apps", json::array()));
    if (mine.empty())
        box->append(*make_label("None yet: tick one below.", {"dim"}, 0.0F));
    for (const auto& streams : mine)
        add_app(streams, true);

    box->append(*Gtk::make_managed<Gtk::Separator>());
    box->append(*make_label("Other playing apps", {"wm-caption"}, 0.0F));
    const json rest = group_by_app(others);
    if (rest.empty())
        box->append(*make_label("No other apps are playing right now.\nStart the app, then try again.", {"dim"},
                                0.0F));
    for (const auto& streams : rest)
        add_app(streams, false);
}

// Shows the channel's new levels and its apps' icons (refreshing the popover if open).
void ChannelColumn::update(const json& channel, const json&)
{
    channel_ = channel;
    personal_.update(channel["personal"]);
    stream_.update(channel["stream"]);

    // The strip: the first few apps' icons, then "+N" for the rest; a hint when empty.
    constexpr size_t kShown = 3;
    remove_children(*apps_strip_);
    const json apps = group_by_app(channel.value("apps", json::array()));
    if (apps.empty()) {
        auto* plus = Gtk::make_managed<Gtk::Image>();
        plus->set_from_icon_name("list-add-symbolic");
        apps_strip_->append(*plus);
        apps_strip_->append(*make_label("Add apps", {"wm-caption"}));
    }
    for (size_t i = 0; i < apps.size() && i < kShown; ++i) {
        auto* icon = app_icon(apps[i].front());
        icon->set_pixel_size(24);
        icon->set_tooltip_text(str(apps[i].front()["name"]));
        apps_strip_->append(*icon);
    }
    if (apps.size() > kShown)
        apps_strip_->append(*make_label("+" + std::to_string(apps.size() - kShown), {"wm-more"}));

    // Open while something changed (an app ticked, started or moved elsewhere): show it.
    if (apps_popover_->get_visible())
        fill_apps_popover();
}

// Moves the channel's meter towards the new peak (falling slowly).
void ChannelColumn::show_meter(double peak)
{
    meter_shown_ = meter_fall(meter_shown_, peak);
    meter_.set_value(meter_shown_);
}

// =====================================================================================
// OutputsPanel
// =====================================================================================

namespace {

// The width of the output rows' middles: the device list, and the mixes' names.
constexpr int kOutputsMiddle = 320;

}  // namespace

// Builds the Outputs panel: the Personal mix with its device list and the Stream mix,
// each with its master slider.
OutputsPanel::OutputsPanel(MixerWindow& window)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 6),
      window_(window),
      personal_(window, "", "personal", "", Gtk::Orientation::HORIZONTAL),
      stream_(window, "", "stream", "", Gtk::Orientation::HORIZONTAL)
{
    add_css_class("wm-outputs");
    append(*make_label("Outputs", {"wm-section"}, 0.0F));

    devices_.set_tooltip_text("Where you hear your channels");
    // A fixed width, whatever's picked: a long device name is cut short with "…" in the
    // box (the list shows it whole), so the sliders beside it never move.
    devices_.set_size_request(kOutputsMiddle, -1);
    devices_.set_hexpand(false);
    auto shown = Gtk::SignalListItemFactory::create();
    shown->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
        auto* label = make_label("", {}, 0.0F);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_max_width_chars(1);  // it takes the box's width, not its text's
        label->set_hexpand(true);
        item->set_child(*label);
    });
    shown->signal_bind().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
        auto* label = dynamic_cast<Gtk::Label*>(item->get_child());
        const auto text = std::dynamic_pointer_cast<Gtk::StringObject>(item->get_item());
        if (label && text) {
            label->set_text(text->get_string());
            label->set_tooltip_text(text->get_string());
        }
    });
    devices_.set_factory(shown);
    auto listed = Gtk::SignalListItemFactory::create();
    listed->signal_setup().connect(
        [](const Glib::RefPtr<Gtk::ListItem>& item) { item->set_child(*make_label("", {}, 0.0F)); });
    listed->signal_bind().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
        auto* label = dynamic_cast<Gtk::Label*>(item->get_child());
        const auto text = std::dynamic_pointer_cast<Gtk::StringObject>(item->get_item());
        if (label && text)
            label->set_text(text->get_string());
    });
    devices_.set_list_factory(listed);
    devices_.property_selected().signal_changed().connect([this] {
        const auto i = devices_.get_selected();
        if (updating_ || i >= device_ids_.size())
            return;
        const std::string id = device_ids_[i];
        window_.call([&] { window_.client().set_setting("personal_output", id); });
    });
    add_row("Personal mix", "audio-headphones-symbolic", devices_, personal_);

    // The mixes' own names are what to pick in OBS, so they're shown as they are there.
    auto* stream_name = make_label(std::string(core::kMixLabel), {"wm-device"}, 0.0F);
    stream_name->set_tooltip_text("In OBS: an Audio Output Capture with this device");
    add_row("Stream mix", "media-record-symbolic", *stream_name, stream_);
}

// Adds one output row (icon, title, middle widget, slider) and returns it.
Gtk::Box* OutputsPanel::add_row(const std::string& title, const char* icon, Gtk::Widget& middle,
                                LevelControl& level)
{
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    row->add_css_class("wm-output-row");
    auto* image = Gtk::make_managed<Gtk::Image>();
    image->set_from_icon_name(icon);
    auto* name = make_label(title, {"wm-output-title"}, 0.0F);
    name->set_size_request(110, -1);
    // The middles share one width (the device list's, usually), so the sliders start at
    // the same place.
    middles_->add_widget(middle);
    middle.set_size_request(kOutputsMiddle, -1);
    middle.set_valign(Gtk::Align::CENTER);
    level.set_hexpand(true);
    row->append(*image);
    row->append(*name);
    row->append(middle);
    row->append(level);
    append(*row);
    return row;
}

// Rebuilds the output device list when the outputs or the setting changed.
void OutputsPanel::fill_devices(const json& state)
{
    // Rebuilt only when the list changes: rebuilding while the user has it open would
    // close it under their pointer.
    const json& po = state.value("personal_output", json::object());
    std::vector<std::string> ids{"default"};
    // Whatever the system's default output is, without its name: it follows the default
    // when that changes.
    std::vector<Glib::ustring> names{"Default Output"};
    for (const auto& output : state.value("outputs", json::array())) {
        ids.push_back(str(output["name"]));
        names.push_back(str(output["description"]));
    }
    std::string shown;
    for (const auto& name : names)
        shown += name + "\n";
    const std::string current = str(po.value("setting", json("default")));
    shown += "=" + current;
    if (shown == devices_shown_)
        return;

    updating_ = true;
    devices_shown_ = shown;
    device_ids_ = ids;
    devices_.set_model(Gtk::StringList::create(names));
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == current)
            devices_.set_selected(static_cast<guint>(i));
    updating_ = false;
}

// Shows the device list and the two masters' levels.
void OutputsPanel::update(const json& state)
{
    fill_devices(state);

    // The Personal Mix's own level: muting it leaves apps that aren't routed playing.
    personal_.update(state.value("personal_mix", json()));

    // With the VOD Mix on, this is its level too: the stream and the VOD always match.
    stream_.update(state.value("stream_mix", json()));
}

// =====================================================================================
// AddColumn
// =====================================================================================

// Builds the "Add channel" button with its popover: the unused presets and a custom name.
AddColumn::AddColumn(MixerWindow& window, const std::set<std::string>& used_labels, bool big)
    : FixedWidthBox(big ? 170 : 128, Gtk::Orientation::VERTICAL, 8),  // 150 px with padding when small
      window_(window)
{
    if (!big) {
        add_css_class("wm-column");
        add_css_class("wm-add");
    }
    auto* button = Gtk::make_managed<Gtk::MenuButton>();
    button->set_label("Add channel");
    button->add_css_class("suggested-action");
    button->add_css_class("wm-add-button");
    button->set_halign(Gtk::Align::CENTER);
    button->set_valign(big ? Gtk::Align::START : Gtk::Align::CENTER);
    button->set_vexpand(!big);

    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* box = popover_box(*popover);
    for (const auto& preset : core::kPresets) {
        const std::string label(preset.label);
        if (used_labels.contains(label))
            continue;
        const std::string icon(preset.icon);
        menu_item(*popover, *box, label, [this, label, icon] { window_.add_channel(label, icon); });
    }
    box->append(*Gtk::make_managed<Gtk::Separator>());

    // Or any name the user likes.
    auto* custom = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_placeholder_text("Custom name");
    entry->set_hexpand(true);
    auto* add = Gtk::make_managed<Gtk::Button>();
    add->set_icon_name("list-add-symbolic");
    auto add_custom = [this, entry, popover] {
        std::string text = entry->get_text();
        text.erase(0, text.find_first_not_of(" \t"));
        text.erase(text.find_last_not_of(" \t") + 1);
        if (text.empty())
            return;
        popover->popdown();
        entry->set_text("");
        window_.add_channel(text, "");
    };
    entry->signal_activate().connect(add_custom);
    add->signal_clicked().connect(add_custom);
    custom->append(*entry);
    custom->append(*add);
    box->append(*custom);

    button->set_popover(*popover);
    append(*button);
}

}  // namespace wavemix::gui
