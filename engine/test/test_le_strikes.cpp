#include <gtest/gtest.h>
#include <tether/bluetooth/advert.hpp>
#include <tether/bluetooth/le_after_sleep.hpp>
#include <tether/bluetooth/le_strikes.hpp>

using namespace tether::bluetooth;

namespace {

    using Kind = LeStrikes::Event::Kind;

    LeStrikeFacts at(int64_t now, bool connected, uint64_t drops, bool live = false, bool armed = true) {
        LeStrikeFacts facts;
        facts.now = now;
        facts.armed = armed;
        facts.le_connected = connected;
        facts.drop_count = drops;
        facts.service_live = live;
        return facts;
    }

    // A link that comes up at `from`, is seen serving from `live_from` on
    // (never when negative) and is looked at every second up to `to`.
    void link(LeStrikes& strikes, int64_t from, int64_t to, uint64_t drops, int64_t live_from = -1) {
        for (int64_t t = from; t <= to; ++t)
            strikes.observe(at(t, true, drops, live_from >= 0 && t >= live_from));
    }

    // The rule as it was before #29, which the environment gives back:
    // withheld 15 s, then 30 s doubled, slow from the second strike, and the
    // rhythm of an ordinary loss.
    LeStrikesConfig before_29() {
        return le_strikes_config_from(nullptr, "15", "30", nullptr, "0", "2");
    }

    LeStateFacts lost_phone() {
        LeStateFacts facts;
        facts.ancs_enabled = true;
        facts.device_paired = true;
        facts.le_available = true;
        facts.advert_registered = true;
        facts.now = 1000;
        facts.le_down_since = 100;
        return facts;
    }

} // namespace

TEST(LeStrikes, ConfigFromEnvironment) {
    LeStrikesConfig config = le_strikes_config_from(nullptr, nullptr, nullptr, nullptr);
    EXPECT_TRUE(config.enabled);
    // Since #29: never off air, never slow, 30 s fast after each.
    EXPECT_EQ(config.first_pause_seconds, 0);
    EXPECT_EQ(config.long_pause_seconds, 0);
    EXPECT_EQ(config.served_seconds, 10);
    EXPECT_EQ(config.fast_seconds, 30);
    EXPECT_EQ(config.slow_from, 0);

    config = before_29();
    EXPECT_EQ(config.first_pause_seconds, 15);
    EXPECT_EQ(config.long_pause_seconds, 30);
    EXPECT_EQ(config.fast_seconds, 0);
    EXPECT_EQ(config.slow_from, 2);

    config = le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, "180", "99");
    EXPECT_EQ(config.fast_seconds, 180);
    EXPECT_EQ(config.slow_from, 99);
    config = le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, "181", "100");
    EXPECT_EQ(config.fast_seconds, LE_STRIKE_FAST_SECONDS);
    EXPECT_EQ(config.slow_from, LE_STRIKE_SLOW_FROM);

    config = le_strikes_config_from("0", "0", "0", "1");
    EXPECT_FALSE(config.enabled);
    EXPECT_EQ(config.first_pause_seconds, 0);
    EXPECT_EQ(config.long_pause_seconds, 0);
    EXPECT_EQ(config.served_seconds, 1);

    config = le_strikes_config_from("1", "30", "600", "45");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.first_pause_seconds, 30);
    EXPECT_EQ(config.long_pause_seconds, 600);
    EXPECT_EQ(config.served_seconds, 45);

    // Not a number, out of range: the default.
    config = le_strikes_config_from("", "soon", "-3", "0");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.first_pause_seconds, LE_STRIKE_PAUSE_SECONDS);
    EXPECT_EQ(config.long_pause_seconds, LE_STRIKE_LONG_PAUSE_SECONDS);
    EXPECT_EQ(config.served_seconds, LE_SERVED_SECONDS);
    config = le_strikes_config_from(nullptr, "601", "99999", "601");
    EXPECT_EQ(config.first_pause_seconds, LE_STRIKE_PAUSE_SECONDS);
    EXPECT_EQ(config.long_pause_seconds, LE_STRIKE_LONG_PAUSE_SECONDS);
    EXPECT_EQ(config.served_seconds, LE_SERVED_SECONDS);
}

