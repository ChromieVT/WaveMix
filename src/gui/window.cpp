// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT

#include "gui/window.hpp"

#include <set>

#include "build_config.hpp"
#include "core/log.hpp"
#include "core/mic.hpp"
#include "core/names.hpp"
#include "core/profiles.hpp"
#include "report/bug_report.hpp"
#include "gui/helpers.hpp"
#include "gui/theme.hpp"
#include "gui/widgets.hpp"

namespace wavemix::gui {

using core::json;

namespace {

constexpr const char* kNotRunning =
    "The WaveMix service isn't running, so what goes through WaveMix is quiet (apps stay in their channels). "
    "Reopen WaveMix to start it again.";
constexpr const char* kWaitingForPipeWire = "Waiting for PipeWire…";

}  // namespace

// Builds the window: title bar with the version and main menu, the toast overlay, the
// channels area and the Outputs panel; then shows the state and polls the meters while
// the window is on screen.
MixerWindow::MixerWindow(client::Client& client) : client_(client)
{
    set_title("WaveMix");
    set_icon_name(std::string(core::kAppId));
    set_default_size(1000, 620);

    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    header->pack_start(*make_profiles_menu());
    header->pack_end(*make_main_menu());

    // The title, with the version beside it: which build is running should never be a
    // question when something goes wrong.
    auto* title = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* name = make_label("WaveMix", {"wm-header-title"});
    auto* version = make_label(WAVEMIX_VERSION, {"wm-header-version"});
    // Centred, not baseline-aligned: lining up two font sizes by baseline makes the
    // title bar taller than its 48 px.
    version->set_valign(Gtk::Align::CENTER);
    name->set_valign(Gtk::Align::CENTER);
    title->append(*name);
    title->append(*version);
    header->set_title_widget(*title);
    set_titlebar(*header);

    // Messages float over the bottom of the window ("toasts"), so showing one never
    // moves the mixer. Most go away by themselves.
    auto* toast = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    toast->add_css_class("wm-toast");
    message_.set_xalign(0);
    message_.set_wrap(true);
    message_.set_max_width_chars(70);
    auto* close = Gtk::make_managed<Gtk::Button>();
    close->set_icon_name("window-close-symbolic");
    close->add_css_class("flat");
    close->set_tooltip_text("Dismiss");
    close->set_valign(Gtk::Align::CENTER);
    close->signal_clicked().connect(sigc::mem_fun(*this, &MixerWindow::hide_message));
    toast->append(message_);
    toast->append(*close);
    toast_.set_child(*toast);
    toast_.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
    toast_.set_reveal_child(false);
    toast_.set_halign(Gtk::Align::CENTER);
    toast_.set_valign(Gtk::Align::END);
    toast_.set_margin_bottom(24);
    toast_.set_can_target(true);

    body_.set_vexpand(true);
    outputs_ = Gtk::make_managed<OutputsPanel>(*this);
    outer_.append(body_);
    outer_.append(*outputs_);
    overlay_.set_child(outer_);
    overlay_.add_overlay(toast_);
    set_child(overlay_);

    client_.on_changed([this] { queue_refresh(); });
    refresh();

    // The meters, 20 times a second while the window is on screen (not while it's
    // hidden to the tray). The service stops measuring a few seconds after the last ask.
    signal_map().connect([this] {
        meter_timer_ = Glib::signal_timeout().connect(
            [this] {
                client_.get_meters([this, alive = alive_](const json& peaks) {
                    if (*alive)
                        show_meters(peaks);
                });
                return true;
            },
            50);
    });
    signal_unmap().connect([this] { meter_timer_.disconnect(); });
}

// Stops callbacks and timers that could otherwise reach the window after it's gone.
MixerWindow::~MixerWindow()
{
    *alive_ = false;  // replies still on their way find nobody
    meter_timer_.disconnect();
    client_.on_changed({});  // the client outlives the window: stop calling us
    refresh_pending_.disconnect();
}

// Hands GetMeters' peaks to the mic column and to each channel's column.
void MixerWindow::show_meters(const json& peaks)
{
    // Every column gets its own: the mic's, and each channel's (silent if it isn't there,
    // or the service is older than the channel meters).
    if (!peaks.is_object())
        return;
    if (mic_)
        mic_->show_meters(peaks);
    const auto channels = peaks.find("channels");
    const bool any = channels != peaks.end() && channels->is_object();
    for (const auto& [id, column] : columns_) {
        double peak = 0.0;
        if (any)
            if (const auto it = channels->find(id); it != channels->end() && it->is_number())
                peak = it->get<double>();
        column->show_meter(peak);
    }
}

// =====================================================================================
// Messages and calls
// =====================================================================================

// Shows a toast, hidden again after `seconds` (or kept until hidden when 0).
void MixerWindow::show_message(const std::string& text, unsigned seconds)
{
    message_.set_text(text);
    toast_.set_reveal_child(true);
    toast_timer_.disconnect();
    if (seconds > 0)
        toast_timer_ = Glib::signal_timeout().connect_seconds(
            [this] {
                hide_message();
                return false;
            },
            seconds);
}

// Hides the toast and cancels its timer.
void MixerWindow::hide_message()
{
    toast_timer_.disconnect();
    toast_.set_reveal_child(false);
}

// Logs the error and shows it for 10 seconds with a hint on reporting it.
void MixerWindow::show_error(const std::string& message)
{
    // Errors also go to the log, and the hint says how to get it to us. They stay a
    // little longer than other messages: there's more to read.
    core::log::error("gui", message);
    show_message(message + "\nIf this keeps happening, ☰ → Save bug report… makes a file to attach to a GitHub "
                           "issue.",
                 10);
}

// Asks where to save, then writes a fresh bug report there and says where it went.
void MixerWindow::save_bug_report()
{
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save bug report");
    dialog->set_initial_name(report::suggested_file_name());
    dialog->save(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try {
            file = dialog->save_finish(result);
        } catch (const Glib::Error&) {
            return;  // cancelled
        }
        // Made now, after the file was picked, so it includes everything up to here.
        try {
            const std::string text = report::bug_report();
            std::string etag;
            file->replace_contents(text, "", etag);
            core::log::info("gui", "bug report saved");
            show_message("Bug report saved as " + file->get_parse_name() +
                             ". Read it, then attach it to an issue at " + std::string(core::kWebsite) + "/issues.",
                         8);
        } catch (const Glib::Error& e) {
            show_error("Could not save the bug report: " + std::string(e.what()));
        }
    });
}

