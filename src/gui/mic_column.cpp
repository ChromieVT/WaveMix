// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// The mic column and its filters window. Like the rest of the window, it changes
// nothing itself: every click is a daemon call, and it redraws from GetState's "mic"
// (docs/contract/get-state.md#mic).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "gui/helpers.hpp"
#include "gui/widgets.hpp"
#include "gui/window.hpp"

namespace wavemix::gui {

using core::json;

namespace {

// What the window calls each filter: "Noise removal", "Noise gate", else the capitalised name.
std::string filter_name(const std::string& filter)
{
    if (filter == "noise")
        return "Noise removal";
    if (filter == "gate")
        return "Noise gate";
    if (filter.empty())
        return filter;
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(filter[0])))) + filter.substr(1);
}

// "band_3_gain" -> 3, or 0 for a setting that isn't an equalizer band's.
int band_of(const std::string& key)
{
    int band = 0;
    return std::sscanf(key.c_str(), "band_%d_", &band) == 1 ? band : 0;
}

// A setting's label: "Band 3 gain" for an equalizer band, else the capitalised key.
std::string control_name(const std::string& key)
{
    if (const int band = band_of(key)) {
        const std::string what = key.substr(key.rfind('_') + 1);
        return "Band " + std::to_string(band) + (what == "q" ? " Q" : " " + what);
    }
    std::string name = key;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// A setting's value as shown, in its unit ("1.2 kHz", "50%", "-24.0 dB", "3.0:1").
std::string format_value(double value, const std::string& unit, const std::string& key)
{
    char text[32];
    if (unit == "Hz" && value >= 1000)
        std::snprintf(text, sizeof text, "%.1f kHz", value / 1000);
    else if (unit == "Hz" || unit == "%")
        std::snprintf(text, sizeof text, unit == "%" ? "%.0f%%" : "%.0f Hz", value);
    else if (unit == "dB")
        std::snprintf(text, sizeof text, "%.1f dB", value);
    else if (key == "ratio")
        std::snprintf(text, sizeof text, "%.1f:1", value);
    else
        std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

// A value typed into a setting's box, in its unit: a number, with or without the unit
// shown ("-12", "-12 dB", "50%", "3:1"); "k" multiplies Hz by 1000 ("1.5k", "1.5 kHz").
// Nothing when it doesn't start with a number.
std::optional<double> parse_value(const std::string& text, const std::string& unit)
{
    const char* start = text.c_str();
    char* end = nullptr;
    double value = std::strtod(start, &end);
    if (end == start || !std::isfinite(value))
        return std::nullopt;
    while (*end == ' ')
        ++end;
    if (unit == "Hz" && (*end == 'k' || *end == 'K'))
        value *= 1000;
    return value;
}

// Why a filter in the chain isn't running, as the window says it; "" when it is, or
// when the reason is shown elsewhere (the power button, or the column's own status).
std::string why_not(const json& step, const json& mic)
{
    if (str(step.value("why_not", json())) == "missing") {
        for (const auto& filter : mic.value("filters", json::array()))
            if (filter["filter"] == step["filter"])
                return "Not installed: install " + str(filter["package"]);
    }
    return "";
}

// The filters window's list pane, in pixels.
constexpr int kFilterListWidth = 340;

// The filters window is rebuilt only when this changes (not for a setting moved, so
// a slider isn't replaced under the pointer while it's dragged).
std::string chain_shape(const json& mic)
{
    json shape = json::array();
    for (const auto& step : mic.value("chain", json::array()))
        shape.push_back({step["filter"], step["on"], step["why_not"]});
    shape.push_back(mic.value("max_filters", 0));
    for (const auto& filter : mic.value("filters", json::array()))
        shape.push_back(filter.value("found", false));
    return shape.dump();
}

// A short note for a filter's row in the list, when it isn't running for a reason the
// power button doesn't show.
std::string why_not_short(const json& step)
{
    return str(step.value("why_not", json())) == "missing" ? "Not installed" : "";
}

// The mouse wheel over a slider scrolls the page instead of moving the slider, so
// scrolling through the settings never changes one by accident. (Dragging and the
// keyboard still move it.)
void scroll_page_not_slider(Gtk::Scale& scale, Gtk::ScrolledWindow& page)
{
    auto wheel = Gtk::EventControllerScroll::create();
    wheel->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    wheel->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    wheel->signal_scroll().connect(
        [&page](double, double dy) {
            auto adjustment = page.get_vadjustment();
            adjustment->set_value(adjustment->get_value() + dy * 40);
            return true;  // the slider never sees it
        },
        false);
    scale.add_controller(wheel);
}

}  // namespace

namespace {

// A meter position (0..1 over -60..0 dB) as "-24 dB", or "-∞ dB" for silence.
std::string meter_db(double position)
{
    if (position <= 0.0)
        return "-∞ dB";
    char text[16];
    std::snprintf(text, sizeof text, "%.0f dB", position * 60.0 - 60.0);
    return text;
}

}  // namespace

// Builds the mic column: title and menu, input strip with WaveMix Mic's meter, the
// Monitor and Mic sliders, and the Filters button.
MicColumn::MicColumn(MixerWindow& window)
    : FixedWidthBox(163, Gtk::Orientation::VERTICAL, 10),  // as wide as a channel
      window_(window),
      monitor_(window, LevelControl::MicSlider{"monitor"}, "Monitor"),
      level_(window, LevelControl::MicSlider{"mic"}, "Mic")
{
    add_css_class("wm-column");
    add_css_class("wm-mic");

    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("audio-input-microphone-symbolic");
    auto* title = shrinkable(make_label("Mic", {"wm-title"}, 0.0F));
    title->set_hexpand(true);
    auto* menu = flat_menu_button("open-menu-symbolic", "Mic options");
    menu->set_popover(*make_menu());
    head->append(*icon);
    head->append(*title);
    head->append(*menu);

    // Which microphone: its icon in a strip where a channel has its apps, so the column
    // looks like the others. Clicking it lists the inputs to pick from.
    input_button_ = Gtk::make_managed<Gtk::MenuButton>();
    input_button_->add_css_class("wm-apps");
    input_button_->set_tooltip_text("Your microphone");
    auto* mic_icon = Gtk::make_managed<Gtk::Image>();
    mic_icon->set_from_icon_name("audio-input-microphone-symbolic");
    mic_icon->set_pixel_size(24);
    mic_icon->set_hexpand(true);
    mic_icon->set_halign(Gtk::Align::CENTER);
    input_button_->set_child(*mic_icon);
    input_popover_ = Gtk::make_managed<Gtk::Popover>();
    // Filled each time it opens, so it lists what's plugged in right now.
    input_popover_->signal_show().connect(sigc::mem_fun(*this, &MicColumn::fill_inputs_popover));
    input_button_->set_popover(*input_popover_);
    status_ = make_label("", {"wm-caption"});
    status_->set_wrap(true);
    status_->set_visible(false);

    monitor_.set_tooltip_text("Hear yourself, in your Personal mix. Off until you turn it up and unmute it.");
    level_.set_tooltip_text("WaveMix Mic, what OBS records");
    auto* sliders = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    sliders->set_homogeneous(true);
    sliders->append(monitor_);
    sliders->append(level_);

    filters_button_ = Gtk::make_managed<Gtk::Button>("Filters");
    filters_button_->add_css_class("wm-filters");
    filters_button_->set_tooltip_text("The mic's filters");
    filters_button_->set_valign(Gtk::Align::CENTER);
    footer_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    footer_->set_halign(Gtk::Align::CENTER);
    footer_->append(*filters_button_);
    filters_button_->signal_clicked().connect(sigc::mem_fun(*this, &MicColumn::open_filters));

    // WaveMix Mic's level, under its icon: what OBS gets.
    setup_meter(mic_meter_);
    mic_meter_.set_tooltip_text("WaveMix Mic's level");

    top_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    top_->append(*input_button_);
    top_->append(mic_meter_);
    top_->append(*status_);

    append(*head);
    append(*top_);
    append(*sliders);
    append(*footer_);
}

// Destroys the filters window (if it was made) along with the column.
MicColumn::~MicColumn() = default;

// Moves each meter towards its new peak, in the column and in the filters window.
void MicColumn::show_meters(const json& peaks)
{
    for (const char* name : {"input", "mic"})
        shown_[name] = meter_fall(shown_[name], peaks.value(name, 0.0));
    mic_meter_.set_value(shown_["mic"]);
    if (input_meter_ && input_db_) {
        input_meter_->set_value(shown_["input"]);
        input_db_->set_text(meter_db(shown_["input"]));
    }
    if (output_meter_ && output_db_) {
        output_meter_->set_value(shown_["mic"]);
        output_db_->set_text(meter_db(shown_["mic"]));
    }
}

// The mic's menu: Disable mic (Preferences turns it back on).
Gtk::Popover* MicColumn::make_menu()
{
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* box = popover_box(*popover);
    // Disabled, the column goes; Preferences has the switch to bring it back.
    menu_item(*popover, *box, "Disable mic", [this] { window_.call([&] { window_.client().set_mic_enabled(false); }); });
    return popover;
}

// Rebuilds the input popover: Default Input, every input, and a picked one that's
// unplugged, each with a checkbox that picks it.
void MicColumn::fill_inputs_popover()
{
    // Like a channel's apps list: one checkbox per input, the one in use ticked.
    auto* box = popover_box(*input_popover_);
    box->set_spacing(4);
    box->set_size_request(260, -1);
    box->append(*make_label("Microphone", {"wm-caption"}, 0.0F));

    const json& input = mic_.value("input", json::object());
    const std::string current = str(input.value("setting", json("default")));
    std::vector<std::pair<std::string, std::string>> choices;  // (setting, what's shown)
    // Whatever the system's default input is, without its name, like the outputs'
    // Default Output: it follows the default when that changes.
    choices.emplace_back("default", "Default Input");
    for (const auto& source : mic_.value("inputs", json::array()))
        choices.emplace_back(str(source["name"]), str(source["description"]));
    // A picked input that isn't plugged in stays in the list, so it's clear what's picked.
    if (std::none_of(choices.begin(), choices.end(), [&](const auto& c) { return c.first == current; }))
        choices.emplace_back(current, current + " (not plugged in)");

    Gtk::CheckButton* first = nullptr;
    for (const auto& [setting, shown] : choices) {
        auto* check = Gtk::make_managed<Gtk::CheckButton>();
        check->add_css_class("wm-app-check");
        auto* name = make_label(shown, {}, 0.0F);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        name->set_max_width_chars(30);
        check->set_child(*name);
        check->set_tooltip_text(setting == "default" ? "Whatever the system's default input is" : setting);
        if (first)
            check->set_group(*first);  // one at a time
        else
            first = check;
        check->set_active(setting == current);
        check->signal_toggled().connect([this, check, setting = setting] {
            if (!check->get_active())
                return;  // the one being unticked: the newly ticked one does the work
            input_popover_->popdown();
            window_.call([&] { window_.client().set_mic_input(setting); });
        });
        box->append(*check);
    }
}

// Shows GetState's "mic": both sliders, the "No microphone found" status, the input
// tooltip and how many filters run, refreshing the filters window if it's open.
void MicColumn::update(const json& mic)
{
    mic_ = mic;
    monitor_.update(mic.value("monitor", json()));
    level_.update(mic.value("level", json()));

    // Say why there's nothing to hear, if there isn't. (A disabled mic has no column.)
    const bool no_input = mic.value("input", json::object()).value("resolved", json()).is_null();
    status_->set_text(no_input ? "No microphone found" : "");
    status_->set_visible(no_input);
    const std::string resolved = str(mic.value("input", json::object()).value("description", json()));
    input_button_->set_tooltip_text(resolved.empty() ? "Your microphone" : "Your microphone: " + resolved);

    int running = 0;
    for (const auto& step : mic.value("chain", json::array()))
        running += step.value("running", false) ? 1 : 0;
    filters_button_->set_label(running ? "Filters · " + std::to_string(running) : "Filters");

    if (filters_window_ && filters_window_->get_visible())
        fill_filters();
}

// Shows the filters window, creating it the first time.
void MicColumn::open_filters()
{
    // Made once, then hidden when closed and shown again: it keeps its size and place.
    if (!filters_window_) {
        filters_window_ = std::make_unique<Gtk::Window>();
        filters_window_->set_title("Mic filters");
        filters_window_->set_transient_for(window_);
        filters_window_->set_default_size(720, 480);
        filters_window_->set_hide_on_close(true);

        // A click outside a setting's value box takes the focus from it, which tidies
        // its value; clicks on what can't take the focus (labels, the background)
        // otherwise leave the cursor in the box.
        auto click = Gtk::GestureClick::create();
        click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        click->signal_pressed().connect([this](int, double x, double y) {
            Gtk::Widget* focus = filters_window_->get_focus();
            Gtk::Widget* box = focus;
            while (box && !box->has_css_class("wm-value"))
                box = box->get_parent();
            if (!box)
                return;
            Gtk::Widget* clicked = filters_window_->pick(x, y);
            if (clicked != box && !(clicked && clicked->is_ancestor(*box)))
                filters_window_->unset_focus();
        });
        filters_window_->add_controller(click);
    }
    chain_shown_.clear();  // always up to date when it opens
    fill_filters();
    filters_window_->present();
}

// Rebuilds the filters window when the chain's shape changed: the list with its meters,
// delay and buttons on the left, the selected filter's settings on the right.
void MicColumn::fill_filters()
{
    const std::string shape = chain_shape(mic_);
    if (shape == chain_shown_)
        return;
    chain_shown_ = shape;
    const json chain = mic_.value("chain", json::array());
    selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(chain.size()) - 1));

    auto* panes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);

    // ---- Left: the list, top to bottom as the voice goes through it, and its buttons.
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    left->add_css_class("wm-filters-list");
    // Exactly 340 px wide, whatever is in it or beside it. A box alone only has a
    // minimum width: it would grow to fit a long line (the packages to install, say),
    // or be squeezed back towards the minimum when the settings pane wants room, so its
    // edge would move as different filters are shown. A scrolled window that doesn't
    // pass on its child's natural width asks for exactly its minimum.
    auto* left_frame = Gtk::make_managed<Gtk::ScrolledWindow>();
    left_frame->set_policy(Gtk::PolicyType::EXTERNAL, Gtk::PolicyType::NEVER);
    left_frame->set_min_content_width(kFilterListWidth);
    left_frame->set_propagate_natural_width(false);
    left_frame->set_hexpand(false);
    left_frame->set_child(*left);

    // The mic before and after the chain: what the filters do to the level.
    auto* meters = Gtk::make_managed<Gtk::Grid>();
    meters->set_column_spacing(8);
    meters->set_row_spacing(4);
    const auto meter_row = [&](int row, const std::string& name, Gtk::LevelBar*& bar, Gtk::Label*& db) {
        bar = Gtk::make_managed<Gtk::LevelBar>();
        setup_meter(*bar);
        bar->set_hexpand(true);
        bar->set_valign(Gtk::Align::CENTER);
        db = make_label("-∞ dB", {"wm-level"}, 1.0F);
        db->set_width_chars(7);
        meters->attach(*make_label(name, {"wm-caption"}, 0.0F), 0, row);
        meters->attach(*bar, 1, row);
        meters->attach(*db, 2, row);
    };
    meter_row(0, "Mic in", input_meter_, input_db_);
    meter_row(1, "WaveMix Mic", output_meter_, output_db_);
    left->append(*meters);

    // How late the filters make the voice, to match in OBS (a Sync Offset on the
    // camera) if it's enough to notice.
    char delay[48];
    std::snprintf(delay, sizeof delay, "Delay through the filters: %.1f ms", mic_.value("delay_ms", 0.0));
    auto* delay_label = make_label(delay, {"wm-caption"}, 0.0F);
    delay_label->set_tooltip_text("Noise removal adds 10 ms and the limiter 5.3 ms; the other filters add none");
    left->append(*delay_label);
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_overlay_scrolling(false);  // a scrollbar beside the rows, never over them
    scroller->set_vexpand(true);
    list_ = Gtk::make_managed<Gtk::ListBox>();
    list_->add_css_class("wm-filters-rows");
    list_->set_selection_mode(Gtk::SelectionMode::BROWSE);
    for (size_t i = 0; i < chain.size(); ++i)
        list_->append(*filter_row(chain[i], static_cast<int>(i)));
    if (chain.empty())
        list_->set_placeholder(*make_label("No filters yet", {"dim"}));
    scroller->set_child(*list_);
    left->append(*scroller);

    // Add, remove, move up, move down, like OBS.
    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto* add = Gtk::make_managed<Gtk::MenuButton>();
    add->set_icon_name("list-add-symbolic");
    add->set_tooltip_text("Add a filter");
    auto* menu = Gtk::make_managed<Gtk::Popover>();
    auto* items = popover_box(*menu);
    for (const auto& filter : mic_.value("filters", json::array())) {
        const std::string id = str(filter["filter"]);
        const bool found = filter.value("found", false);
        auto* item = menu_item(
            *menu, *items, filter_name(id) + (found ? "" : " (not installed)"),
            [this, id] {
                window_.call([&] {
                    // The new filter goes at the end, and its settings are shown.
                    selected_ = window_.client().add_mic_filter(id);
                    chain_shown_.clear();
                });
            },
            found);
        if (!found)
            item->set_tooltip_text("Install " + str(filter["package"]) + " first");
    }
    add->set_popover(*menu);
    // Upwards, over the list: the button is at the bottom of the window. (The menu
    // button sets its popover's side from this, overriding the popover's own. GTK
    // still opens it downwards if the screen has no room above.)
    add->set_direction(Gtk::ArrowType::UP);
    const auto tool = [&](const char* icon, const std::string& tip, std::function<void()> action) {
        auto* button = Gtk::make_managed<Gtk::Button>();
        button->set_icon_name(icon);
        button->set_tooltip_text(tip);
        button->set_sensitive(!chain.empty());
        button->signal_clicked().connect(std::move(action));
        buttons->append(*button);
    };
    buttons->append(*add);
    tool("user-trash-symbolic", "Remove the selected filter", [this] {
        const int position = selected_;
        window_.call([&] { window_.client().remove_mic_filter(position); });
    });
    tool("go-up-symbolic", "Move up", [this] { move_selected(-1); });
    tool("go-down-symbolic", "Move down", [this] { move_selected(1); });
    left->append(*buttons);

    // Plugins that aren't there, with what to install.
    std::vector<std::string> packages;
    for (const auto& filter : mic_.value("filters", json::array())) {
        const std::string package = str(filter["package"]);
        if (!filter.value("found", false) && std::find(packages.begin(), packages.end(), package) == packages.end())
            packages.push_back(package);
    }
    if (!packages.empty()) {
        std::string text = "Some filters need a package installed first: ";
        for (size_t i = 0; i < packages.size(); ++i)
            text += (i ? ", " : "") + packages[i];
        left->append(*make_label(text + ".", {"wm-caption"}, 0.0F, true));
    }

    // ---- Right: the selected filter's settings.
    settings_pane_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    settings_pane_->add_css_class("wm-filters-settings");
    settings_pane_->set_hexpand(true);

    panes->append(*left_frame);
    panes->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    panes->append(*settings_pane_);
    filters_window_->set_child(*panes);

    if (auto* row = list_->get_row_at_index(selected_))
        list_->select_row(*row);
    list_->signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row)
            return;
        selected_ = row->get_index();
        show_settings();
    });
    show_settings();
}

