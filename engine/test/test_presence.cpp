#include <gtest/gtest.h>

#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/presence.hpp>

using namespace tether::bluetooth;

namespace {

    constexpr const char* REMOTE = "org.bluez.Reason.Remote";
    constexpr const char* SUSPEND = "org.bluez.Reason.Suspend";
    constexpr const char* LOCAL = "org.bluez.Reason.Local";

    PresenceInput here(int64_t now) {
        PresenceInput in;
        in.now = now;
        in.bearer.device_present = true;
        in.bearer.device_paired = true;
        in.bearer.classic_connected = true;
        in.bearer.le_connected = true;
        return in;
    }

    PresenceInput gone(int64_t now, const char* reason, bool lock = false, int grace = 30) {
        PresenceInput in;
        in.now = now;
        in.bearer.device_present = true;
        in.bearer.device_paired = true;
        in.disconnect_reason = reason;
        in.lock_enabled = lock;
        in.grace_seconds = grace;
        return in;
    }

    // Runs the tracker one second at a time and keeps what it published.
    struct Walk {
        PresenceTracker tracker;
        std::vector<nlohmann::json> events;
        int locks = 0;
        int wakes = 0;
        bool lock_succeeds = true;

        void feed(const PresenceInput& in) {
            const PresenceStep step = tracker.step(in);
            if (step.lock_now) {
                ++locks;
                tracker.lock_result(lock_succeeds);
            }
            if (step.wake_now)
                ++wakes;
            if (step.changed || step.lock_now)
                events.push_back(tracker.to_json());
        }

        template <typename Make>
        void span(int64_t from, int64_t to, Make make) {
            for (int64_t now = from; now <= to; ++now)
                feed(make(now));
        }
    };

} // namespace

TEST(Presence, IsUnknownUntilThePhoneIsReachedOnce) {
    Walk run;
    run.span(0, 120, [](int64_t now) { return gone(now, REASON_TIMEOUT, true); });
    EXPECT_TRUE(run.events.empty()) << "a phone never seen since the daemon started has not left";
    EXPECT_EQ(run.locks, 0);
    EXPECT_EQ(run.tracker.to_json()["state"], "unknown");
}

TEST(Presence, SaysHereOnce) {
    Walk run;
    run.span(0, 10, here);
    ASSERT_EQ(run.events.size(), 1u);
    EXPECT_EQ(run.events[0]["command"], "bt_presence");
    EXPECT_EQ(run.events[0]["state"], "here");
    EXPECT_EQ(run.events[0]["reason"], "");
    EXPECT_EQ(run.events[0]["lock"], "none");
    EXPECT_EQ(run.events[0]["wake"], false);
}

TEST(Presence, AFlapShorterThanTheConfirmationIsNotAnAbsence) {
    Walk run;
    run.feed(here(0));
    run.span(1, PRESENCE_CONFIRM_SECONDS, [](int64_t now) { return gone(now, REASON_TIMEOUT); });
    run.feed(here(PRESENCE_CONFIRM_SECONDS + 1));
    EXPECT_EQ(run.events.size(), 1u);
}

TEST(Presence, WalkingAwayIsOutOfRange) {
    Walk run;
    run.feed(here(0));
    run.span(1, 60, [](int64_t now) { return gone(now, REASON_TIMEOUT); });
    ASSERT_EQ(run.events.size(), 2u);
    EXPECT_EQ(run.events[1]["state"], "away");
    EXPECT_EQ(run.events[1]["reason"], "out_of_range");
    EXPECT_EQ(run.events[1]["lock"], "off");
    EXPECT_EQ(run.events[1]["away_seconds"], PRESENCE_CONFIRM_SECONDS);
    EXPECT_EQ(run.locks, 0);
}