// Runs the action, shows a ClientError as a toast, then refreshes.
void MixerWindow::call(const std::function<void()>& action)
{
    try {
        action();
    } catch (const client::ClientError& e) {
        show_error(e.what());
    }
    queue_refresh();
}

// Routes a stream into a channel ("" sends it back out) through the daemon.
void MixerWindow::assign(uint32_t stream, const std::string& channel)
{
    call([&] { client_.assign_app(stream, channel); });
}

// Adds a channel through the daemon.
void MixerWindow::add_channel(const std::string& label, const std::string& icon)
{
    call([&] { client_.add_channel(label, icon); });
}

// =====================================================================================
// Dialogs
// =====================================================================================

// Creates the one modal dialog (closing any other) that Escape and its close button close.
Gtk::Window& MixerWindow::new_dialog(const std::string& title)
{
    close_dialog();  // one at a time: a dialog can open the next (Preferences, Reset)
    dialog_ = std::make_unique<Gtk::Window>();
    Gtk::Window& dialog = *dialog_;
    dialog.set_title(title);
    dialog.set_transient_for(*this);
    dialog.set_modal(true);
    dialog.signal_close_request().connect(
        [this] {
            close_dialog();
            return true;  // we close it ourselves
        },
        false);

    // Escape closes it, like any dialog.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint key, guint, Gdk::ModifierType) {
            if (key != GDK_KEY_Escape)
                return false;
            close_dialog();
            return true;
        },
        false);
    dialog.add_controller(keys);
    return dialog;
}

// Hides the open dialog and deletes it once the current signal handler is done.
void MixerWindow::close_dialog()
{
    if (!dialog_)
        return;
    dialog_->hide();
    // Delete it later: this usually runs inside one of the dialog's own signal handlers.
    // The idle callback owns it until then, so a dialog opened meanwhile is left alone.
    Glib::signal_idle().connect_once([closed = std::shared_ptr<Gtk::Window>(std::move(dialog_))] {});
}

// Shows a two-button question with Cancel as the default and `yes` as a destructive action.
void MixerWindow::confirm(const std::string& title, const std::string& question, const std::string& detail,
                          const std::string& yes, std::function<void()> on_yes, std::function<void()> on_no)
{
    Gtk::Window& dialog = new_dialog(title);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(18);
    auto* heading = make_label(question, {"wm-dialog-title"}, 0.0F, true);
    auto* text = make_label(detail, {"dim"}, 0.0F, true);
    text->set_max_width_chars(48);
    box->append(*heading);
    box->append(*text);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->add_css_class("wm-dialog-buttons");
    buttons->set_halign(Gtk::Align::END);
    buttons->set_margin_top(8);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* accept = Gtk::make_managed<Gtk::Button>(yes);
    accept->add_css_class("destructive-action");
    buttons->append(*cancel);
    buttons->append(*accept);
    box->append(*buttons);
    dialog.set_child(*box);
    dialog.set_default_widget(*cancel);

    cancel->signal_clicked().connect([this, on_no = std::move(on_no)] {
        close_dialog();
        if (on_no)
            on_no();
    });
    accept->signal_clicked().connect([this, on_yes = std::move(on_yes)] {
        close_dialog();
        on_yes();
    });
    dialog.present();
    cancel->grab_focus();
}

// Asks for a new name in a dialog and renames the channel if it isn't blank.
void MixerWindow::rename_channel(const json& channel)
{
    const std::string id = str(channel["id"]);
    Gtk::Window& dialog = new_dialog("Rename channel");

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(16);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(str(channel["label"]));
    box->append(*entry);
    box->append(*make_label("Other mixers (pavucontrol etc.) show the new name after your next login.",
                            {"wm-caption"}, 0.0F, true));

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->add_css_class("wm-dialog-buttons");
    buttons->set_halign(Gtk::Align::END);
    buttons->set_margin_top(6);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* rename = Gtk::make_managed<Gtk::Button>("Rename");
    rename->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*rename);
    box->append(*buttons);
    dialog.set_child(*box);

    auto apply = [this, entry, id] {
        std::string text = entry->get_text();
        text.erase(0, text.find_first_not_of(" \t"));
        text.erase(text.find_last_not_of(" \t") + 1);
        if (!text.empty())
            call([&] { client_.rename_channel(id, text); });
        close_dialog();
    };
    cancel->signal_clicked().connect(sigc::mem_fun(*this, &MixerWindow::close_dialog));
    rename->signal_clicked().connect(apply);
    entry->signal_activate().connect(apply);
    dialog.present();
}