TEST(LeStrikes, BeforeTwentyNineTheFirstUnusedLinkTookTheAdvertOffAir) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    // Dead after two seconds, the service never answered.
    const auto event = strikes.observe(at(3, false, 1));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Strike);
    EXPECT_EQ(event->strikes, 1);
    EXPECT_EQ(event->links, 1);
    EXPECT_EQ(event->pause_seconds, 15);
    EXPECT_TRUE(strikes.advert_paused(3));
    EXPECT_EQ(strikes.pause_left(3), 15);
    EXPECT_TRUE(strikes.advert_paused(17));
    EXPECT_FALSE(strikes.advert_paused(18));
    EXPECT_EQ(strikes.pause_left(18), 0);
    // One strike: the advert comes back at its usual interval.
    EXPECT_FALSE(strikes.slow_advert());
    EXPECT_TRUE(strikes.unsteady());
}

TEST(LeStrikes, ALinkThatServedIsNotCounted) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 5, 0, 3);
    // The pass that sees the drop still reads the service as live.
    EXPECT_FALSE(strikes.observe(at(6, false, 1, true)));
    EXPECT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.advert_paused(6));
    EXPECT_FALSE(strikes.unsteady());
}

TEST(LeStrikes, ServiceSeenOnlyByThePassOfTheDropStillCounts) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    strikes.observe(at(1, true, 0));
    // The answer came and the link died between two passes.
    EXPECT_FALSE(strikes.observe(at(2, false, 1, true)));
    EXPECT_EQ(strikes.strikes(), 0);
}

TEST(LeStrikes, AServedLinkThenLinksBornAndDeadBetweenTwoPasses) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 30, 0, 2);
    // Three Disconnected signals in one pass: the served link, then two
    // that nobody saw.
    const auto event = strikes.observe(at(31, false, 3, true));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Strike);
    EXPECT_EQ(event->links, 2);
    EXPECT_EQ(event->strikes, 2);
    EXPECT_EQ(event->pause_seconds, 30);
}

TEST(LeStrikes, BeforeTwentyNineTheSecondOneInARowWasHalfAMinuteThenASlowAdvert) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    ASSERT_TRUE(strikes.observe(at(3, false, 1)));
    // The advert is back at 18 s, the phone connects at 20 and is lost at 22.
    link(strikes, 20, 21, 1);
    const auto event = strikes.observe(at(22, false, 2));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->strikes, 2);
    EXPECT_EQ(event->pause_seconds, 30);
    EXPECT_TRUE(strikes.advert_paused(51));
    EXPECT_FALSE(strikes.advert_paused(52));
    EXPECT_TRUE(strikes.slow_advert());
    // Never a third link within fifteen seconds of the second.
    EXPECT_GE(strikes.pause_left(22), 15);
}

TEST(LeStrikes, ThePauseDoublesAndStopsAtItsCeiling) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    int64_t now = 0;
    uint64_t drops = 0;
    const int expected[] = {15, 30, 60, 120, 120, 120};
    for (int pause : expected) {
        // After the pause has run out, before the count is forgotten.
        now += 700;
        strikes.observe(at(now, true, drops));
        const auto event = strikes.observe(at(now + 1, false, ++drops));
        ASSERT_TRUE(event);
        EXPECT_EQ(event->pause_seconds, pause);
    }
    EXPECT_EQ(strikes.strikes(), 6);
}

TEST(LeStrikes, ALinkThatHasServedForAWhileClearsTheCount) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    link(strikes, 20, 21, 1);
    strikes.observe(at(22, false, 2));
    ASSERT_EQ(strikes.strikes(), 2);

    // Up at 150, the service answers at 153 and stays.
    for (int64_t t = 150; t < 163; ++t)
        EXPECT_FALSE(strikes.observe(at(t, true, 2, t >= 153))) << t;
    EXPECT_TRUE(strikes.slow_advert());
    const auto event = strikes.observe(at(163, true, 2, true));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Served);
    EXPECT_EQ(event->strikes, 2);
    EXPECT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.slow_advert());
    EXPECT_FALSE(strikes.unsteady());
    EXPECT_FALSE(strikes.lost_after_strikes(170));
    // Said once.
    EXPECT_FALSE(strikes.observe(at(164, true, 2, true)));
}

