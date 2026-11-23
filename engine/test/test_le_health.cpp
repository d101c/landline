#include <gtest/gtest.h>
#include <tether/bluetooth/le_after_sleep.hpp>
#include <tether/bluetooth/le_health.hpp>

using namespace tether::bluetooth;

namespace {

    using Kind = LeHealth::Event::Kind;

    // A link up from `from` to `to` with nothing else going on.
    void hold(LeHealth& health, int64_t from, int64_t to, uint64_t drops) {
        for (int64_t t = from; t <= to; ++t)
            EXPECT_FALSE(health.observe(t, true, false, drops)) << t;
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

TEST(LeHealth, ConfigFromEnvironment) {
    LeHealthConfig config = le_health_config_from(nullptr, nullptr, nullptr);
    EXPECT_EQ(config.stable_seconds, LE_STABLE_SECONDS);
    EXPECT_EQ(config.quiet_seconds, CLASSIC_QUIET_SECONDS);
    EXPECT_TRUE(config.burst_pause);

    config = le_health_config_from("0", "0", "0");
    EXPECT_EQ(config.stable_seconds, 0);
    EXPECT_EQ(config.quiet_seconds, 0);
    EXPECT_FALSE(config.burst_pause);

    config = le_health_config_from("90", "600", "1");
    EXPECT_EQ(config.stable_seconds, 90);
    EXPECT_EQ(config.quiet_seconds, 600);
    EXPECT_TRUE(config.burst_pause);

    config = le_health_config_from("soon", "-3", "");
    EXPECT_EQ(config.stable_seconds, LE_STABLE_SECONDS);
    EXPECT_EQ(config.quiet_seconds, CLASSIC_QUIET_SECONDS);
    config = le_health_config_from("99999", "99999", nullptr);
    EXPECT_EQ(config.stable_seconds, LE_STABLE_SECONDS);
    EXPECT_EQ(config.quiet_seconds, CLASSIC_QUIET_SECONDS);
}

TEST(LeHealth, NoOpeningOnAYoungLink) {
    LeHealth health;
    EXPECT_FALSE(health.auto_open_allowed(0));
    health.observe(0, false, false, 0);
    EXPECT_FALSE(health.auto_open_allowed(0));
    health.observe(10, true, false, 0);
    EXPECT_FALSE(health.stable(10));
    EXPECT_FALSE(health.auto_open_allowed(10 + LE_STABLE_SECONDS - 1));
    EXPECT_TRUE(health.auto_open_allowed(10 + LE_STABLE_SECONDS));
}

TEST(LeHealth, ADropStartsTheWaitAgain) {
    LeHealth health;
    health.observe(0, true, false, 0);
    hold(health, 1, 60, 0);
    EXPECT_TRUE(health.auto_open_allowed(60));
    // Down and up again between two looks: only the count says so.
    health.observe(61, true, false, 1);
    EXPECT_FALSE(health.auto_open_allowed(61));
    EXPECT_FALSE(health.auto_open_allowed(61 + LE_STABLE_SECONDS - 1));
    EXPECT_TRUE(health.auto_open_allowed(61 + LE_STABLE_SECONDS));
}

TEST(LeHealth, StableSecondsZeroOpensAtOnce) {
    LeHealth health;
    LeHealthConfig config;
    config.stable_seconds = 0;
    health.configure(config);
    health.observe(0, false, false, 0);
    EXPECT_FALSE(health.auto_open_allowed(0));
    health.observe(1, true, false, 0);
    EXPECT_TRUE(health.auto_open_allowed(1));
}

// 18:05:49.7 the classic link is opened, 18:05:50.5 the LE link is gone.
TEST(LeHealth, ADropRightAfterAnOpeningIsPutDownToIt) {
    LeHealth health;
    health.observe(0, true, false, 0);
    hold(health, 1, 100, 0);
    health.note_classic(ClassicTouch::Opened, 100);
    const auto event = health.observe(101, false, false, 1);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::ClassicBlamed);
    EXPECT_EQ(event->touch, ClassicTouch::Opened);
    EXPECT_EQ(event->after_seconds, 1);
    EXPECT_EQ(event->quiet_seconds, CLASSIC_QUIET_SECONDS);
    EXPECT_EQ(health.quiet_until(101), 101 + CLASSIC_QUIET_SECONDS);

    // The LE link is back and holds: still nothing opened unasked.
    health.observe(105, true, true, 1);
    EXPECT_TRUE(health.stable(105 + LE_STABLE_SECONDS));
    EXPECT_FALSE(health.auto_open_allowed(105 + LE_STABLE_SECONDS));
    EXPECT_FALSE(health.auto_open_allowed(100 + CLASSIC_QUIET_SECONDS));
    EXPECT_TRUE(health.auto_open_allowed(101 + CLASSIC_QUIET_SECONDS));
    EXPECT_EQ(health.quiet_until(101 + CLASSIC_QUIET_SECONDS), -1);
}

TEST(LeHealth, ADropLongAfterAnOpeningIsNot) {
    LeHealth health;
    health.observe(0, true, false, 0);
    health.note_classic(ClassicTouch::Opened, 10);
    hold(health, 11, 10 + CLASSIC_BLAME_SECONDS, 0);
    const auto event = health.observe(11 + CLASSIC_BLAME_SECONDS, false, true, 1);
    EXPECT_FALSE(event);
    EXPECT_EQ(health.quiet_until(12 + CLASSIC_BLAME_SECONDS), -1);
}

TEST(LeHealth, ACloseIsBlamedTooAndOnlyOnce) {
    LeHealth health;
    health.observe(0, true, true, 0);
    health.note_classic(ClassicTouch::Closed, 5);
    auto event = health.observe(6, false, false, 1);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::ClassicBlamed);
    EXPECT_EQ(event->touch, ClassicTouch::Closed);
    // The next drop, a second later, is not the close's doing again.
    event = health.observe(7, false, false, 2);
    EXPECT_FALSE(event);
    EXPECT_EQ(health.quiet_until(7), 6 + CLASSIC_QUIET_SECONDS);
}