// Enables the channel straight away, or asks for confirmation before disabling it.
void MixerWindow::set_channel_enabled(const json& channel, bool enabled)
{
    const std::string id = str(channel["id"]);
    if (enabled) {
        call([&] { client_.set_channel_enabled(id, true); });
        return;
    }
    confirm("Disable channel", "Disable “" + str(channel["label"]) + "”?",
            "Its apps go back to your default output, and OBS no longer hears it. It keeps its place, name, "
            "levels and apps, and they come back when you enable it again.",
            "Disable", [this, id] { call([&] { client_.set_channel_enabled(id, false); }); });
}

// Shows a dialog with a text entry (at most kMaxProfileName characters), an optional
// note, and Cancel and `ok` buttons; passes the trimmed text to `on_ok` unless it's empty.
void MixerWindow::ask_text(const std::string& title, const std::string& initial, const std::string& note,
                           const std::string& ok, std::function<void(const std::string&)> on_ok)
{
    Gtk::Window& dialog = new_dialog(title);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(16);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(initial);
    entry->set_max_length(static_cast<int>(core::kMaxProfileName));
    box->append(*entry);
    if (!note.empty()) {
        auto* caption = make_label(note, {"wm-caption"}, 0.0F, true);
        caption->set_max_width_chars(48);
        box->append(*caption);
    }

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->add_css_class("wm-dialog-buttons");
    buttons->set_halign(Gtk::Align::END);
    buttons->set_margin_top(6);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* accept = Gtk::make_managed<Gtk::Button>(ok);
    accept->add_css_class("suggested-action");
    buttons->append(*cancel);
    buttons->append(*accept);
    box->append(*buttons);
    dialog.set_child(*box);

    auto apply = [this, entry, on_ok = std::move(on_ok)] {
        std::string text = entry->get_text();
        text.erase(0, text.find_first_not_of(" \t"));
        text.erase(text.find_last_not_of(" \t") + 1);
        close_dialog();
        if (!text.empty())
            on_ok(text);
    };
    cancel->signal_clicked().connect(sigc::mem_fun(*this, &MixerWindow::close_dialog));
    accept->signal_clicked().connect(apply);
    entry->signal_activate().connect(apply);
    dialog.present();
    entry->grab_focus();
}

// Asks for confirmation, then removes the channel.
void MixerWindow::remove_channel(const json& channel)
{
    const std::string id = str(channel["id"]);
    confirm("Remove channel", "Remove “" + str(channel["label"]) + "”?",
            "Apps in this channel go back to your default output and their assignment to it is forgotten.",
            "Remove", [this, id] { call([&] { client_.remove_channel(id); }); });
}

// =====================================================================================
// Refreshing from the daemon
// =====================================================================================

// Schedules one refresh 50 ms from now, unless one is already scheduled.
void MixerWindow::queue_refresh()
{
    // A burst of changes (and our own calls) gives one refresh, 50 ms later.
    if (!refresh_pending_.connected()) {
        refresh_pending_ = Glib::signal_timeout().connect(
            [this] {
                refresh();
                return false;
            },
            50);
    }
}

// Fetches GetState and redraws: status toasts, outputs, theme, and the columns
// (rebuilt when their structure changed, updated otherwise).
void MixerWindow::refresh()
{
    try {
        state_ = client_.state();
    } catch (const client::ClientError&) {
        show_message(kNotRunning);
        return;
    }

    if (!state_.value("connected", false))
        show_message(kWaitingForPipeWire);  // stays until it's over
    else if (toast_.get_reveal_child() &&
             (message_.get_text() == kWaitingForPipeWire || message_.get_text() == kNotRunning))
        hide_message();  // that problem is over
    outputs_->update(state_);
    if (state_.contains("profiles"))
        profile_label_->set_text(str(state_["profiles"].value("active", json(""))));
    profiles_button_->set_visible(state_.contains("profiles"));  // not from a service before profiles

    // The theme (Preferences), when it changes: a built-in one, or Custom, the user's
    // stylesheet on top of Dark.
    const json& settings = state_.value("settings", json::object());
    const std::string theme_id = str(settings.value("theme", json("dark")));
    std::pair<std::string, std::string> theme{
        theme_id, theme_id == kCustomTheme ? str(settings.value("custom_theme", json(""))) : std::string()};
    if (theme != theme_shown_) {
        theme_shown_ = theme;
        set_theme(theme.first, theme.second, [this](const std::string& problem) { show_message(problem, 10); });
    }

    decltype(structure_) structure;
    for (const auto& ch : state_["channels"])
        structure.emplace_back(str(ch["id"]), str(ch["label"]), str(ch["icon"]), ch.value("in_vod", true),
                               ch.value("enabled", true));
    // The VOD buttons come and go with the VOD Mix preference, so it rebuilds too.
    const bool vod = state_.value("settings", json::object()).value("vod_enabled", false);
    // The Mic column is there while the mic is enabled (Preferences) and the service is
    // connected to PipeWire.
    const bool mic = state_.contains("mic") && state_["mic"].value("enabled", true);
    if (!built_ || structure != structure_ || vod != built_with_vod_ || mic != built_with_mic_) {
        structure_ = std::move(structure);
        built_with_vod_ = vod;
        built_with_mic_ = mic;
        rebuild();
    }

    if (mic_)
        mic_->update(state_["mic"]);
    for (const auto& ch : state_["channels"]) {
        if (auto it = columns_.find(str(ch["id"])); it != columns_.end())
            it->second->update(ch, state_["channels"]);
    }
}

