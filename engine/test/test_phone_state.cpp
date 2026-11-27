#include <gtest/gtest.h>

#include <tether/bluetooth/phone_state.hpp>

using namespace tether::bluetooth;

TEST(BatteryWatch, SaysNothingAboveTheThresholds) {
    BatteryWatch watch;
    for (int level : {100, 60, 30, 21})
        EXPECT_FALSE(watch.observe(level)) << level;
}

TEST(BatteryWatch, AnnouncesTwentyThenTenOnceEach) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(23));

    const auto low = watch.observe(20);
    ASSERT_TRUE(low);
    EXPECT_EQ(low->threshold, 20);
    EXPECT_EQ(low->level, 20);
    EXPECT_FALSE(low->initial);
    for (int level : {19, 18, 15, 11})
        EXPECT_FALSE(watch.observe(level)) << level;

    const auto critical = watch.observe(10);
    ASSERT_TRUE(critical);
    EXPECT_EQ(critical->threshold, 10);
    for (int level : {9, 5, 1, 0})
        EXPECT_FALSE(watch.observe(level)) << level;
}

// A phone found again after hours away may have dropped through both.
TEST(BatteryWatch, ADropThroughBothAnnouncesOnlyTheLowerOne) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(40));
    const auto alert = watch.observe(8);
    ASSERT_TRUE(alert);
    EXPECT_EQ(alert->threshold, 10);
    EXPECT_FALSE(watch.observe(7));
}

// 20, 21, 20, 21: one announcement, not one per flicker.
TEST(BatteryWatch, HoveringAtAThresholdAnnouncesOnce) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(21));
    EXPECT_TRUE(watch.observe(20));
    for (int level : {21, 20, 22, 19, 24, 20})
        EXPECT_FALSE(watch.observe(level)) << level;
}

TEST(BatteryWatch, RearmsOnceChargedPastTheMargin) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(30));
    EXPECT_TRUE(watch.observe(18));
    EXPECT_FALSE(watch.observe(BATTERY_LOW_PERCENT + BATTERY_REARM_MARGIN - 1));
    EXPECT_FALSE(watch.observe(BATTERY_LOW_PERCENT + BATTERY_REARM_MARGIN));
    const auto again = watch.observe(20);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->threshold, 20);
}

TEST(BatteryWatch, TheCriticalThresholdRearmsOnItsOwn) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(30));
    EXPECT_TRUE(watch.observe(20));
    EXPECT_TRUE(watch.observe(9));
    // Charged a little: back in the low band, which was already announced.
    EXPECT_FALSE(watch.observe(BATTERY_CRITICAL_PERCENT + BATTERY_REARM_MARGIN));
    EXPECT_FALSE(watch.observe(17));
    const auto again = watch.observe(10);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->threshold, 10);
}

// The link dropping reads as "unknown"; the phone coming back at the same
// level is not a new crossing.
TEST(BatteryWatch, AnUnknownLevelNeitherAnnouncesNorRearms) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(25));
    EXPECT_TRUE(watch.observe(15));
    EXPECT_FALSE(watch.observe(-1));
    EXPECT_FALSE(watch.observe(15));
    EXPECT_FALSE(watch.observe(-1));
    EXPECT_FALSE(watch.observe(14));
}

TEST(BatteryWatch, AFirstReadingAlreadyLowIsMarkedInitial) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(-1));
    const auto alert = watch.observe(12);
    ASSERT_TRUE(alert);
    EXPECT_EQ(alert->threshold, 20);
    EXPECT_TRUE(alert->initial);

    const auto next = watch.observe(10);
    ASSERT_TRUE(next);
    EXPECT_FALSE(next->initial);
}

// Hands-free reports 0-5, published as 20 % steps: 20 is its lowest non-zero value.
TEST(BatteryWatch, TheHandsFreeScaleReachesOnlyTheFirstThreshold) {
    BatteryWatch watch;
    EXPECT_FALSE(watch.observe(60));
    EXPECT_FALSE(watch.observe(40));
    const auto alert = watch.observe(20);
    ASSERT_TRUE(alert);
    EXPECT_EQ(alert->threshold, 20);
    EXPECT_FALSE(watch.observe(20));
}