TEST(LeHealth, TheWaitDoublesWhenItHappensAgain) {
    LeHealth health;
    health.observe(0, true, false, 0);
    health.note_classic(ClassicTouch::Opened, 1);
    auto event = health.observe(2, false, false, 1);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->quiet_seconds, CLASSIC_QUIET_SECONDS);
    health.observe(400, true, false, 1);
    health.note_classic(ClassicTouch::Opened, 500);
    event = health.observe(501, false, false, 2);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->quiet_seconds, 2 * CLASSIC_QUIET_SECONDS);
    // Never beyond the ceiling.
    int64_t t = 501;
    uint64_t drops = 2;
    for (int i = 0; i < 6; ++i) {
        t += 100;
        health.observe(t, true, false, drops);
        health.note_classic(ClassicTouch::Opened, t + 1);
        event = health.observe(t + 2, false, false, ++drops);
        ASSERT_TRUE(event);
        EXPECT_LE(event->quiet_seconds, CLASSIC_QUIET_SECONDS_MAX);
    }
    EXPECT_EQ(event->quiet_seconds, CLASSIC_QUIET_SECONDS_MAX);
    // And forgotten after an hour without.
    t += CLASSIC_BLAME_MEMORY_SECONDS + 10;
    health.observe(t, true, false, drops);
    health.note_classic(ClassicTouch::Opened, t + 1);
    event = health.observe(t + 2, false, false, ++drops);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->quiet_seconds, CLASSIC_QUIET_SECONDS);
}

TEST(LeHealth, QuietSecondsZeroBlamesNothing) {
    LeHealth health;
    LeHealthConfig config;
    config.quiet_seconds = 0;
    health.configure(config);
    health.observe(0, true, false, 0);
    health.note_classic(ClassicTouch::Opened, 1);
    EXPECT_FALSE(health.observe(2, false, false, 1));
    EXPECT_EQ(health.quiet_until(2), -1);
}