// Rebuilds the channels row from the state: the mic column, a column per channel and
// the Add column, or the first-run screen when there are no channels.
void MixerWindow::rebuild()
{
    built_ = true;
    remove_children(body_);
    columns_.clear();
    mic_ = nullptr;
    const json& channels = state_["channels"];

    // The mic comes first, with or without channels: it doesn't need any. (Not while
    // it's disabled, the daemon isn't connected, or the daemon is older than the mic.)
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    row->set_margin(12);
    tops_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::VERTICAL);
    footers_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::VERTICAL);
    if (built_with_mic_) {
        mic_ = Gtk::make_managed<MicColumn>(*this);
        mic_->set_hexpand(false);  // its own width, also beside the first-run screen, which fills the row
        tops_->add_widget(mic_->top());
        footers_->add_widget(mic_->footer());
        row->append(*mic_);
    }

    if (channels.empty()) {
        // First run: say what WaveMix does, and offer the presets right away.
        auto* empty = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        empty->set_valign(Gtk::Align::CENTER);
        empty->set_halign(Gtk::Align::CENTER);
        empty->set_vexpand(true);
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("multimedia-volume-control-symbolic");
        icon->set_pixel_size(64);
        empty->append(*icon);
        empty->append(*make_label("No channels yet", {"wm-empty-title"}));
        empty->append(*make_label("Add a channel, then route apps into it. Each channel gets its own\n"
                                  "Personal and Stream volume. Apps you don't route keep playing\n"
                                  "through your normal output, just like before.",
                                  {"dim"}));
        auto* add = Gtk::make_managed<AddColumn>(*this, std::set<std::string>{}, true);
        add->set_halign(Gtk::Align::CENTER);
        add->set_margin_top(8);
        empty->append(*add);
        empty->set_hexpand(true);
        row->set_vexpand(true);
        row->append(*empty);
        body_.append(*row);
        return;
    }

    row->set_halign(Gtk::Align::START);
    std::set<std::string> used;
    const int count = static_cast<int>(channels.size());
    for (int i = 0; i < count; ++i) {
        const json& ch = channels[static_cast<size_t>(i)];
        auto* column = Gtk::make_managed<ChannelColumn>(*this, ch, i);
        tops_->add_widget(column->top());
        footers_->add_widget(column->footer());
        columns_[str(ch["id"])] = column;
        used.insert(str(ch["label"]));
        row->append(*column);
    }
    if (count < core::kMaxChannels)
        row->append(*Gtk::make_managed<AddColumn>(*this, used));

    // The row scrolls sideways when the channels don't fit. Its scrollbar is a widget of
    // its own below it, rather than the scrolled window's: that one is drawn inside the
    // row's area, over the space below the channels, so they'd sit right on top of it.
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::EXTERNAL, Gtk::PolicyType::NEVER);
    scroller->set_vexpand(true);
    scroller->set_child(*row);
    const auto scrolled = scroller->get_hadjustment();
    auto* scrollbar = Gtk::make_managed<Gtk::Scrollbar>(scrolled, Gtk::Orientation::HORIZONTAL);
    scrollbar->set_margin_start(12);
    scrollbar->set_margin_end(12);
    // Always there, so the channels are the same height either way, but only seen (and
    // only clickable) while there's something to scroll to.
    const auto show_if_needed = [scrollbar, scrolled] {
        const bool needed = scrolled->get_upper() - scrolled->get_page_size() > 0.5;
        scrollbar->set_opacity(needed ? 1.0 : 0.0);
        scrollbar->set_can_target(needed);
    };
    show_if_needed();
    scrolled->signal_changed().connect(sigc::track_object(show_if_needed, *scrollbar));
    body_.append(*scroller);
    body_.append(*scrollbar);
}

// Moves a dragged channel to the column it was dropped on.
void MixerWindow::drop_channel(const std::string& id, int position)
{
    // Dropped onto column `position`: it takes that place, and the others move aside.
    const json& channels = state_.contains("channels") ? state_["channels"] : json::array();
    for (size_t i = 0; i < channels.size(); ++i) {
        if (str(channels[i]["id"]) == id && static_cast<int>(i) == position)
            return;  // dropped onto itself
    }
    call([&] { client_.move_channel(id, position); });
}

// =====================================================================================
// Settings
// =====================================================================================

// Builds the title bar's profiles button: the profile in use and an arrow, hidden until
// the state lists profiles.
Gtk::MenuButton* MixerWindow::make_profiles_menu()
{
    // The name of the profile in use, cut short past about 20 characters so it never
    // crowds the title, and a menu filled each time it opens.
    auto* button = Gtk::make_managed<Gtk::MenuButton>();
    profiles_button_ = button;
    button->set_visible(false);  // until a state says there are profiles
    button->set_has_frame(false);
    button->set_tooltip_text("Profiles");
    button->add_css_class("wm-profiles");
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    profile_label_ = make_label("", {"wm-profile-name"}, 0.0F);
    profile_label_->set_ellipsize(Pango::EllipsizeMode::END);
    profile_label_->set_max_width_chars(20);
    auto* arrow = Gtk::make_managed<Gtk::Image>();
    arrow->set_from_icon_name("pan-down-symbolic");
    content->append(*profile_label_);
    content->append(*arrow);
    button->set_child(*content);
    profiles_popover_ = Gtk::make_managed<Gtk::Popover>();
    profiles_popover_->signal_show().connect(sigc::mem_fun(*this, &MixerWindow::fill_profiles_menu));
    button->set_popover(*profiles_popover_);
    return button;
}

