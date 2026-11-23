#include <gtest/gtest.h>
#include <tether/bluetooth/message_sync_policy.hpp>

#include <vector>

using namespace tether::bluetooth;

namespace {

    // A session listed at `last`, notifications working.
    MessageSyncFacts quiet(int64_t now, int64_t last) {
        MessageSyncFacts facts;
        facts.now = now;
        facts.last_poll_at = last;
        facts.ancs_ready = true;
        facts.ancs_content = true;
        return facts;
    }

} // namespace

TEST(MessageSyncPolicy, ANewSessionIsListedAtOnce) {
    MessageSyncFacts facts = quiet(100, -1);
    facts.session_new = true;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::PollNow);
}

TEST(MessageSyncPolicy, ARequestWaitsTwoSecondsThenLists) {
    MessageSyncFacts facts = quiet(1000, 990);
    facts.requested_at = 1000;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::Wait);
    facts.now = 1001;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::Wait);
    facts.now = 1002;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::PollNow);
}

TEST(MessageSyncPolicy, ABurstOfFiveRequestsIsOneListing) {
    MessageSyncRequest request;
    for (int64_t t = 1000; t < 1005; ++t)
        request.ask(t, "ancs");
    EXPECT_EQ(request.requested_at, 1000);
    EXPECT_EQ(request.attempts, 0);

    int listings = 0;
    int64_t last = 990;
    for (int64_t now = 1000; now < 1030; ++now) {
        MessageSyncFacts facts = quiet(now, last);
        facts.requested_at = request.requested_at;
        facts.request_attempts = request.attempts;
        if (plan_message_sync(facts) == MessageSyncPlan::PollNow) {
            ++listings;
            last = now;
            request.listed(1);
        }
    }
    EXPECT_EQ(listings, 1);
    EXPECT_FALSE(request.pending());
}

TEST(MessageSyncPolicy, AListingWithNothingNewIsRetriedTwiceThenGivenUp) {
    MessageSyncRequest request;
    request.ask(1000, "ancs");
    std::vector<int64_t> listed_at;
    int64_t last = 990;
    for (int64_t now = 1000; now < 1100; ++now) {
        MessageSyncFacts facts = quiet(now, last);
        facts.requested_at = request.requested_at;
        facts.request_attempts = request.attempts;
        if (plan_message_sync(facts) == MessageSyncPlan::PollNow) {
            listed_at.push_back(now);
            last = now;
            request.listed(0);
        }
    }
    // +2 s, then 3 s later, then 10 s later, then nothing.
    EXPECT_EQ(listed_at, (std::vector<int64_t>{1002, 1005, 1015}));
    EXPECT_FALSE(request.pending());
}

TEST(MessageSyncPolicy, ARetryThatBringsTheMessageSettlesTheRequest) {
    MessageSyncRequest request;
    request.ask(1000, "ancs");
    request.listed(0);
    EXPECT_TRUE(request.pending());
    EXPECT_EQ(request.attempts, 1);
    request.listed(2);
    EXPECT_FALSE(request.pending());
}

TEST(MessageSyncPolicy, ARequestDuringTheRetriesStartsOver) {
    MessageSyncRequest request;
    request.ask(1000, "ancs");
    request.listed(0);
    request.ask(1004, "ancs");
    EXPECT_EQ(request.requested_at, 1004);
    EXPECT_EQ(request.attempts, 0);
}

TEST(MessageSyncPolicy, ARemovalIsSettledByOneListing) {
    MessageSyncRequest request;
    request.ask(1000, "ancs_removed", false);
    request.listed(0);
    EXPECT_FALSE(request.pending());
}

TEST(MessageSyncPolicy, AnArrivalDuringARemovalRequestStillExpectsAMessage) {
    MessageSyncRequest request;
    request.ask(1000, "ancs_removed", false);
    request.ask(1001, "ancs");
    request.listed(0);
    EXPECT_TRUE(request.pending());
}

TEST(MessageSyncPolicy, ARequestNoSessionServedExpires) {
    MessageSyncRequest request;
    request.ask(1000, "ancs");
    request.expire(1000 + MESSAGE_SYNC_EXPIRY_SECONDS - 1);
    EXPECT_TRUE(request.pending());
    request.expire(1000 + MESSAGE_SYNC_EXPIRY_SECONDS);
    EXPECT_FALSE(request.pending());
}

TEST(MessageSyncPolicy, WithoutNotificationsTheInboxIsListedEveryFifteenSeconds) {
    MessageSyncFacts facts = quiet(1000, 990);
    facts.ancs_ready = false;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::Wait);
    facts.now = 990 + MESSAGE_FALLBACK_POLL_SECONDS;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::PollNow);
}