// 18:04:26 to 18:05:45: a link every three or four seconds, none holding.
TEST(LeHealth, ABurstTakesTheAdvertOffAir) {
    LeHealth health;
    health.observe(0, false, false, 0);
    EXPECT_FALSE(health.observe(1, true, false, 1));
    EXPECT_FALSE(health.advert_paused(1));
    EXPECT_FALSE(health.observe(5, true, false, 2));
    const auto event = health.observe(9, false, false, 3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Burst);
    EXPECT_EQ(event->drops, 3);
    EXPECT_EQ(event->pause_seconds, LE_BURST_PAUSE_SECONDS);
    EXPECT_TRUE(health.advert_paused(9));
    EXPECT_TRUE(health.unsteady(9));
    EXPECT_TRUE(health.advert_paused(9 + LE_BURST_PAUSE_SECONDS - 1));
    EXPECT_FALSE(health.advert_paused(9 + LE_BURST_PAUSE_SECONDS));
    // Still unsteady until a link has held.
    EXPECT_TRUE(health.unsteady(9 + LE_BURST_PAUSE_SECONDS));
}

TEST(LeHealth, DropsFarApartAreNoBurst) {
    LeHealth health;
    health.observe(0, true, false, 0);
    EXPECT_FALSE(health.observe(10, true, false, 1));
    EXPECT_FALSE(health.observe(50, true, false, 2));
    EXPECT_FALSE(health.observe(10 + LE_BURST_WINDOW_SECONDS, true, false, 3));
    EXPECT_FALSE(health.advert_paused(10 + LE_BURST_WINDOW_SECONDS));
    EXPECT_FALSE(health.unsteady(10 + LE_BURST_WINDOW_SECONDS));
}

TEST(LeHealth, ThePauseDoublesThenIsForgottenOnceTheLinkHolds) {
    LeHealth health;
    health.observe(0, false, false, 0);
    auto event = health.observe(1, false, false, 3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 30);
    event = health.observe(40, false, false, 6);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 60);
    event = health.observe(110, false, false, 9);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 120);
    event = health.observe(240, false, false, 12);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, LE_BURST_PAUSE_SECONDS_MAX);

    // A link that holds: said once, and the next burst starts from 30 s.
    EXPECT_FALSE(health.observe(400, true, false, 12));
    event = health.observe(400 + LE_STABLE_SECONDS, true, false, 12);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Steady);
    EXPECT_FALSE(health.unsteady(400 + LE_STABLE_SECONDS));
    EXPECT_FALSE(health.observe(401 + LE_STABLE_SECONDS, true, false, 12));
    event = health.observe(500, false, false, 15);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->pause_seconds, 30);
}

TEST(LeHealth, BurstPauseOffKeepsTheAdvertOnAirButSaysSo) {
    LeHealth health;
    LeHealthConfig config;
    config.burst_pause = false;
    health.configure(config);
    health.observe(0, false, false, 0);
    const auto event = health.observe(1, false, false, 3);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Burst);
    EXPECT_EQ(event->pause_seconds, 0);
    EXPECT_FALSE(health.advert_paused(1));
    EXPECT_TRUE(health.unsteady(1));
}

TEST(LeHealth, ManySignalsInOnePassCountAsOneBurst) {
    LeHealth health;
    health.observe(0, false, false, 0);
    const auto event = health.observe(1, false, false, 40);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->drops, LE_BURST_DROPS);
    EXPECT_FALSE(health.observe(2, false, false, 40));
}

TEST(LeHealth, TheEdgeAloneCountsWhenNoSignalCame) {
    LeHealth health;
    health.observe(0, true, false, 0);
    hold(health, 1, 50, 0);
    health.note_classic(ClassicTouch::Opened, 50);
    const auto event = health.observe(51, false, false, 0);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::ClassicBlamed);
}

TEST(LeHealth, ASignalThenTheEdgeIsOneDrop) {
    LeHealth health;
    health.observe(0, true, false, 0);
    // The signal is counted while the property still reads connected.
    EXPECT_FALSE(health.observe(10, true, false, 1));
    EXPECT_FALSE(health.observe(11, false, false, 1));
    EXPECT_FALSE(health.observe(12, true, false, 1));
    EXPECT_FALSE(health.observe(13, false, false, 2));
    // Two drops, not four: no burst.
    EXPECT_FALSE(health.unsteady(13));
}