// Rebuilds the profiles menu: every profile (the one in use ticked, a click switches),
// then Save as, Rename, Export, Import and Delete (only while there's another profile).
void MixerWindow::fill_profiles_menu()
{
    auto* box = popover_box(*profiles_popover_);
    const json profiles = state_.value("profiles", json::object());
    const std::string active = str(profiles.value("active", json("")));
    const json names = profiles.value("names", json::array());

    // The profiles: the one in use ticked, a click on another switches to it.
    for (const auto& item : names) {
        const std::string name = str(item);
        auto* button = Gtk::make_managed<Gtk::Button>();
        button->add_css_class("flat");
        button->add_css_class("wm-menu-item");
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* tick = Gtk::make_managed<Gtk::Image>();
        tick->set_from_icon_name("object-select-symbolic");
        tick->set_opacity(name == active ? 1.0 : 0.0);
        auto* label = make_label(name, {}, 0.0F);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_max_width_chars(28);
        row->append(*tick);
        row->append(*label);
        button->set_child(*row);
        button->signal_clicked().connect([this, name, active] {
            profiles_popover_->popdown();
            if (name != active)
                call([&] { client_.switch_profile(name); });
        });
        box->append(*button);
    }
    box->append(*Gtk::make_managed<Gtk::Separator>());

    menu_item(*profiles_popover_, *box, "Save as a new profile…", [this, active] {
        ask_text("New profile", active.empty() ? "" : active + " copy",
                 "A copy of your setup as it is now. You carry on in the new one.", "Save",
                 [this](const std::string& name) { call([&] { client_.save_profile_as(name); }); });
    });
    menu_item(*profiles_popover_, *box, "Rename…", [this, active] {
        ask_text("Rename profile", active, "", "Rename",
                 [this, active](const std::string& name) { call([&] { client_.rename_profile(active, name); }); });
    });
    menu_item(*profiles_popover_, *box, "Export…", [this] { export_profile(); });
    menu_item(*profiles_popover_, *box, "Import…", [this] { import_profile(); });
    box->append(*Gtk::make_managed<Gtk::Separator>());
    menu_item(
        *profiles_popover_, *box, "Delete profile…",
        [this, active] {
            confirm("Delete profile", "Delete “" + active + "”?",
                    "WaveMix switches to another profile first. Export it before if you may want it back.",
                    "Delete", [this, active] { call([&] { client_.delete_profile(active); }); });
        },
        names.size() > 1, true);
}

// Gets the profile in use's export text from the daemon, asks where to save it, and
// writes it there.
void MixerWindow::export_profile()
{
    const std::string active = str(state_.value("profiles", json::object()).value("active", json("")));
    std::string text;
    try {
        text = client_.export_profile(active);
    } catch (const client::ClientError& e) {
        show_error(e.what());
        return;
    }
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Export profile");
    dialog->set_initial_name(active + ".wavemix-profile.json");
    dialog->save(*this, [this, dialog, text, active](const Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try {
            file = dialog->save_finish(result);
        } catch (const Glib::Error&) {
            return;  // cancelled
        }
        try {
            std::string etag;
            file->replace_contents(text, "", etag);
            show_message("Exported “" + active + "” as " + file->get_parse_name() + ".", 6);
        } catch (const Glib::Error& e) {
            show_error("Could not export the profile: " + std::string(e.what()));
        }
    });
}

// Asks for a profile file, has the daemon import it and says what it was named.
void MixerWindow::import_profile()
{
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import profile");
    auto filter = Gtk::FileFilter::create();
    filter->set_name("WaveMix profiles");
    filter->add_pattern("*.json");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    dialog->set_filters(filters);
    dialog->open(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        Glib::RefPtr<Gio::File> file;
        try {
            file = dialog->open_finish(result);
        } catch (const Glib::Error&) {
            return;  // cancelled
        }
        std::string text;
        try {
            char* contents = nullptr;
            gsize length = 0;
            std::string etag;
            file->load_contents(contents, length, etag);
            text.assign(contents, length);
            g_free(contents);
        } catch (const Glib::Error& e) {
            show_error("Could not read the profile: " + std::string(e.what()));
            return;
        }
        std::string name;
        call([&] { name = client_.import_profile(text); });
        if (!name.empty())
            show_message("Imported “" + name + "”. Pick it in the profiles menu to use it.", 6);
    });
}

// The main menu: Preferences, OBS sources, bug report, About, Restart and Quit.
Gtk::MenuButton* MixerWindow::make_main_menu()
{
    // Everything that isn't mixing, in one menu: what's set once, the OBS names, help
    // when something's wrong, and the service.
    auto* button = Gtk::make_managed<Gtk::MenuButton>();
    button->set_icon_name("open-menu-symbolic");
    button->set_tooltip_text("Menu");
    button->set_has_frame(false);
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* box = popover_box(*popover);
    menu_item(*popover, *box, "Preferences…", [this] { open_preferences(); });
    menu_item(*popover, *box, "OBS sources…", [this] { open_obs_sources(); });
    menu_item(*popover, *box, "Save bug report…", [this] { save_bug_report(); });
    menu_item(*popover, *box, "About WaveMix", [this] { open_about(); });
    box->append(*Gtk::make_managed<Gtk::Separator>());
    menu_item(*popover, *box, "Restart service", [this] { restart_service(); });
    menu_item(*popover, *box, "Quit WaveMix", [this] { quit_service(); }, true, true);
    button->set_popover(*popover);
    return button;
}