TEST(Presence, NamesEveryOtherReason) {
    const std::pair<const char*, const char*> cases[] = {
        {REMOTE, "switched_off"},
        {SUSPEND, "computer_suspended"},
        {LOCAL, "local"},
        {"org.bluez.Reason.Authentication", "unknown"},
        {"", "unknown"},
    };
    for (const auto& [bluez, published] : cases) {
        Walk run;
        run.feed(here(0));
        run.span(1, 10, [&](int64_t now) { return gone(now, bluez); });
        ASSERT_EQ(run.events.size(), 2u) << bluez;
        EXPECT_EQ(run.events[1]["reason"], published) << bluez;
    }
}

TEST(Presence, AControllerSwitchedOffHereIsItsOwnReason) {
    Walk run;
    run.feed(here(0));
    run.span(1, 10, [](int64_t now) {
        PresenceInput in = gone(now, REASON_TIMEOUT, true);
        in.adapter_powered = false;
        return in;
    });
    ASSERT_EQ(run.events.size(), 2u);
    EXPECT_EQ(run.events[1]["reason"], "bluetooth_off");
    EXPECT_EQ(run.events[1]["lock"], "spared");
}

// The supervisor cuts BR/EDR itself for hands-free and brings it straight back.
TEST(Presence, ADropMadeOnPurposeIsNotAnAbsence) {
    Walk run;
    run.feed(here(0));
    run.span(1, 15, [](int64_t now) {
        PresenceInput in = gone(now, LOCAL, true);
        in.planned_drop = true;
        return in;
    });
    EXPECT_EQ(run.events.size(), 1u);
}

TEST(Presence, AnOpenObexSessionStillReachesThePhone) {
    Walk run;
    run.feed(here(0));
    run.span(1, 60, [](int64_t now) {
        PresenceInput in = gone(now, REASON_TIMEOUT, true);
        in.obex_up = true;
        return in;
    });
    EXPECT_EQ(run.events.size(), 1u);
    EXPECT_EQ(run.locks, 0);
}

TEST(Presence, LocksOnceAfterTheGraceAndSaysSo) {
    Walk run;
    run.feed(here(0));
    run.span(1, 200, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    EXPECT_EQ(run.locks, 1);
    ASSERT_EQ(run.events.size(), 3u);
    EXPECT_EQ(run.events[1]["lock"], "pending");
    EXPECT_EQ(run.events[1]["lock_in_seconds"], 30 - PRESENCE_CONFIRM_SECONDS);
    EXPECT_EQ(run.events[2]["state"], "away");
    EXPECT_EQ(run.events[2]["lock"], "locked");
    EXPECT_EQ(run.events[2]["away_seconds"], 30);
    EXPECT_EQ(run.events[2]["lock_in_seconds"], 0);
}

TEST(Presence, AGraceShorterThanTheConfirmationStillLocks) {
    Walk run;
    run.feed(here(0));
    run.span(1, 20, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 1); });
    EXPECT_EQ(run.locks, 1);
    ASSERT_EQ(run.events.size(), 2u);
    EXPECT_EQ(run.events[1]["lock"], "locked");
}

TEST(Presence, ReportsALockThatFailed) {
    Walk run;
    run.lock_succeeds = false;
    run.feed(here(0));
    run.span(1, 100, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 10); });
    EXPECT_EQ(run.locks, 1) << "a refused lock is not retried every second";
    EXPECT_EQ(run.events.back()["lock"], "failed");
}

// Bluetooth switched off on the iPhone, the computer going to sleep: not a walk away.
TEST(Presence, NeverLocksForAnotherReason) {
    for (const char* reason : {REMOTE, SUSPEND, LOCAL, ""}) {
        Walk run;
        run.feed(here(0));
        run.span(1, 600, [&](int64_t now) { return gone(now, reason, true, 30); });
        EXPECT_EQ(run.locks, 0) << reason;
        ASSERT_EQ(run.events.size(), 2u) << reason;
        EXPECT_EQ(run.events[1]["lock"], "spared") << reason;
    }
}

