// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ChromieVT
//
// Notifications: raising, replacing, muting, expiring, and how GetState lists them.

#include <gtest/gtest.h>

#include "core/notifications.hpp"

using namespace wavemix;
using core::json;

namespace {

// A question (with a button) of `kind`, with this id.
core::Notification question(const std::string& id, const std::string& kind = "test")
{
    return {id, kind, core::NotificationLevel::Warning, "Title " + id, "Body", {{"ok", "OK"}}};
}

// A piece of information (no buttons) of `kind`, with this id.
core::Notification info(const std::string& id, const std::string& kind = "info")
{
    return {id, kind, core::NotificationLevel::Info, "Title " + id, "", {}};
}

}  // namespace

// Notifications are listed oldest first; raising an id again replaces it, as the newest.
TEST(Notifications, RaisingTheSameIdReplacesIt)
{
    core::Notifications notifications;
    EXPECT_TRUE(notifications.raise(question("a"), {}));
    EXPECT_TRUE(notifications.raise(question("b"), {}));
    auto again = question("a");
    again.title = "Changed";
    EXPECT_TRUE(notifications.raise(again, {}));
    const auto all = notifications.all();
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0].id, "b");
    EXPECT_EQ(all[1].id, "a");
    EXPECT_EQ(all[1].title, "Changed");
}

// A muted kind is never raised, and muting a kind takes its notifications away.
TEST(Notifications, MutedKindsAreNotShown)
{
    core::Notifications notifications;
    EXPECT_FALSE(notifications.raise(question("a", "app-moved"), {"app-moved"}));
    EXPECT_TRUE(notifications.all().empty());
    notifications.raise(question("b", "app-moved"), {});
    notifications.raise(question("c", "test"), {});
    EXPECT_EQ(notifications.dismiss_kind("app-moved"), std::vector<std::string>{"b"});
    ASSERT_EQ(notifications.all().size(), 1U);
    EXPECT_EQ(notifications.all()[0].id, "c");
}

// Information goes after kInfoSeconds; questions stay until answered or dismissed.
TEST(Notifications, InformationExpiresQuestionsStay)
{
    core::Notifications notifications;
    const auto start = core::Notifications::Clock::now();
    notifications.raise(info("i"), {}, start);
    notifications.raise(question("q"), {}, start);
    EXPECT_TRUE(notifications.expire(start + core::kInfoSeconds - std::chrono::seconds(1)).empty());
    EXPECT_EQ(notifications.expire(start + core::kInfoSeconds), std::vector<std::string>{"i"});
    ASSERT_NE(notifications.find("q"), nullptr);
    EXPECT_TRUE(notifications.dismiss("q"));
    EXPECT_FALSE(notifications.dismiss("q"));
    EXPECT_EQ(notifications.find("q"), nullptr);
}

// GetState lists each notification with its level spelled out and its buttons.
TEST(Notifications, GetStateListsThem)
{
    core::Notifications notifications;
    notifications.raise(question("q", "app-moved"), {});
    const json listed = notifications.to_json();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed[0]["id"], "q");
    EXPECT_EQ(listed[0]["kind"], "app-moved");
    EXPECT_EQ(listed[0]["level"], "warning");
    EXPECT_EQ(listed[0]["title"], "Title q");
    EXPECT_EQ(listed[0]["actions"], json::parse(R"([{"id": "ok", "label": "OK"}])"));
    EXPECT_EQ(listed[0]["desktop_only"], false);
}

// The muted kinds come from settings; anything but a list of strings counts as none.
TEST(Notifications, MutedKindsFromTheSettings)
{
    EXPECT_TRUE(core::muted_notification_kinds(json::object()).empty());
    EXPECT_TRUE(
        core::muted_notification_kinds(json::parse(R"({"settings": {"muted_notifications": "test"}})")).empty());
    EXPECT_EQ(core::muted_notification_kinds(json::parse(R"({"settings": {"muted_notifications": ["test", 3]}})")),
              std::vector<std::string>{"test"});
}