// A dialog laid out as rows of "label  [widget]", with Close at the bottom. Returns
// the grid to add rows to; `add` adds one.
Gtk::Grid& MixerWindow::new_form(const std::string& title, Gtk::Widget* left)
{
    Gtk::Window& dialog = new_dialog(title);
    dialog.set_default_size(420, -1);
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(12);
    grid->set_column_spacing(16);
    grid->set_margin(18);

    auto* done = Gtk::make_managed<Gtk::Button>("Close");
    done->set_size_request(96, -1);
    done->signal_clicked().connect(sigc::mem_fun(*this, &MixerWindow::close_dialog));
    done->set_halign(Gtk::Align::END);
    auto* bottom = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    bottom->set_margin_start(18);
    bottom->set_margin_end(18);
    bottom->set_margin_bottom(16);
    done->set_hexpand(true);
    if (left)
        bottom->append(*left);
    bottom->append(*done);

    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    content->append(*grid);
    content->append(*bottom);
    dialog.set_child(*content);
    return *grid;
}

// Opens Preferences: start at login (and minimised), the VOD Mix, the mic, where the
// loopbacks run, the theme, and the Reset button.
void MixerWindow::open_preferences()
{
    const json& settings = state_.contains("settings") ? state_["settings"] : json::object();
    auto* reset = Gtk::make_managed<Gtk::Button>("Reset WaveMix");
    reset->add_css_class("destructive-action");
    reset->signal_clicked().connect(sigc::mem_fun(*this, &MixerWindow::reset_wavemix));
    Gtk::Grid& grid = new_form("Preferences", reset);
    int row = 0;

    auto* autostart = Gtk::make_managed<Gtk::CheckButton>("Start WaveMix when I log in");
    autostart->set_active(settings.value("autostart", true));
    auto* minimized = Gtk::make_managed<Gtk::CheckButton>("Start minimised (only the tray icon)");
    minimized->set_active(settings.value("start_minimized", true));
    minimized->set_sensitive(autostart->get_active());
    minimized->set_margin_start(24);  // it belongs to the one above
    autostart->signal_toggled().connect([this, autostart, minimized] {
        minimized->set_sensitive(autostart->get_active());
        call([&] { client_.set_setting("autostart", autostart->get_active()); });
    });
    minimized->signal_toggled().connect(
        [this, minimized] { call([&] { client_.set_setting("start_minimized", minimized->get_active()); }); });
    grid.attach(*autostart, 0, row++, 2);
    grid.attach(*minimized, 0, row++, 2);

    // The VOD Mix: a second mix for OBS's VOD track. Off by default; off, there's no
    // VOD Mix device at all, and no VOD buttons or row in the window.
    auto* vod = Gtk::make_managed<Gtk::CheckButton>("VOD mix (a second mix for OBS's VOD track)");
    vod->set_active(settings.value("vod_enabled", false));
    vod->set_margin_top(8);
    vod->signal_toggled().connect([this, vod] { call([&] { client_.set_setting("vod_enabled", vod->get_active()); }); });
    grid.attach(*vod, 0, row++, 2);

    // The mic: on by default. Off, there's no WaveMix Mic and no Mic column; this is
    // where it comes back. (Not offered while the service isn't connected to PipeWire.)
    if (state_.contains("mic")) {
        auto* mic = Gtk::make_managed<Gtk::CheckButton>("Enable mic");
        mic->set_active(state_["mic"].value("enabled", true));
        mic->set_margin_top(8);
        mic->signal_toggled().connect([this, mic] { call([&] { client_.set_mic_enabled(mic->get_active()); }); });
        grid.attach(*mic, 0, row++, 2);
    }

    // Where the channels' loopbacks run. Off (the default), in the service: if it stops,
    // what goes through WaveMix goes quiet. On, in the sound server, as before 0.1.7: the
    // channels keep playing without the service, at the cost of many more open files
    // there. The mic chain needs the service either way.
    auto* pulse = Gtk::make_managed<Gtk::CheckButton>("Keep the channels playing if WaveMix stops");
    pulse->set_active(settings.value("pulse_routing", false));
    pulse->set_margin_top(8);
    pulse->signal_toggled().connect(
        [this, pulse] { call([&] { client_.set_setting("pulse_routing", pulse->get_active()); }); });
    grid.attach(*pulse, 0, row++, 2);
    auto* pulse_note = make_label(
        "The sound server runs the channels' routing, so your apps carry on playing at their last levels. The "
        "mic still needs WaveMix, and the sound server uses many more open files.",
        {"wm-caption"}, 0.0F, true);
    pulse_note->set_margin_start(24);
    pulse_note->set_max_width_chars(52);
    grid.attach(*pulse_note, 0, row++, 2);

    // Fallbacks: used while the device picked in the window isn't there (unplugged, say).
    // Without one there, the output follows the system default; the mic waits for one.
    add_fallback_row(grid, row, "Fallback output", "fallback_output", state_.value("outputs", json::array()));
    if (state_.contains("mic"))
        add_fallback_row(grid, row, "Fallback mic", "fallback_input",
                         state_["mic"].value("inputs", json::array()));
    auto* fallback_note = make_label(
        "Used while the device you picked in the window isn't connected (not when it follows the system "
        "default). Without the fallback either, the output follows the system default, and the mic waits until "
        "one of them is back.",
        {"wm-caption"}, 0.0F, true);
    fallback_note->set_max_width_chars(52);
    grid.attach(*fallback_note, 0, row++, 2);

    add_theme_rows(grid, row);

    dialog_->present();
}