// One filter's row in the list: drag handle, power button, name and a short note.
Gtk::Widget* MicColumn::filter_row(const json& step, int position)
{
    const std::string filter = str(step["filter"]);
    const bool on = step.value("on", true);

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->add_css_class("wm-filter");

    auto* handle = Gtk::make_managed<Gtk::Image>();
    handle->set_from_icon_name("list-drag-handle-symbolic");
    handle->add_css_class("wm-drag-handle");
    handle->set_tooltip_text("Drag to move");
    handle->set_cursor(Gdk::Cursor::create("grab"));
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    text->set_hexpand(true);
    text->set_valign(Gtk::Align::CENTER);
    if (!step.value("running", false))
        text->add_css_class("wm-filter-idle");  // the name dimmed; the power button stays bright
    text->append(*shrinkable(make_label(filter_name(filter), {"wm-filter-name"}, 0.0F)));
    if (const std::string why = why_not_short(step); !why.empty())
        text->append(*make_label(why, {"wm-filter-why"}, 0.0F));
    // A power button: green while the filter is on, grey when it's off.
    auto* power = Gtk::make_managed<Gtk::ToggleButton>();
    power->set_icon_name("system-shutdown-symbolic");
    power->add_css_class("wm-power");
    power->set_active(on);
    power->set_valign(Gtk::Align::CENTER);
    power->set_tooltip_text(on ? "On. Click to switch it off (it keeps its place and settings)."
                               : "Off. Click to switch it on.");
    power->signal_toggled().connect([this, power, position] {
        const bool now = power->get_active();
        window_.call([&] { window_.client().set_mic_filter_on(position, now); });
    });
    row->append(*handle);
    row->append(*text);
    row->append(*power);

    // Dragged by its handle onto another filter: it takes that one's place.
    auto drag = Gtk::DragSource::create();
    drag->set_actions(Gdk::DragAction::MOVE);
    drag->signal_prepare().connect(
        [position](double, double) {
            Glib::Value<int> value;
            value.init(value.value_type());
            value.set(position);
            return Gdk::ContentProvider::create(value);
        },
        false);
    drag->signal_drag_begin().connect([row, drag](const Glib::RefPtr<Gdk::Drag>&) {
        drag->set_icon(Gtk::WidgetPaintable::create(*row), 0, 0);
    });
    handle->add_controller(drag);
    auto drop = Gtk::DropTarget::create(Glib::Value<int>::value_type(), Gdk::DragAction::MOVE);
    drop->signal_drop().connect(
        [this, position](const Glib::ValueBase& value, double, double) {
            Glib::Value<int> dropped;
            dropped.init(value.gobj());
            const int from = dropped.get();
            if (from != position)
                window_.call([&] {
                    window_.client().move_mic_filter(from, position);
                    selected_ = position;
                });
            return true;
        },
        false);
    row->add_controller(drop);
    return row;
}

