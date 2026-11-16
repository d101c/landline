#include <gtest/gtest.h>
#include <tether/bluetooth/supervision_tick.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    struct Recorder : SupervisionSteps {
        std::vector<std::string> order;
        // Which of the slow steps report having held the thread.
        bool profiles_held = false;
        bool contacts_held = false;
        bool call_history_held = false;
        bool messages_held = false;
        bool reads_allowed = true;

        void links() override { order.push_back("links"); }
        void notifications() override { order.push_back("notifications"); }
        void calls() override { order.push_back("calls"); }
        void publish() override { order.push_back("publish"); }
        bool profiles() override {
            order.push_back("profiles");
            return profiles_held;
        }
        void presence() override { order.push_back("presence"); }
        bool long_reads_allowed() override { return reads_allowed; }
        bool contacts() override {
            order.push_back("contacts");
            return contacts_held;
        }
        bool call_history() override {
            order.push_back("call_history");
            return call_history_held;
        }
        bool messages() override {
            order.push_back("messages");
            return messages_held;
        }

        size_t first(const std::string& step) const {
            return static_cast<size_t>(std::find(order.begin(), order.end(), step) - order.begin());
        }
        size_t count(const std::string& step) const {
            return static_cast<size_t>(std::count(order.begin(), order.end(), step));
        }
        // The steps that ran after the first occurrence of `step`.
        std::vector<std::string> after(const std::string& step) const {
            const size_t at = first(step);
            if (at >= order.size())
                return {};
            return {order.begin() + static_cast<long>(at) + 1, order.end()};
        }
    };

} // namespace

// Everything runs on one thread. Opening an OBEX session and reading the
// address book or the mailbox can each take tens of seconds, and notifications
// used to wait behind all of them: +47 s after a link loss, measured.
TEST(SupervisionTick, NotificationsAndCallsComeBeforeAnythingThatWaitsOnObex) {
    Recorder tick;
    run_supervision_tick(tick);

    for (const char* fast : {"notifications", "calls"})
        for (const char* slow : {"profiles", "contacts", "call_history", "messages"})
            EXPECT_LT(tick.first(fast), tick.first(slow)) << fast << " must run before " << slow;
}

TEST(SupervisionTick, LinksAreSupervisedFirst) {
    Recorder tick;
    run_supervision_tick(tick);
    ASSERT_FALSE(tick.order.empty());
    EXPECT_EQ(tick.order.front(), "links");
}

// Clients learn that notifications and calls are back without waiting for the
// OBEX sessions, and again once those have been dealt with.
TEST(SupervisionTick, PublishesBeforeAndAfterTheObexSessions) {
    Recorder tick;
    run_supervision_tick(tick);

    EXPECT_EQ(std::count(tick.order.begin(), tick.order.end(), "publish"), 2);
    const size_t early = tick.first("publish");
    EXPECT_GT(early, tick.first("calls"));
    EXPECT_LT(early, tick.first("profiles"));

    const auto late = std::find(tick.order.rbegin(), tick.order.rend(), "publish");
    const size_t late_index = static_cast<size_t>(tick.order.rend() - late) - 1;
    EXPECT_GT(late_index, tick.first("profiles"));
    EXPECT_LT(late_index, tick.first("contacts"));
}

// A tick in which nothing held the thread does each thing once: the quick
// steps are not free (BlueZ and PipeWire are asked each time).
TEST(SupervisionTick, AQuietTickRunsEveryStepOnceApartFromPublish) {
    Recorder tick;
    run_supervision_tick(tick);
    for (const char* step :
         {"links", "notifications", "calls", "profiles", "presence", "contacts", "call_history", "messages"})
        EXPECT_EQ(tick.count(step), 1u) << step;
}

// 16 November trial: obexd brought BR/EDR up at +6.5 s and subscribers were
// told at +11.4 s, after the address book. The links are read again as soon
// as the sessions have been dealt with, and published before any long read.
TEST(SupervisionTick, LinksAreReadAgainAfterTheSessionsOpened) {
    Recorder tick;
    tick.profiles_held = true;
    run_supervision_tick(tick);

    const auto rest = tick.after("profiles");
    const auto links = std::find(rest.begin(), rest.end(), "links");
    const auto publish = std::find(rest.begin(), rest.end(), "publish");
    const auto contacts = std::find(rest.begin(), rest.end(), "contacts");
    ASSERT_NE(links, rest.end());
    ASSERT_NE(publish, rest.end());
    EXPECT_LT(links, publish) << "the status published after the sessions is built from fresh links";
    EXPECT_LT(publish, contacts);
}

// And after each read that held the thread, before the next one starts.
TEST(SupervisionTick, QuickStepsRunAgainAfterEachLongRead) {
    Recorder tick;
    tick.contacts_held = true;
    tick.call_history_held = true;
    tick.messages_held = true;
    run_supervision_tick(tick);

    const std::vector<std::string> quick{"links", "notifications", "calls", "publish"};
    for (const char* slow : {"contacts", "call_history", "messages"}) {
        const auto rest = tick.after(slow);
        ASSERT_GE(rest.size(), quick.size()) << slow;
        EXPECT_EQ(std::vector<std::string>(rest.begin(), rest.begin() + 4), quick) << "right after " << slow;
    }
    EXPECT_EQ(tick.count("notifications"), 4u);
    EXPECT_EQ(tick.count("contacts"), 1u);
}

TEST(SupervisionTick, OnlyTheReadThatHeldTheThreadIsFollowedByQuickSteps) {
    Recorder tick;
    tick.call_history_held = true;
    run_supervision_tick(tick);

    EXPECT_EQ(tick.count("notifications"), 2u);
    const auto rest = tick.after("contacts");
    ASSERT_FALSE(rest.empty());
    EXPECT_EQ(rest.front(), "call_history") << "nothing was read, nothing to catch up on";
}

// 16 November trial: notifications ran first in the tick and still came back
// behind the address book, +9.7 s. Their session is rebuilt over several
// ticks, and the first of those was followed by a five-second read.
TEST(SupervisionTick, LongReadsAreSkippedWhileNotificationsComeBack) {
    Recorder tick;
    tick.reads_allowed = false;
    run_supervision_tick(tick);

    for (const char* slow : {"contacts", "call_history", "messages"})
        EXPECT_EQ(std::count(tick.order.begin(), tick.order.end(), slow), 0) << slow;
    for (const char* kept : {"links", "notifications", "calls", "profiles", "presence"})
        EXPECT_EQ(std::count(tick.order.begin(), tick.order.end(), kept), 1) << kept;
    EXPECT_EQ(tick.order.back(), "publish");
}