TEST(LeStrikes, ALinkThatServedTooBrieflyNeitherCountsNorClears) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    // Served for four seconds, then lost.
    link(strikes, 20, 26, 1, 22);
    EXPECT_FALSE(strikes.observe(at(27, false, 2, true)));
    EXPECT_EQ(strikes.strikes(), 1);
    // The next unused one is the second, not the first.
    link(strikes, 30, 31, 2);
    const auto event = strikes.observe(at(32, false, 3));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->strikes, 2);
}

TEST(LeStrikes, TheServiceOfTheLinkBeforeIsNotCreditedToTheNextOne) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 30, 0, 2);
    // Dropped and already back by the pass that sees the drop; the client
    // has not been told yet and still reads ready.
    EXPECT_FALSE(strikes.observe(at(31, true, 1, true)));
    // The client has forgotten its session; the new link dies unused.
    strikes.observe(at(32, true, 1, false));
    const auto event = strikes.observe(at(33, false, 2, false));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->strikes, 1);
}

TEST(LeStrikes, NothingIsCountedWhenThePhoneIsNotHeldToIt) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0, false, false));
    // A pairing under way: the link comes and goes.
    for (uint64_t drop = 1; drop <= 5; ++drop) {
        strikes.observe(at(static_cast<int64_t>(drop) * 3, true, drop - 1, false, false));
        EXPECT_FALSE(strikes.observe(at(static_cast<int64_t>(drop) * 3 + 1, false, drop, false, false)));
    }
    EXPECT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.advert_paused(16));
}

TEST(LeStrikes, WhatIsThereAtStartIsNotADrop) {
    LeStrikes strikes;
    // The monitor has counted drops before this process looked.
    EXPECT_FALSE(strikes.observe(at(0, false, 7)));
    EXPECT_FALSE(strikes.observe(at(1, false, 7)));
    EXPECT_EQ(strikes.strikes(), 0);
}

TEST(LeStrikes, AStackThatSendsNoSignalIsReadFromTheEdge) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    strikes.observe(at(1, true, 0));
    const auto event = strikes.observe(at(2, false, 0));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->strikes, 1);
    // The signal counted, the property following a moment later: once.
    LeStrikes other;
    other.observe(at(0, false, 0));
    other.observe(at(1, true, 0));
    ASSERT_TRUE(other.observe(at(2, true, 1)));
    EXPECT_FALSE(other.observe(at(3, false, 1)));
    EXPECT_EQ(other.strikes(), 1);
}

TEST(LeStrikes, TheCountIsForgottenWhenThePhoneHasBeenGoneForLong) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    link(strikes, 20, 21, 1);
    strikes.observe(at(22, false, 2));
    EXPECT_FALSE(strikes.observe(at(22 + LE_STRIKE_MEMORY_SECONDS - 1, false, 2)));
    EXPECT_TRUE(strikes.slow_advert());
    const auto event = strikes.observe(at(22 + LE_STRIKE_MEMORY_SECONDS, false, 2));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Forgotten);
    EXPECT_EQ(event->strikes, 2);
    EXPECT_FALSE(strikes.slow_advert());
    // What it may have done to the phone is still what explains its silence.
    EXPECT_TRUE(strikes.lost_after_strikes(22));
    EXPECT_FALSE(strikes.lost_after_strikes(22 + LE_STRIKE_BLAME_SECONDS + 1));
}

TEST(LeStrikes, OneStrikeDoesNotExplainALostPhone) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    EXPECT_FALSE(strikes.lost_after_strikes(3));
    link(strikes, 20, 21, 1);
    strikes.observe(at(22, false, 2));
    EXPECT_TRUE(strikes.lost_after_strikes(22));
}

TEST(LeStrikes, SwitchedOffItCountsNothing) {
    LeStrikes strikes;
    LeStrikesConfig config;
    config.enabled = false;
    strikes.configure(config);
    strikes.observe(at(0, false, 0));
    for (uint64_t drop = 1; drop <= 4; ++drop) {
        strikes.observe(at(static_cast<int64_t>(drop) * 3, true, drop - 1));
        EXPECT_FALSE(strikes.observe(at(static_cast<int64_t>(drop) * 3 + 1, false, drop)));
    }
    EXPECT_FALSE(strikes.advert_paused(13));
    EXPECT_FALSE(strikes.slow_advert());
    EXPECT_FALSE(strikes.unsteady());
    EXPECT_FALSE(strikes.lost_after_strikes(13));
}