TEST(BatteryWatch, ResetForgetsWhatWasAnnounced) {
    BatteryWatch watch;
    EXPECT_TRUE(watch.observe(5));
    watch.reset();
    const auto alert = watch.observe(5);
    ASSERT_TRUE(alert);
    EXPECT_TRUE(alert->initial);
}

TEST(BatteryState, NamesTheBands) {
    EXPECT_STREQ(battery_state(-1), "");
    EXPECT_STREQ(battery_state(100), "ok");
    EXPECT_STREQ(battery_state(21), "ok");
    EXPECT_STREQ(battery_state(20), "low");
    EXPECT_STREQ(battery_state(11), "low");
    EXPECT_STREQ(battery_state(10), "critical");
    EXPECT_STREQ(battery_state(0), "critical");
}

namespace {
    constexpr int64_t MINUTE = 60'000;
}

TEST(ChargeWatch, SaysNothingBeforeTheLevelHasMoved) {
    ChargeWatch watch;
    EXPECT_FALSE(watch.observe(50, "le", 0));
    EXPECT_FALSE(watch.observe(50, "le", 5 * MINUTE));
}

TEST(ChargeWatch, ARisingLevelIsAPhoneOnCharge) {
    ChargeWatch watch;
    watch.observe(20, "le", 0);
    EXPECT_EQ(watch.observe(21, "le", 2 * MINUTE), std::optional<bool>(true));
    // Steady between two points: still charging.
    EXPECT_EQ(watch.observe(21, "le", 3 * MINUTE), std::optional<bool>(true));
    EXPECT_EQ(watch.observe(22, "le", 4 * MINUTE), std::optional<bool>(true));
}

TEST(ChargeWatch, AFallingLevelIsAPhoneOffCharge) {
    ChargeWatch watch;
    watch.observe(20, "le", 0);
    watch.observe(21, "le", 2 * MINUTE);
    EXPECT_EQ(watch.observe(20, "le", 30 * MINUTE), std::optional<bool>(false));
    // A phone at rest holds a point for a long time: still off charge.
    EXPECT_EQ(watch.observe(20, "le", 120 * MINUTE), std::optional<bool>(false));
}

TEST(ChargeWatch, ARiseThatStopsIsNoLongerKnown) {
    ChargeWatch watch;
    watch.observe(99, "le", 0);
    EXPECT_EQ(watch.observe(100, "le", 5 * MINUTE), std::optional<bool>(true));
    EXPECT_EQ(watch.observe(100, "le", 5 * MINUTE + CHARGE_RISE_VALID_MS), std::optional<bool>(true));
    // Full and plugged, or unplugged and not yet a point lower: no telling.
    EXPECT_FALSE(watch.observe(100, "le", 5 * MINUTE + CHARGE_RISE_VALID_MS + 1));
}

TEST(ChargeWatch, AnAbsenceForgetsEverything) {
    ChargeWatch watch;
    watch.observe(20, "le", 0);
    watch.observe(21, "le", MINUTE);
    EXPECT_FALSE(watch.observe(-1, "", 2 * MINUTE));
    // Higher than before the absence: charged meanwhile, perhaps not now.
    EXPECT_FALSE(watch.observe(60, "le", 90 * MINUTE));
}

TEST(ChargeWatch, TheHandsFreeLevelIsTooCoarse) {
    ChargeWatch watch;
    watch.observe(40, "hfp", 0);
    EXPECT_FALSE(watch.observe(60, "hfp", 20 * MINUTE));
    // And a change of source is not a change of level.
    EXPECT_FALSE(watch.observe(63, "le", 21 * MINUTE));
    EXPECT_EQ(watch.observe(64, "le", 23 * MINUTE), std::optional<bool>(true));
}

TEST(WifiCalling, IsReadOffTheNetworkName) {
    EXPECT_TRUE(operator_says_wifi_calling("Carrier WiFi-Calling"));
    EXPECT_TRUE(operator_says_wifi_calling("Wi-Fi Calling"));
    EXPECT_TRUE(operator_says_wifi_calling("carrier wifi call"));
    EXPECT_FALSE(operator_says_wifi_calling("Carrier"));
    EXPECT_FALSE(operator_says_wifi_calling("Free WiFi"));
    EXPECT_FALSE(operator_says_wifi_calling(""));
}