// Adds a fallback device row to a form: a picker of "No fallback" and the devices there
// now, which stores the choice in the `key` setting.
void MixerWindow::add_fallback_row(Gtk::Grid& grid, int& row, const std::string& title, const std::string& key,
                                   const json& devices)
{
    // No fallback, then the devices there now. A fallback that isn't connected stays listed, so
    // opening Preferences never changes it.
    const std::string current = str(state_.value("settings", json::object()).value(key, json("")));
    std::vector<std::string> names{""};
    std::vector<Glib::ustring> labels{"No fallback"};
    for (const auto& device : devices) {
        names.push_back(str(device.value("name", json(""))));
        labels.emplace_back(str(device.value("description", json(names.back()))));
    }
    if (std::find(names.begin(), names.end(), current) == names.end()) {
        names.push_back(current);
        labels.emplace_back(current + " (not connected)");
    }
    auto* label = make_label(title, {}, 0.0F);
    label->set_margin_top(8);
    auto* picker = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(labels));
    picker->set_selected(static_cast<guint>(std::find(names.begin(), names.end(), current) - names.begin()));
    picker->set_margin_top(8);
    picker->set_hexpand(true);
    picker->property_selected().signal_changed().connect([this, picker, names, key] {
        const auto i = picker->get_selected();
        if (i < names.size())
            call([&] { client_.set_setting(key, names[i]); });
    });
    grid.attach(*label, 0, row);
    grid.attach(*picker, 1, row++);
}

// Adds the Theme picker and the Custom CSS file chooser rows to a form.
void MixerWindow::add_theme_rows(Gtk::Grid& grid, int& row)
{
    const json& settings = state_.value("settings", json::object());

    // The built-in themes, then Custom: the user's own stylesheet, on top of Dark.
    // Custom is listed once there's a stylesheet to use. Switching to a built-in theme
    // leaves it unused but remembered, so Custom brings it straight back.
    auto* label = make_label("Theme", {}, 0.0F);
    label->set_margin_top(8);
    std::vector<std::string> ids;
    std::vector<Glib::ustring> names;
    for (const auto& theme : builtin_themes()) {
        ids.push_back(theme.id);
        names.push_back(theme.name);
    }
    const std::string current = str(settings.value("theme", json("dark")));
    if (current == kCustomTheme || !str(settings.value("custom_theme", json(""))).empty()) {
        ids.emplace_back(kCustomTheme);
        names.emplace_back("Custom");
    }
    const auto found = std::find(ids.begin(), ids.end(), current);
    auto list = Gtk::StringList::create(names);
    auto* picker = Gtk::make_managed<Gtk::DropDown>(list);
    picker->set_selected(found == ids.end() ? 0 : static_cast<guint>(found - ids.begin()));
    picker->set_margin_top(8);
    picker->set_hexpand(true);
    grid.attach(*label, 0, row);
    grid.attach(*picker, 1, row++);

    // The stylesheet: a few colours changed, or anything else.
    auto* custom_label = make_label("Custom CSS", {}, 0.0F);
    custom_label->set_valign(Gtk::Align::START);
    custom_label->set_margin_top(6);
    auto* custom = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* choose = Gtk::make_managed<Gtk::Button>("Choose…");
    choose->set_tooltip_text("Use a CSS file of your own");
    choose->set_halign(Gtk::Align::START);
    custom->append(*choose);
    custom->append(*make_label("Changes show as soon as the file is saved.", {"wm-caption"}, 0.0F, true));
    grid.attach(*custom_label, 0, row);
    grid.attach(*custom, 1, row++);

    // `ids` is shared with the picker's handler, since choosing a file can add Custom.
    auto shared_ids = std::make_shared<std::vector<std::string>>(std::move(ids));
    picker->property_selected().signal_changed().connect([this, picker, shared_ids] {
        const auto i = picker->get_selected();
        if (i < shared_ids->size())
            call([&] { client_.set_setting("theme", (*shared_ids)[i]); });
    });

    // A file chosen: it becomes Custom's, and Custom is listed (if it wasn't) and picked.
    // The file picker answers later: by then Preferences may be closed, and this does
    // nothing (the slot goes empty with the picker).
    const sigc::slot<void(const std::string&)> use = sigc::track_object(
        [this, picker, list, shared_ids](const std::string& path) {
            call([&] { client_.set_setting("custom_theme", path); });
            auto& ids = *shared_ids;
            auto found = std::find(ids.begin(), ids.end(), kCustomTheme);
            if (found == ids.end()) {
                ids.emplace_back(kCustomTheme);
                list->append("Custom");
                found = ids.end() - 1;
            }
            picker->set_selected(static_cast<guint>(found - ids.begin()));
        },
        *picker);

    choose->signal_clicked().connect([this, use] {
        auto filter = Gtk::FileFilter::create();
        filter->set_name("CSS files");
        filter->add_suffix("css");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        filters->append(filter);
        auto chooser = Gtk::FileDialog::create();
        chooser->set_title("Choose a CSS file");
        chooser->set_filters(filters);
        chooser->open(*dialog_, [chooser, use](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                if (auto picked = chooser->open_finish(result))
                    use(picked->get_path());
            } catch (const Glib::Error&) {
                // cancelled
            }
        });
    });
}