TEST(LeStrikes, PausesOfZeroLeaveTheAdvertOnAir) {
    LeStrikes strikes;
    LeStrikesConfig config = before_29();
    config.first_pause_seconds = 0;
    config.long_pause_seconds = 0;
    strikes.configure(config);
    strikes.observe(at(0, false, 0));
    strikes.observe(at(1, true, 0));
    auto event = strikes.observe(at(2, false, 1));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 0);
    EXPECT_FALSE(strikes.advert_paused(2));
    strikes.observe(at(3, true, 1));
    event = strikes.observe(at(4, false, 2));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 0);
    EXPECT_FALSE(strikes.advert_paused(4));
    // Still slow: that is the other half of the rule.
    EXPECT_TRUE(strikes.slow_advert());
}

TEST(LeStrikes, ResetKeepsTheSettingsAndTheDropCount) {
    LeStrikes strikes;
    LeStrikesConfig config;
    config.first_pause_seconds = 40;
    strikes.configure(config);
    strikes.observe(at(0, false, 0));
    strikes.observe(at(1, true, 0));
    strikes.observe(at(2, false, 1));
    ASSERT_TRUE(strikes.advert_paused(3));
    // The computer slept: the links it cut are nobody's strikes.
    strikes.reset(4);
    EXPECT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.advert_paused(3));
    EXPECT_EQ(strikes.config().first_pause_seconds, 40);
    EXPECT_FALSE(strikes.observe(at(10, false, 4)));
}

// The evening of 17 November 2026: about twenty links in eighty seconds,
// each dead within one to eight seconds. With the pauses of 18 November the
// phone was offered four of them in the first two minutes; those pauses
// only delayed the return home and are gone by default since #29.
TEST(LeStrikes, TheArrivalOfTheSeventeenthWithThePausesOfBefore) {
    LeStrikes strikes;
    strikes.configure(before_29());
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    int links = 0;
    bool up = false;
    int64_t up_since = 0;
    for (int64_t t = 1; t <= 120; ++t) {
        if (!up && !strikes.advert_paused(t)) {
            // The advert is on air: the phone answers it within the second.
            up = true;
            up_since = t;
            ++links;
        } else if (up && t - up_since >= 2) {
            up = false;
            ++drops;
        }
        strikes.observe(at(t, up, drops));
    }
    EXPECT_EQ(links, 4);
    EXPECT_EQ(strikes.strikes(), 4);
}

TEST(LeStrikes, SlowIsTheLongestIntervalOfTheList) {
    // 1022.5 ms is 1636 slots of 0.625 ms; BlueZ truncates milliseconds.
    EXPECT_EQ(ADVERT_INTERVAL_SLOW.max_ms * 1000 / 625, 1636u);
    EXPECT_EQ(ADVERT_INTERVAL_SLOW.min_ms * 1000 / 625, 1635u);
    EXPECT_TRUE(ADVERT_INTERVAL_SLOW.set());
}

TEST(LeStrikesState, HoldingOffIsSaidBeforeAnythingElse) {
    LeStateFacts facts = lost_phone();
    facts.holding_off = true;
    // Whatever else is true: the silence is this computer's own.
    facts.lost_after_burst = true;
    facts.classic_connected = true;
    facts.advert_registered = false;
    LeState state = le_state_for(facts);
    EXPECT_EQ(state.state, "waiting");
    EXPECT_EQ(state.cause, "holding_off");
    EXPECT_FALSE(state.detail.empty());

    // Also within the first minutes of the loss.
    facts.le_down_since = 990;
    state = le_state_for(facts);
    EXPECT_EQ(state.state, "waiting");
    EXPECT_EQ(state.cause, "holding_off");
}

