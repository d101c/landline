#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/client.hpp>
#include <tether/bluetooth/connection.hpp>
#include <tether/bluetooth/reconnect_policy.hpp>

using namespace tether::bluetooth;

namespace {

    nlohmann::json payload(bool map_open, bool pbap_open) {
        ProfileStatus profiles;
        profiles.map_open = map_open;
        profiles.pbap_open = pbap_open;
        profiles.map_error = map_open ? ObexError::None : ObexError::Unavailable;
        profiles.pbap_error = pbap_open ? ObexError::None : ObexError::Unavailable;
        profiles.reason = map_open && pbap_open ? "connected" : "unreachable";
        return to_json(BearerStatus{}, profiles);
    }

    PlannedDrop drop_at(int64_t now) {
        PlannedDrop drop;
        drop.until = now + HFP_CYCLE_MASK_SECONDS;
        drop.map = true;
        drop.pbap = true;
        drop.reason = "connected";
        return drop;
    }

} // namespace

// 16 November trial: the BR/EDR link was cut on purpose for hands-free, and
// the subscriber saw messages and contacts go down and come back.
TEST(PlannedDrop, SessionsTakenDownOnPurposeStayPublishedAsOpen) {
    const PlannedDrop drop = drop_at(100);
    nlohmann::json status = payload(false, false);

    EXPECT_TRUE(mask_planned_drop(status, drop, 101));
    EXPECT_TRUE(status["map_open"].get<bool>());
    EXPECT_TRUE(status["pbap_open"].get<bool>());
    EXPECT_EQ(status["map_error"], "none");
    EXPECT_EQ(status["pbap_error"], "none");
    EXPECT_EQ(status["profile_reason"], "connected");
}

TEST(PlannedDrop, CoversNothingOnceTheTimeIsUp) {
    const PlannedDrop drop = drop_at(100);
    nlohmann::json status = payload(false, false);

    EXPECT_FALSE(mask_planned_drop(status, drop, 100 + HFP_CYCLE_MASK_SECONDS));
    EXPECT_FALSE(status["map_open"].get<bool>());
    EXPECT_EQ(status["profile_reason"], "unreachable");
}

TEST(PlannedDrop, CoversOnlyWhatWasOpenBeforeTheDrop) {
    PlannedDrop drop = drop_at(100);
    drop.pbap = false;
    nlohmann::json status = payload(false, false);

    EXPECT_TRUE(mask_planned_drop(status, drop, 101));
    EXPECT_TRUE(status["map_open"].get<bool>());
    EXPECT_FALSE(status["pbap_open"].get<bool>());
    EXPECT_EQ(status["pbap_error"], "unavailable");
}

TEST(PlannedDrop, LeavesAProfileReleasedOnPurposeAlone) {
    const PlannedDrop drop = drop_at(100);
    nlohmann::json status = payload(false, true);
    status["map_held"] = true;

    EXPECT_FALSE(mask_planned_drop(status, drop, 101));
    EXPECT_FALSE(status["map_open"].get<bool>());
}

TEST(PlannedDrop, NoDropNoMask) {
    nlohmann::json status = payload(false, false);
    EXPECT_FALSE(mask_planned_drop(status, PlannedDrop{}, 5));
    EXPECT_FALSE(status["map_open"].get<bool>());
}

// The cover ends as soon as the sessions are back, so a loss that follows is
// published for what it is.
TEST(PlannedDrop, EndsWhenTheSessionsAreBack) {
    PlannedDrop drop = drop_at(100);

    drop.observe(true, true, 100); // still open: the drop has not reached them yet
    EXPECT_TRUE(drop.active(100));

    drop.observe(false, false, 101);
    EXPECT_TRUE(drop.active(101));
    drop.observe(true, false, 103);
    EXPECT_TRUE(drop.active(103));

    drop.observe(true, true, 105);
    EXPECT_FALSE(drop.active(105));

    nlohmann::json status = payload(false, false);
    EXPECT_FALSE(mask_planned_drop(status, drop, 106));
}

TEST(PlannedDrop, EndsWhenTheTimeIsUpEvenIfNothingCameBack) {
    PlannedDrop drop = drop_at(100);
    drop.observe(false, false, 101);
    drop.observe(false, false, 100 + HFP_CYCLE_MASK_SECONDS);
    EXPECT_EQ(drop.until, -1);
}

// The address book: 767 entries took five seconds of the supervising thread on
// every new PBAP session, twice within twenty seconds around the hands-free
// cycle.
TEST(ContactsPlan, ReadsAtOnceWhenThereIsNothingToKeep) {
    EXPECT_EQ(plan_contacts(false, 10, 768, 768), ContactsPlan::PullNow) << "no book in memory";
    EXPECT_EQ(plan_contacts(true, -1, -1, 768), ContactsPlan::PullNow) << "a book from disk, never read by this run";
}

TEST(ContactsPlan, KeepsAFreshBookWhenThePhoneCountIsUnchanged) {
    EXPECT_EQ(plan_contacts(true, 19, 768, 768), ContactsPlan::Keep);
    EXPECT_EQ(plan_contacts(true, CONTACTS_FRESH_SECONDS - 1, 768, 768), ContactsPlan::Keep);
}

TEST(ContactsPlan, ReadsAtOnceWhenThePhoneCountMoved) {
    EXPECT_EQ(plan_contacts(true, 19, 768, 769), ContactsPlan::PullNow);
    EXPECT_EQ(plan_contacts(true, 40000, 768, 760), ContactsPlan::PullNow);
}