// Opens a dialog with the device names to pick in OBS for each mix and the mic.
void MixerWindow::open_obs_sources()
{
    Gtk::Grid& grid = new_form("OBS sources");
    int row = 0;
    const auto add = [&](const std::string& label, std::string_view device, const char* source) {
        auto* name = make_label(label, {}, 0.0F);
        auto* value = make_label(std::string(device), {"wm-stream-text"}, 0.0F);
        auto* kind = make_label(source, {"dim"}, 0.0F);
        grid.attach(*name, 0, row);
        grid.attach(*value, 1, row);
        grid.attach(*kind, 2, row);
        ++row;
    };
    add("Stream", core::kMixLabel, "Audio Output Capture");
    if (state_.value("settings", json::object()).value("vod_enabled", false))
        add("VOD track", core::kVodLabel, "Audio Output Capture");
    // The mic is a source, not a sink, so OBS lists it under its inputs.
    if (state_.contains("mic") && state_["mic"].value("enabled", true))
        add("Mic", core::kMicLabel, "Audio Input Capture");
    dialog_->present();
}

// Opens the About window: icon, name, version (and the service's, if it differs) and links.
void MixerWindow::open_about()
{
    // The icon, the name, who makes it and which version, then the links: like most
    // apps' About windows.
    Gtk::Window& dialog = new_dialog("About WaveMix");
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    content->set_size_request(340, -1);
    content->set_margin(24);
    content->set_margin_top(32);

    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(std::string(core::kAppId));
    icon->set_pixel_size(128);
    icon->set_margin_bottom(12);
    content->append(*icon);
    content->append(*make_label("WaveMix", {"wm-about-name"}));
    content->append(*make_label("ChromieVT", {"dim"}));

    auto* version = make_label(WAVEMIX_VERSION, {"wm-about-version"});
    version->set_halign(Gtk::Align::CENTER);
    version->set_margin_top(10);
    content->append(*version);
    // After an update the old service can still be running until it restarts.
    if (const std::string service = str(state_.value("version", json()));
        !service.empty() && service != WAVEMIX_VERSION) {
        auto* note = make_label("The service running is " + service + ": ☰ → Restart service updates it.",
                                {"wm-caption"});
        note->set_wrap(true);
        note->set_justify(Gtk::Justification::CENTER);
        content->append(*note);
    }

    auto* links = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    links->add_css_class("wm-about-links");
    links->set_margin_top(20);
    const std::string website(core::kWebsite);
    const auto add_link = [&](const std::string& text, const std::string& url) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto* name = make_label(text, {}, 0.0F);
        name->set_hexpand(true);
        row->append(*name);
        row->append(*make_label("↗", {"dim"}));
        auto* button = Gtk::make_managed<Gtk::Button>();
        button->set_child(*row);
        button->set_has_frame(false);
        button->set_tooltip_text(url);
        button->signal_clicked().connect([this, url] { open_link(url); });
        links->append(*button);
    };
    add_link("Website", website);
    add_link("Report a problem", website + "/issues/new/choose");
    content->append(*links);

    auto* licence = make_label("GPL-3.0-or-later", {"wm-caption"});
    licence->set_margin_top(12);
    content->append(*licence);

    dialog.set_child(*content);
    dialog.present();
}

// Opens a web page in the user's browser (through the portal, in the Flatpak).
void MixerWindow::open_link(const std::string& url)
{
    auto launcher = Gtk::UriLauncher::create(url);
    launcher->launch(*this, [this, launcher](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            launcher->launch_finish(result);
        } catch (const Glib::Error& e) {
            show_error("Could not open " + launcher->get_uri() + ": " + std::string(e.what()));
        }
    });
}

// Asks for confirmation, then resets WaveMix to the defaults (Cancel returns to Preferences).
void MixerWindow::reset_wavemix()
{
    // Asked from Preferences: Cancel goes back there.
    confirm("Reset WaveMix", "Reset WaveMix?",
            "All your preferences and configuration will be reset: your channels and the apps in them, the "
            "mic's filters, your levels, the output device and the theme go back to how a new install has "
            "them. This can't be undone.",
            "OK",
            [this] {
                try {
                    client_.reset();
                } catch (const client::ClientError& e) {
                    show_error(std::string("Couldn't reset WaveMix: ") + e.what());
                    return;
                }
                show_message("WaveMix has been reset.", 4);
                queue_refresh();
            },
            [this] { open_preferences(); });
}

// Asks the service to restart, and starts it again if it isn't back within 4 seconds.
void MixerWindow::restart_service()
{
    try {
        client_.restart();
    } catch (const client::ClientError& e) {
        show_error(std::string("Couldn't restart the WaveMix service: ") + e.what());
        return;
    }
    show_message("Restarting WaveMix…");
    // The service comes back by itself; if it hasn't within a few seconds, start it.
    Glib::signal_timeout().connect_seconds_once(
        [this] {
            if (!client_.running())
                client_.ensure_daemon(3.0);
            hide_message();
            queue_refresh();
        },
        4);
}

// Asks for confirmation, then quits the service (and with it this window).
void MixerWindow::quit_service()
{
    const bool pulse = state_.value("settings", json::object()).value("pulse_routing", false);
    confirm("Quit WaveMix", "Quit WaveMix?",
            pulse ? "Your channels keep playing, at their levels of the moment, but the mic goes quiet until "
                    "WaveMix runs again (next login, or when you reopen it). OBS keeps its devices."
                  : "Everything that goes through WaveMix goes quiet until it runs again (next login, or when you "
                    "reopen it): your apps stay in their channels, and OBS keeps its devices, just with no sound. "
                    "Apps you haven't routed keep playing as usual.",
            "Quit", [this] {
                try {
                    client_.quit();
                } catch (const client::ClientError&) {
                    // already gone
                }
                if (auto app = get_application())
                    app->quit();
            });
}

}  // namespace wavemix::gui