// Moves the selected filter by `by` places (if it stays in the list) and keeps it selected.
void MicColumn::move_selected(int by)
{
    const int to = selected_ + by;
    if (to < 0 || to >= static_cast<int>(mic_.value("chain", json::array()).size()))
        return;
    const int from = selected_;
    window_.call([&] {
        window_.client().move_mic_filter(from, to);
        selected_ = to;  // the settings shown follow the filter
    });
}

// Fills the settings pane with the selected filter's name, status and controls.
void MicColumn::show_settings()
{
    remove_children(*settings_pane_);
    const json chain = mic_.value("chain", json::array());
    if (chain.empty()) {
        auto* empty = make_label("No filters yet: your mic goes to OBS as it is. Add one with +.", {"dim"}, 0.5F, true);
        empty->set_vexpand(true);
        settings_pane_->append(*empty);
        return;
    }
    const json& step = chain.at(static_cast<size_t>(selected_));
    const std::string filter = str(step["filter"]);

    settings_pane_->append(*make_label(filter_name(filter), {"wm-dialog-title"}, 0.0F));
    if (const std::string why = why_not(step, mic_); !why.empty())
        settings_pane_->append(*make_label(why, {"wm-filter-why"}, 0.0F, true));

    auto* page = Gtk::make_managed<Gtk::ScrolledWindow>();
    page->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    page->set_overlay_scrolling(false);
    page->set_vexpand(true);
    auto* controls_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    controls_box->set_margin_end(8);  // clear of the scrollbar
    page->set_child(*controls_box);
    settings_pane_->append(*page);

    // The equalizer's bands show their gain; frequency and Q are tucked away below
    // them, as they're set once and left.
    const json& settings = step.value("settings", json::object());
    json controls = json::array();
    for (const auto& filter_info : mic_.value("filters", json::array()))
        if (filter_info["filter"] == filter)
            controls = filter_info.value("controls", json::array());
    Gtk::Box* tuning = nullptr;  // made only for the equalizer: an unparented widget is never freed
    for (const auto& control : controls) {
        const std::string key = str(control["key"]);
        const double value = settings.value(key, control.value("default", 0.0));
        if (!band_of(key) || key.ends_with("_gain")) {
            controls_box->append(*control_row(control, value, selected_, *page));
            continue;
        }
        if (!tuning)
            tuning = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        tuning->append(*control_row(control, value, selected_, *page));
    }
    if (tuning) {
        auto* more = Gtk::make_managed<Gtk::Expander>("Band frequencies and Q");
        more->add_css_class("wm-caption");
        more->set_margin_top(6);
        more->set_child(*tuning);
        controls_box->append(*more);
    }
}