TEST(LeStrikesState, NotWhileConnectedNorWhenOff) {
    LeStateFacts facts = lost_phone();
    facts.holding_off = true;
    facts.le_connected = true;
    EXPECT_EQ(le_state_for(facts).state, "connected");
    facts.le_connected = false;
    facts.ancs_enabled = false;
    EXPECT_EQ(le_state_for(facts).state, "off");
}

TEST(LeStrikesState, AfterThePauseTheLinkIsUnsteadyThenThePhoneHasGivenUp) {
    LeStateFacts facts = lost_phone();
    // The advert is back, strikes on record, the loss is recent.
    facts.le_down_since = 990;
    facts.unsteady = true;
    LeState state = le_state_for(facts);
    EXPECT_EQ(state.state, "waiting");
    EXPECT_EQ(state.cause, "unsteady");

    // Three minutes on, the classic link has reached the phone: it is here.
    facts.le_down_since = 100;
    facts.lost_after_burst = true;
    facts.classic_seen_since_loss = true;
    state = le_state_for(facts);
    EXPECT_EQ(state.state, "paused");
    EXPECT_EQ(state.cause, "phone_backoff");
}

// Pebble's rule for links that served and keep dropping
// (gap_le_slave_reconnect.c): more than three in a minute and the advert
// goes slow, with no fast phase, until a minute passes without one.
TEST(LeStrikesChurn, MoreThanThreeServedLossesInAMinuteSlowTheAdvert) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    int64_t t = 1;
    std::optional<LeStrikes::Event> event;
    for (int i = 0; i < 4; ++i) {
        link(strikes, t, t + 5, drops, t + 1);
        ++drops;
        event = strikes.observe(at(t + 6, false, drops, true));
        if (i < 3) {
            EXPECT_FALSE(event) << i;
            EXPECT_FALSE(strikes.churning(t + 6)) << i;
        }
        t += 10;
    }
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Churn);
    EXPECT_EQ(event->links, 4);
    // Not strikes: the advert stays on air, only slow.
    EXPECT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.advert_paused(t));
    EXPECT_TRUE(strikes.churning(t));
    EXPECT_TRUE(strikes.churning(37 + LE_CHURN_WINDOW_SECONDS - 1));
    EXPECT_FALSE(strikes.churning(37 + LE_CHURN_WINDOW_SECONDS));
    // A fifth loss within the minute extends it, said once.
    link(strikes, 41, 45, drops, 42);
    ++drops;
    EXPECT_FALSE(strikes.observe(at(46, false, drops, true)));
    EXPECT_TRUE(strikes.churning(46 + LE_CHURN_WINDOW_SECONDS - 1));
}

TEST(LeStrikesChurn, FourServedLossesSpreadOverMoreThanAMinuteAreNotAChurn) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    for (int i = 0; i < 6; ++i) {
        const int64_t t = 1 + i * 25;
        link(strikes, t, t + 5, drops, t + 1);
        ++drops;
        EXPECT_FALSE(strikes.observe(at(t + 6, false, drops, true))) << i;
        EXPECT_FALSE(strikes.churning(t + 6)) << i;
    }
}

TEST(LeStrikesChurn, UnusedLinksAreStillStrikesNotChurn) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    // Up and down without the service: a strike, as before.
    link(strikes, 1, 2, 0);
    const auto event = strikes.observe(at(3, false, 1));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Strike);
    EXPECT_FALSE(strikes.churning(3));
}

TEST(LeStrikesChurn, NothingWhenOffOrNotHeld) {
    LeStrikes strikes;
    LeStrikesConfig config;
    config.enabled = false;
    strikes.configure(config);
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    for (int i = 0; i < 5; ++i) {
        const int64_t t = 1 + i * 8;
        link(strikes, t, t + 5, drops, t + 1);
        ++drops;
        EXPECT_FALSE(strikes.observe(at(t + 6, false, drops, true)));
    }
    EXPECT_FALSE(strikes.churning(40));
}

// --- Since #29: the advert stays on air ---------------------------------