TEST(ContactsPlan, PutsOffAnOlderBookInsteadOfReadingItDuringTheReconnection) {
    EXPECT_EQ(plan_contacts(true, CONTACTS_FRESH_SECONDS, 768, 768), ContactsPlan::PullLater);
    EXPECT_EQ(plan_contacts(true, 86400, 768, 768), ContactsPlan::PullLater);
}

// A phone that does not report its count cannot say nothing changed.
TEST(ContactsPlan, WithoutACountEvenAFreshBookIsReadAgainLater) {
    EXPECT_EQ(plan_contacts(true, 19, 768, -1), ContactsPlan::PullLater);
    EXPECT_EQ(plan_contacts(true, 19, -1, 768), ContactsPlan::PullLater);
    EXPECT_EQ(plan_contacts(true, 86400, -1, -1), ContactsPlan::PullLater);
}

TEST(ContactsPlan, EverySessionCanBeMadeToReadAgain) {
    EXPECT_FALSE(contacts_always_pulled(nullptr));
    EXPECT_FALSE(contacts_always_pulled("0"));
    EXPECT_TRUE(contacts_always_pulled("1"));
}

// Long reads against the notification session, in the two trials.
TEST(NotificationPriority, ALostSessionHoldsTheLongReadsUntilItIsBack) {
    NotificationPriority priority;
    EXPECT_FALSE(priority.hold(true, true, true, 100));

    // Cut from the computer: the session and the LE link fall together.
    EXPECT_TRUE(priority.hold(true, false, false, 101));
    // LE is back within the second; the session takes a few more ticks.
    EXPECT_TRUE(priority.hold(true, false, true, 102));
    EXPECT_TRUE(priority.hold(true, false, true, 105));
    EXPECT_FALSE(priority.hold(true, true, true, 106)) << "ready: the reads may run";
}

TEST(NotificationPriority, AReturnAfterALongAbsenceHoldsThemAgain) {
    NotificationPriority priority;
    priority.hold(true, true, true, 0);
    EXPECT_TRUE(priority.hold(true, false, false, 1));
    EXPECT_FALSE(priority.hold(true, false, false, 1 + ANCS_FIRST_SECONDS)) << "nobody to wait for";
    EXPECT_FALSE(priority.hold(true, false, false, 55));

    EXPECT_TRUE(priority.hold(true, false, true, 56)) << "LE is back: notifications first";
    EXPECT_TRUE(priority.hold(true, false, true, 58));
    EXPECT_FALSE(priority.hold(true, true, true, 59));
}

TEST(NotificationPriority, TheWaitIsBounded) {
    NotificationPriority priority;
    priority.hold(true, true, true, 0);
    EXPECT_TRUE(priority.hold(true, false, true, 10));
    EXPECT_TRUE(priority.hold(true, false, true, 10 + ANCS_FIRST_SECONDS - 1));
    EXPECT_FALSE(priority.hold(true, false, true, 10 + ANCS_FIRST_SECONDS));
}

// Notification sharing switched off on the phone: the session never becomes
// ready, and the LE bearer flaps. The mailbox must not wait behind every flap.
TEST(NotificationPriority, APhoneThatNeverBecomesReadyGetsOneWaitOnly) {
    NotificationPriority priority;
    EXPECT_TRUE(priority.hold(true, false, true, 0)) << "at start, once";
    EXPECT_FALSE(priority.hold(true, false, true, ANCS_FIRST_SECONDS));

    for (int64_t now = 20; now < 200; now += 4) {
        EXPECT_FALSE(priority.hold(true, false, false, now));
        EXPECT_FALSE(priority.hold(true, false, true, now + 2));
    }
}

TEST(NotificationPriority, NothingWaitsWhenNotificationsAreOffOrTheWaitIsZero) {
    NotificationPriority off;
    EXPECT_FALSE(off.hold(false, false, true, 0));
    EXPECT_FALSE(off.hold(false, false, true, 1));

    NotificationPriority zero;
    zero.hold(true, true, true, 0, 0);
    EXPECT_FALSE(zero.hold(true, false, true, 1, 0));
}

TEST(NotificationPriority, TheWaitCanBeSetFromTheEnvironment) {
    EXPECT_EQ(ancs_first_seconds_from(nullptr, 12), 12);
    EXPECT_EQ(ancs_first_seconds_from("", 12), 12);
    EXPECT_EQ(ancs_first_seconds_from("0", 12), 0);
    EXPECT_EQ(ancs_first_seconds_from("30", 12), 30);
    EXPECT_EQ(ancs_first_seconds_from("120", 12), 120);
    EXPECT_EQ(ancs_first_seconds_from("121", 12), 12);
    EXPECT_EQ(ancs_first_seconds_from("99999999999", 12), 12);
    EXPECT_EQ(ancs_first_seconds_from("12s", 12), 12);
    EXPECT_EQ(ancs_first_seconds_from("-3", 12), 12);
}

// The first attempt after a drop comes before the LE link is back; the second
// was five seconds away, which was most of the +9.7 s.
TEST(AncsRetry, IsEverySecondWhileASessionIsRebuilt) {
    using namespace tether::bluetooth::ancs;
    EXPECT_EQ(ancs_retry_seconds(100, 100), ANCS_SETTLE_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100 + ANCS_SETTLE_SECONDS - 1, 100), ANCS_SETTLE_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100 + ANCS_SETTLE_SECONDS, 100), ANCS_WATCH_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100 + ANCS_WATCH_SECONDS - 1, 100), ANCS_WATCH_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100 + ANCS_WATCH_SECONDS, 100), ANCS_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100, -1), ANCS_RETRY_SECONDS);
    EXPECT_EQ(ancs_retry_seconds(100, 100, false), ANCS_RETRY_SECONDS) << "switched off";
}