// One setting's row: its name, a slider (log scale for Hz) and its value, sent live.
Gtk::Widget* MicColumn::control_row(const json& control, double value, int position, Gtk::ScrolledWindow& page)
{
    const std::string key = str(control["key"]);
    const std::string unit = str(control["unit"]);
    const double min = control.value("min", 0.0);
    const double max = control.value("max", 1.0);

    // Frequencies move along a log scale, as the ear hears them: 20 Hz to 200 Hz takes
    // as much of the slider as 2 kHz to 20 kHz.
    const bool log_scale = unit == "Hz";
    const auto to_slider = [log_scale](double v) { return log_scale ? std::log10(v) : v; };
    const auto from_slider = [log_scale](double v) { return log_scale ? std::pow(10.0, v) : v; };
    const double step = unit == "dB" ? 0.5 : (unit == "%" ? 1.0 : (log_scale ? 0.01 : 0.1));

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto* name = make_label(control_name(key), {"wm-caption"}, 0.0F);
    name->set_size_request(110, -1);
    auto adjustment = Gtk::Adjustment::create(to_slider(value), to_slider(min), to_slider(max), step, step * 10);
    auto* scale = Gtk::make_managed<Gtk::Scale>(adjustment, Gtk::Orientation::HORIZONTAL);
    scale->set_draw_value(false);
    scale->set_hexpand(true);
    scale->add_css_class("wm-personal");
    scale->set_tooltip_text(control_name(key));
    scroll_page_not_slider(*scale, page);
    // The value in a box: it follows the slider, and the slider follows what's typed,
    // as it's typed.
    auto* shown = Gtk::make_managed<Gtk::Entry>();
    shown->add_css_class("wm-level");
    shown->add_css_class("wm-value");
    shown->set_text(format_value(value, unit, key));
    shown->set_width_chars(7);
    shown->set_max_width_chars(7);
    shown->set_alignment(1.0F);
    shown->set_valign(Gtk::Align::CENTER);
    shown->set_tooltip_text("Type a value: the slider follows");
    // Which way a change is going, so neither side answers the other: the slider
    // doesn't rewrite the text under the cursor, and the tidied text ("1.2 kHz", rounded)
    // doesn't move the slider.
    struct Sync {
        bool typing = false;
        bool tidying = false;
    };
    auto sync = std::make_shared<Sync>();
    const auto tidy = [adjustment, shown, sync, from_slider, unit, key] {
        sync->tidying = true;
        shown->set_text(format_value(from_slider(adjustment->get_value()), unit, key));
        sync->tidying = false;
    };
    adjustment->signal_value_changed().connect([this, adjustment, sync, tidy, from_slider, key, position] {
        if (!sync->typing)
            tidy();
        // Live, without waiting: the daemon sets it on the running filter.
        window_.client().set_mic_filter_value(position, key, from_slider(adjustment->get_value()),
                                              [this](const std::string& error) { window_.show_error(error); });
    });

    // Each change to the text moves the slider, kept within its range; text that isn't
    // (yet) a number, such as "-", leaves it where it is.
    shown->signal_changed().connect([adjustment, shown, sync, to_slider, min, max, unit] {
        if (sync->tidying)
            return;
        const auto typed = parse_value(shown->get_text(), unit);
        if (!typed)
            return;
        sync->typing = true;
        adjustment->set_value(to_slider(std::clamp(*typed, min, max)));
        sync->typing = false;
    });
    // Enter, or leaving the box, shows the value as the slider has it: in its unit,
    // within the range, or back as it was if what's typed isn't a number.
    shown->signal_activate().connect([this, tidy] {
        tidy();
        if (filters_window_)
            filters_window_->unset_focus();
    });
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect(tidy);
    shown->add_controller(focus);

    row->append(*name);
    row->append(*scale);
    row->append(*shown);
    return row;
}

}  // namespace wavemix::gui