TEST(LeStrikesOnAir, AnUnusedLinkLeavesTheAdvertOnAirFastThenSteady) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    const auto event = strikes.observe(at(3, false, 1));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Strike);
    EXPECT_EQ(event->strikes, 1);
    EXPECT_EQ(event->pause_seconds, 0);
    EXPECT_FALSE(strikes.advert_paused(3));
    EXPECT_EQ(strikes.pause_left(3), 0);
    EXPECT_FALSE(strikes.slow_advert());
    EXPECT_TRUE(strikes.unsteady());
    // 20 ms for 30 s after the strike, then 152.5 ms, though the LE link
    // went down at 3 and every link with it.
    EXPECT_EQ(advert_interval_with_strikes(strikes, 3, 3, ADVERT_FAST_SECONDS, -1, 3), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 32, 3, ADVERT_FAST_SECONDS, -1, 3), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 33, 3, ADVERT_FAST_SECONDS, -1, 3), ADVERT_INTERVAL_STEADY);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 600, 3, ADVERT_FAST_SECONDS, -1, 3), ADVERT_INTERVAL_STEADY);
    // BR/EDR reaching the phone without LE still makes it fast.
    EXPECT_EQ(advert_interval_with_strikes(strikes, 100, 3, ADVERT_FAST_SECONDS, 90, 3), ADVERT_INTERVAL_FAST);
    // No fast advert at all asked for: none after a strike either.
    EXPECT_EQ(advert_interval_with_strikes(strikes, 4, 3, 0, -1, 3), ADVERT_INTERVAL_STEADY);
}

TEST(LeStrikesOnAir, NeverOffAirNorSlowHoweverManyInARow) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    for (int64_t t = 1; t < 200; t += 3) {
        strikes.observe(at(t, true, drops));
        const auto event = strikes.observe(at(t + 1, false, ++drops));
        ASSERT_TRUE(event);
        EXPECT_EQ(event->pause_seconds, 0);
        EXPECT_FALSE(strikes.advert_paused(t + 1));
        EXPECT_NE(advert_interval_with_strikes(strikes, t + 2, t + 1), ADVERT_INTERVAL_SLOW);
    }
    EXPECT_GE(strikes.strikes(), 60);
    EXPECT_FALSE(strikes.slow_advert());
    // Still counted, and still what explains a phone that goes silent.
    EXPECT_TRUE(strikes.lost_after_strikes(200));
}

TEST(LeStrikesOnAir, AServedLinkGivesTheUsualRhythmBack) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    link(strikes, 50, 70, 1, 52);
    ASSERT_EQ(strikes.strikes(), 0);
    EXPECT_FALSE(strikes.sets_rhythm());
    // An ordinary loss at 80: fast for 180 s as always.
    EXPECT_EQ(advert_interval_with_strikes(strikes, 200, 80), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 260, 80), ADVERT_INTERVAL_STEADY);
}

TEST(LeStrikesOnAir, ChurnIsStillSlow) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    int64_t t = 1;
    for (int i = 0; i < 4; ++i) {
        link(strikes, t, t + 5, drops, t + 1);
        strikes.observe(at(t + 6, false, ++drops, true));
        t += 10;
    }
    ASSERT_TRUE(strikes.churning(t));
    EXPECT_EQ(advert_interval_with_strikes(strikes, t, t - 4), ADVERT_INTERVAL_SLOW);
}

TEST(LeStrikesOnAir, FastSecondsOfZeroLeaveTheRhythmOfALoss) {
    LeStrikes strikes;
    strikes.configure(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, "0", nullptr));
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    ASSERT_TRUE(strikes.unsteady());
    EXPECT_FALSE(strikes.sets_rhythm());
    EXPECT_EQ(advert_interval_with_strikes(strikes, 100, 3), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 183, 3), ADVERT_INTERVAL_STEADY);
}

TEST(LeStrikesOnAir, TheSlowAdvertOfBeforeCanBeAskedFor) {
    LeStrikes strikes;
    strikes.configure(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, "2"));
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    strikes.observe(at(3, false, 1));
    EXPECT_FALSE(strikes.slow_advert());
    link(strikes, 5, 6, 1);
    strikes.observe(at(7, false, 2));
    EXPECT_TRUE(strikes.slow_advert());
    EXPECT_FALSE(strikes.advert_paused(7));
    EXPECT_EQ(advert_interval_with_strikes(strikes, 8, 7), ADVERT_INTERVAL_SLOW);
}