TEST(MessageSyncPolicy, NotificationsWithoutTextAreNotReliedOn) {
    MessageSyncFacts facts = quiet(990 + MESSAGE_FALLBACK_POLL_SECONDS, 990);
    facts.ancs_content = false;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::PollNow);
}

TEST(MessageSyncPolicy, WithNotificationsTheInboxIsListedOnceAnHour) {
    MessageSyncFacts facts = quiet(990 + 3599, 990);
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::Wait);
    facts.now = 990 + MESSAGE_BACKGROUND_POLL_SECONDS;
    EXPECT_EQ(plan_message_sync(facts), MessageSyncPlan::PollNow);
}

TEST(MessageSyncPolicy, TheEnvironmentVariableReplacesTheBackgroundInterval) {
    EXPECT_EQ(message_poll_override_from(nullptr), 0);
    EXPECT_EQ(message_poll_override_from(""), 0);
    EXPECT_EQ(message_poll_override_from("15"), 15);
    EXPECT_EQ(message_poll_override_from("3600"), 3600);
    EXPECT_EQ(message_poll_override_from("4"), 0);
    EXPECT_EQ(message_poll_override_from("3601"), 0);
    EXPECT_EQ(message_poll_override_from("15s"), 0);

    MessageSyncConfig config;
    config.background_override = 15;
    MessageSyncFacts facts = quiet(1004, 990);
    EXPECT_EQ(plan_message_sync(facts, config), MessageSyncPlan::Wait);
    facts.now = 1005;
    EXPECT_EQ(plan_message_sync(facts, config), MessageSyncPlan::PollNow);

    // And in the other direction: without notifications, an hour if asked.
    config.background_override = 3600;
    facts = quiet(1100, 990);
    facts.ancs_ready = false;
    EXPECT_EQ(plan_message_sync(facts, config), MessageSyncPlan::Wait);
}

TEST(AncsSyncTriggers, AMessagesNotificationAsksForTheInbox) {
    AncsSyncTriggers triggers;
    const auto ask =
        triggers.on_notification(7, ancs::APP_ID_MESSAGES, ancs::CategoryId::Social, false, 1);
    EXPECT_EQ(ask.reason, "ancs");
    EXPECT_FALSE(ask.calls_changed);
}

TEST(AncsSyncTriggers, AnotherAppAsksForNothing) {
    AncsSyncTriggers triggers;
    const auto ask = triggers.on_notification(7, "com.example.app", ancs::CategoryId::Social, false, 1);
    EXPECT_TRUE(ask.reason.empty());
    EXPECT_TRUE(triggers.on_removed(7).reason.empty());
}

TEST(AncsSyncTriggers, TheBacklogAsksOncePerNotificationSession) {
    AncsSyncTriggers triggers;
    EXPECT_EQ(triggers.on_notification(1, ancs::APP_ID_MESSAGES, ancs::CategoryId::Social, true, 4).reason,
              "ancs_backlog");
    EXPECT_TRUE(triggers.on_notification(2, ancs::APP_ID_MESSAGES, ancs::CategoryId::Social, true, 4).reason.empty());
    EXPECT_EQ(triggers.on_notification(3, ancs::APP_ID_MESSAGES, ancs::CategoryId::Social, true, 5).reason,
              "ancs_backlog");
}

TEST(AncsSyncTriggers, ARemovedMessagesNotificationAsksForTheState) {
    AncsSyncTriggers triggers;
    triggers.on_notification(9, ancs::APP_ID_MESSAGES, ancs::CategoryId::Social, false, 1);
    EXPECT_EQ(triggers.on_removed(9).reason, "ancs_removed");
    // Only once.
    EXPECT_TRUE(triggers.on_removed(9).reason.empty());
}

TEST(AncsSyncTriggers, ACallNotificationMarksTheCallHistory) {
    AncsSyncTriggers triggers;
    EXPECT_TRUE(
        triggers.on_notification(1, "com.apple.mobilephone", ancs::CategoryId::IncomingCall, false, 1).calls_changed);
    EXPECT_TRUE(
        triggers.on_notification(2, "com.apple.mobilephone", ancs::CategoryId::MissedCall, false, 1).calls_changed);
    // What was already on the phone when the session came up is old news.
    EXPECT_FALSE(
        triggers.on_notification(3, "com.apple.mobilephone", ancs::CategoryId::MissedCall, true, 1).calls_changed);
    EXPECT_FALSE(triggers.on_notification(4, "com.example.app", ancs::CategoryId::Social, false, 1).calls_changed);
}