TEST(LeHealth, LostAfterABurstAndSeenOverClassic) {
    LeHealth health;
    health.observe(0, false, false, 0);
    ASSERT_TRUE(health.observe(10, false, false, 3));
    EXPECT_TRUE(health.lost_after_burst(12));
    EXPECT_TRUE(health.lost_after_burst(10 + LE_BURST_MEMORY_SECONDS));
    EXPECT_FALSE(health.lost_after_burst(11 + LE_BURST_MEMORY_SECONDS));
    EXPECT_FALSE(health.lost_after_burst(-1));
    EXPECT_FALSE(health.classic_seen_since(12));
    health.observe(14, false, true, 3);
    EXPECT_TRUE(health.classic_seen_since(12));
    EXPECT_FALSE(health.classic_seen_since(15));
}

TEST(LeHealth, ResetKeepsTheConfigurationAndTheCount) {
    LeHealth health;
    LeHealthConfig config;
    config.stable_seconds = 7;
    health.configure(config);
    health.observe(0, false, false, 0);
    ASSERT_TRUE(health.observe(1, false, false, 3));
    health.reset(3);
    EXPECT_EQ(health.config().stable_seconds, 7);
    EXPECT_FALSE(health.unsteady(2));
    EXPECT_FALSE(health.advert_paused(2));
    EXPECT_FALSE(health.observe(2, false, false, 3));
}

// 18:04 to 18:06 of 17 November, as the new rules would have seen it.
TEST(LeHealth, TheEveningOf17November) {
    LeHealth health;
    health.observe(0, false, false, 0);
    uint64_t drops = 0;
    int bursts = 0;
    int offered = 0;
    // A link comes up every 4 s for 80 s whenever the advert is on air, and dies.
    for (int64_t t = 1; t <= 80; ++t) {
        const bool tries = t % 4 == 0 && !health.advert_paused(t);
        if (tries) {
            ++offered;
            ++drops;
        }
        const auto event = health.observe(t, false, false, drops);
        if (event && event->kind == Kind::Burst)
            ++bursts;
        EXPECT_FALSE(health.auto_open_allowed(t));
    }
    // Twenty links that evening; a handful now.
    EXPECT_LE(offered, 8);
    EXPECT_GE(bursts, 1);
    // The link that at last holds: the sync waits for it to have held.
    int64_t up = 200;
    health.observe(up, true, false, drops);
    EXPECT_FALSE(health.auto_open_allowed(up + 4));
    EXPECT_TRUE(health.auto_open_allowed(up + LE_STABLE_SECONDS));
}

TEST(LeStateFragile, WaitingSaysUnsteady) {
    LeStateFacts facts = lost_phone();
    facts.now = 110;
    EXPECT_EQ(le_state_for(facts).cause, "reconnecting");
    facts.unsteady = true;
    EXPECT_EQ(le_state_for(facts), (LeState{"waiting", "unsteady", ""}));
}

TEST(LeStateFragile, AdvertWithheldIsWaitingNotOffAir) {
    LeStateFacts facts = lost_phone();
    facts.advert_registered = false;
    EXPECT_EQ(le_state_for(facts).cause, "advert_off_air");
    facts.advert_paused = true;
    facts.unsteady = true;
    EXPECT_EQ(le_state_for(facts), (LeState{"waiting", "unsteady", ""}));
}

TEST(LeStateFragile, PhoneBackoffWhenTheClassicLinkReachedIt) {
    LeStateFacts facts = lost_phone();
    facts.lost_after_burst = true;
    LeState state = le_state_for(facts);
    EXPECT_EQ(state.state, "paused");
    EXPECT_EQ(state.cause, "phone_away");
    EXPECT_NE(state.detail.find("several times in a row"), std::string::npos);

    facts.classic_seen_since_loss = true;
    state = le_state_for(facts);
    EXPECT_EQ(state.state, "paused");
    EXPECT_EQ(state.cause, "phone_backoff");
    EXPECT_NE(state.detail.find("Bluetooth off and back on"), std::string::npos);

    facts.classic_seen_since_loss = false;
    facts.classic_connected = true;
    EXPECT_EQ(le_state_for(facts).cause, "phone_backoff");

    // Without a burst nothing changes.
    facts.lost_after_burst = false;
    EXPECT_EQ(le_state_for(facts).cause, "phone_silent");
    facts.classic_connected = false;
    EXPECT_EQ(le_state_for(facts).detail, "Notifications are paused. The iPhone is out of reach.");
}