// 18 November 2026, coming home (#1, #29). In through the door at 16:18:00.
// Links at 16:17:32, 16:17:50 and 16:19:43 died after 1.4, 0.7 and 0.7 s,
// before the notification service answered; the fourth, at 16:20:56, held
// and notifications came at 16:20:58. The advert had been withheld 15, 30
// and 60 s and put back at 1022.5 ms: about 80 s indoors before the iPhone
// answered the slow advert, then a minute off air. Seconds here are counted
// from 16:17:00, one pass a second, and a link is up on the pass after it
// opened and seen down on the pass after it died.
TEST(LeStrikesOnAir, TheReturnHomeOfTheEighteenth) {
    LeStrikes strikes;
    LeStateFacts state = lost_phone();
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    struct Link {
        int64_t up;
        int64_t down;
    };
    // 16:17:32 + 1.4 s, 16:17:50 + 0.7 s, 16:19:43 + 0.7 s.
    const Link dead[] = {{32, 34}, {50, 51}, {163, 164}};
    int64_t le_down_since = 0;
    for (int64_t t = 1; t < 236; ++t) {
        bool up = false;
        bool drop = false;
        for (const Link& l : dead) {
            up = up || (t >= l.up && t < l.down);
            drop = drop || t == l.down;
        }
        if (drop) {
            ++drops;
            le_down_since = t;
        }
        const auto event = strikes.observe(at(t, up, drops));
        if (drop) {
            ASSERT_TRUE(event) << t;
            EXPECT_EQ(event->kind, Kind::Strike) << t;
            EXPECT_EQ(event->pause_seconds, 0) << t;
        }
        // The advert is on air on every pass, never at 1022.5 ms.
        EXPECT_FALSE(strikes.advert_paused(t)) << t;
        // Before the first strike the rhythm is that of a loss eighty minutes
        // old, which this test does not model.
        if (!up && t >= 34) {
            const AdvertInterval interval = advert_interval_with_strikes(strikes, t, le_down_since);
            EXPECT_NE(interval, ADVERT_INTERVAL_SLOW) << t;
            // 20 ms for 30 s after each dead link, 152.5 ms after that.
            const bool fast = (t >= 34 && t < 81) || (t >= 164 && t < 194);
            EXPECT_EQ(interval, fast ? ADVERT_INTERVAL_FAST : ADVERT_INTERVAL_STEADY) << t;
        }
        // Published: waiting, links failing, trying again.
        if (t > 34 && !up) {
            state.now = t;
            state.le_down_since = le_down_since;
            state.unsteady = strikes.unsteady();
            state.holding_off = strikes.advert_paused(t);
            state.lost_after_burst = strikes.lost_after_strikes(le_down_since);
            const LeState le = le_state_for(state);
            EXPECT_EQ(le.state, "waiting") << t;
            EXPECT_EQ(le.cause, "unsteady") << t;
        }
    }
    EXPECT_EQ(strikes.strikes(), 3);
    // The link of 16:20:56 holds: notifications from 16:20:58, the count is
    // cleared ten seconds later.
    const int64_t held = 236;
    std::optional<LeStrikes::Event> served;
    for (int64_t t = held; t < held + 15 && !served; ++t)
        served = strikes.observe(at(t, true, drops, t >= held + 2));
    ASSERT_TRUE(served);
    EXPECT_EQ(served->kind, Kind::Served);
    EXPECT_EQ(served->strikes, 3);
    EXPECT_FALSE(strikes.unsteady());
}

// --- #40: a link that served after the last strike ----------------------