TEST(Presence, AReasonThatArrivesLateIsPublishedAndCounts) {
    Walk run;
    run.feed(here(0));
    run.span(1, 5, [](int64_t now) { return gone(now, "", true, 30); });
    run.span(6, 40, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    ASSERT_EQ(run.events.size(), 4u);
    EXPECT_EQ(run.events[1]["reason"], "unknown");
    EXPECT_EQ(run.events[1]["lock"], "spared");
    EXPECT_EQ(run.events[2]["reason"], "out_of_range");
    EXPECT_EQ(run.events[2]["lock"], "pending");
    EXPECT_EQ(run.events[3]["lock"], "locked");
    EXPECT_EQ(run.locks, 1);
}

// Switching the lock on while the phone is already gone must not lock at once.
TEST(Presence, ALockEnabledDuringAnAbsenceWaitsForTheNextOne) {
    Walk run;
    run.feed(here(0));
    run.span(1, 100, [](int64_t now) { return gone(now, REASON_TIMEOUT, false); });
    run.span(101, 300, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    EXPECT_EQ(run.locks, 0);
    EXPECT_EQ(run.events.back()["lock"], "off");

    run.feed(here(301));
    run.span(302, 400, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    EXPECT_EQ(run.locks, 1);
}

TEST(Presence, SwitchingTheLockOffDuringTheGraceCancelsIt) {
    Walk run;
    run.feed(here(0));
    run.span(1, 10, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    run.span(11, 100, [](int64_t now) { return gone(now, REASON_TIMEOUT, false, 30); });
    EXPECT_EQ(run.locks, 0);
    EXPECT_EQ(run.events.back()["lock"], "off");
}

TEST(Presence, ComingBackSaysHereAndNeverUnlocks) {
    Walk run;
    run.feed(here(0));
    run.span(1, 50, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    run.feed(here(51));
    EXPECT_EQ(run.events.back()["state"], "here");
    EXPECT_EQ(run.events.back()["lock"], "none");
    EXPECT_EQ(run.events.back()["away_seconds"], 50);
    EXPECT_EQ(run.events.back()["wake"], false) << "waking the login screen is off unless asked for";
    EXPECT_EQ(run.wakes, 0);
}

TEST(Presence, WakesTheLoginScreenOnlyAfterItsOwnLockAndOnlyWhenAsked) {
    const auto back = [](int64_t now) {
        PresenceInput in = here(now);
        in.wake_on_return = true;
        return in;
    };

    Walk locked;
    locked.feed(here(0));
    locked.span(1, 50, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    locked.feed(back(51));
    EXPECT_EQ(locked.wakes, 1);
    EXPECT_EQ(locked.events.back()["wake"], true);
    locked.feed(back(52));
    EXPECT_EQ(locked.wakes, 1);
    EXPECT_EQ(locked.tracker.to_json()["wake"], false) << "said once, in the event of the return";

    // Back before the grace ran out: nothing was locked, nothing to wake.
    Walk early;
    early.feed(here(0));
    early.span(1, 20, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    early.feed(back(21));
    EXPECT_EQ(early.wakes, 0);
    EXPECT_EQ(early.events.back()["wake"], false);

    // Bluetooth switched off on the phone and back on: nothing was locked either.
    Walk spared;
    spared.feed(here(0));
    spared.span(1, 300, [](int64_t now) { return gone(now, REMOTE, true, 30); });
    spared.feed(back(301));
    EXPECT_EQ(spared.wakes, 0);
}

TEST(Presence, EachAbsenceLocksAgain) {
    Walk run;
    run.feed(here(0));
    run.span(1, 40, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    run.feed(here(41));
    run.span(42, 90, [](int64_t now) { return gone(now, REASON_TIMEOUT, true, 30); });
    EXPECT_EQ(run.locks, 2);
}

TEST(Presence, ResetGoesBackToUnknown) {
    PresenceTracker tracker;
    EXPECT_FALSE(tracker.reset());
    tracker.step(here(0));
    EXPECT_TRUE(tracker.reset());
    EXPECT_EQ(tracker.to_json()["state"], "unknown");
}

TEST(LockOnAwayCommand, KeepsWhatTheCommandLeavesOut) {
    Config config;
    config.lock_away_seconds = 90;
    config.wake_on_return = true;
    apply_lock_on_away(config, nlohmann::json{{"command", "bt_set_lock_on_away"}, {"enabled", true}});
    EXPECT_TRUE(config.lock_on_away);
    EXPECT_EQ(config.lock_away_seconds, 90) << "flipping the switch reset the grace to 30";
    EXPECT_TRUE(config.wake_on_return);

    apply_lock_on_away(config, nlohmann::json{{"grace_seconds", 45}, {"wake_on_return", false}});
    EXPECT_TRUE(config.lock_on_away);
    EXPECT_EQ(config.lock_away_seconds, 45);
    EXPECT_FALSE(config.wake_on_return);
}

TEST(LockOnAwayCommand, ClampsTheGrace) {
    Config config;
    apply_lock_on_away(config, nlohmann::json{{"grace_seconds", 0}});
    EXPECT_EQ(config.lock_away_seconds, 1);
    apply_lock_on_away(config, nlohmann::json{{"grace_seconds", 999999}});
    EXPECT_EQ(config.lock_away_seconds, MAX_AWAY_LOCK_GRACE_SECONDS);
    apply_lock_on_away(config, nlohmann::json{{"grace_seconds", "soon"}});
    EXPECT_EQ(config.lock_away_seconds, MAX_AWAY_LOCK_GRACE_SECONDS);
}

TEST(LockOnAwayConfig, IsOffByDefaultAndSurvivesARoundTrip) {
    Config config;
    EXPECT_FALSE(config.lock_on_away);
    EXPECT_FALSE(config.wake_on_return);
    EXPECT_TRUE(config.wake_command.empty());

    config.wake_on_return = true;
    config.wake_command = "true";
    const Config read = deserialize_config(serialize_config(config));
    EXPECT_TRUE(read.wake_on_return);
    EXPECT_EQ(read.wake_command, "true");
    EXPECT_EQ(deserialize_config(R"({"lock_away_seconds": 100000})").lock_away_seconds, MAX_AWAY_LOCK_GRACE_SECONDS);
}

// The lean link mode: the classic link is closed at rest, so the LE link is
// the whole of presence.
TEST(Presence, WithTheClassicLinkClosedTheLeLinkAloneIsPresence) {
    PresenceTracker tracker;
    PresenceInput in = here(0);
    in.bearer.classic_connected = false;
    EXPECT_TRUE(tracker.step(in).changed);
    EXPECT_EQ(tracker.to_json().value("state", ""), "here");

    // The LE link times out with nothing else up: the phone has left.
    bool changed = false;
    for (int64_t now = 10; now < 10 + PRESENCE_CONFIRM_SECONDS + 1; ++now)
        changed = tracker.step(gone(now, REASON_TIMEOUT)).changed || changed;
    EXPECT_TRUE(changed);
    EXPECT_EQ(tracker.to_json().value("state", ""), "away");
    EXPECT_EQ(tracker.to_json().value("reason", ""), "out_of_range");
}

// A classic link opened for a lease does not make the phone leave when the
// LE link blinks under it.
TEST(Presence, AnLeBlinkUnderAnOpenClassicLinkIsNotAnAbsence) {
    PresenceTracker tracker;
    tracker.step(here(0));
    PresenceInput in = here(5);
    in.bearer.le_connected = false;
    for (int64_t now = 5; now < 60; ++now) {
        in.now = now;
        EXPECT_FALSE(tracker.step(in).changed);
    }
    EXPECT_EQ(tracker.to_json().value("state", ""), "here");
}