TEST(LeStrikesOnAir, ALinkServedBrieflyAfterAStrikeIsAnOrdinaryLoss) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    link(strikes, 1, 2, 0);
    ASSERT_EQ(strikes.observe(at(3, false, 1))->kind, Kind::Strike);
    // A link at 50 serves for 5 s, too briefly to clear the count, and drops.
    link(strikes, 50, 55, 1, 50);
    strikes.observe(at(56, false, 2, true));
    // Still on record for the journal and le_state.
    EXPECT_EQ(strikes.strikes(), 1);
    EXPECT_TRUE(strikes.unsteady());
    EXPECT_TRUE(strikes.served_since_strike());
    EXPECT_FALSE(strikes.sets_rhythm());
    // The rhythm of an ordinary loss: 20 ms for 180 s, then 152.5 ms.
    EXPECT_EQ(advert_interval_with_strikes(strikes, 56, 56), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 235, 56), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_with_strikes(strikes, 236, 56), ADVERT_INTERVAL_STEADY);
    // A new unused link: the strikes set the rhythm again.
    link(strikes, 300, 301, 2);
    const auto event = strikes.observe(at(302, false, 3));
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Strike);
    EXPECT_EQ(event->strikes, 2);
    EXPECT_FALSE(strikes.served_since_strike());
    EXPECT_TRUE(strikes.sets_rhythm());
    EXPECT_EQ(advert_interval_with_strikes(strikes, 340, 302), ADVERT_INTERVAL_STEADY);
}

// --- #41: a brake on an endless run of unused links ----------------------

TEST(LeStrikes, BrakeFromEnvironment) {
    EXPECT_EQ(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr).brake_from, 8);
    EXPECT_EQ(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "0").brake_from, 0);
    EXPECT_EQ(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "99").brake_from, 99);
    EXPECT_EQ(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "100").brake_from,
              LE_STRIKE_BRAKE_FROM);
    EXPECT_EQ(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "x").brake_from,
              LE_STRIKE_BRAKE_FROM);
}

TEST(LeStrikesOnAir, EightUnusedLinksInARowAndTheFastPhaseStops) {
    LeStrikes strikes;
    strikes.observe(at(0, false, 0));
    uint64_t drops = 0;
    int braked_events = 0;
    for (int i = 1; i <= 12; ++i) {
        const int64_t t = i * 100;
        link(strikes, t, t + 1, drops);
        const auto event = strikes.observe(at(t + 2, false, ++drops));
        ASSERT_TRUE(event);
        ASSERT_EQ(event->kind, Kind::Strike);
        braked_events += event->braked ? 1 : 0;
        EXPECT_EQ(event->braked, i == 8) << i;
        EXPECT_EQ(strikes.braked(), i >= 8) << i;
        // Right after the strike, even with BR/EDR just reaching the phone.
        const AdvertInterval interval =
            advert_interval_with_strikes(strikes, t + 3, t + 2, ADVERT_FAST_SECONDS, t + 2, t + 2);
        EXPECT_EQ(interval, i >= 8 ? ADVERT_INTERVAL_STEADY : ADVERT_INTERVAL_FAST) << i;
        EXPECT_FALSE(strikes.advert_paused(t + 3));
    }
    // Said once.
    EXPECT_EQ(braked_events, 1);
    // A link that serves, even briefly, lifts the brake.
    link(strikes, 2000, 2004, drops, 2000);
    strikes.observe(at(2005, false, ++drops, true));
    EXPECT_FALSE(strikes.braked());
    EXPECT_EQ(advert_interval_with_strikes(strikes, 2006, 2005), ADVERT_INTERVAL_FAST);
    // Three in a row, as on the way home of the eighteenth, are far from it.
    for (int64_t t : {2100, 2200, 2300}) {
        link(strikes, t, t + 1, drops);
        strikes.observe(at(t + 2, false, ++drops));
    }
    EXPECT_FALSE(strikes.braked());
    EXPECT_EQ(advert_interval_with_strikes(strikes, 2303, 2302), ADVERT_INTERVAL_FAST);
}

TEST(LeStrikesOnAir, TheBrakeCanBeMovedOrTurnedOff) {
    for (const char* setting : {"0", "3"}) {
        LeStrikes strikes;
        strikes.configure(le_strikes_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, setting));
        strikes.observe(at(0, false, 0));
        uint64_t drops = 0;
        for (int i = 1; i <= 20; ++i) {
            link(strikes, i * 100, i * 100 + 1, drops);
            strikes.observe(at(i * 100 + 2, false, ++drops));
        }
        const bool off = std::string_view(setting) == "0";
        EXPECT_EQ(strikes.braked(), !off) << setting;
        EXPECT_EQ(advert_interval_with_strikes(strikes, 2003, 2002), off ? ADVERT_INTERVAL_FAST : ADVERT_INTERVAL_STEADY)
            << setting;
    }
}
