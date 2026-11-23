#include <gtest/gtest.h>
#include <tether/bluetooth/le_first_use.hpp>

using namespace tether::bluetooth;

namespace {
    using Kind = LeFirstUse::Event::Kind;
}

TEST(LeFirstUse, EventPassesAreSpacedAndNeverLate) {
    EXPECT_EQ(event_pass_wait_ms(0), EVENT_PASS_GAP_MS);
    EXPECT_EQ(event_pass_wait_ms(40), EVENT_PASS_GAP_MS - 40);
    EXPECT_EQ(event_pass_wait_ms(EVENT_PASS_GAP_MS), 0);
    EXPECT_EQ(event_pass_wait_ms(5000), 0);
    // A clock that went backwards, a gap switched off: now.
    EXPECT_EQ(event_pass_wait_ms(-3), 0);
    EXPECT_EQ(event_pass_wait_ms(10, 0), 0);
}

TEST(LeFirstUse, PromptUnlessSwitchedOff) {
    EXPECT_TRUE(le_prompt_enabled(nullptr));
    EXPECT_TRUE(le_prompt_enabled(""));
    EXPECT_TRUE(le_prompt_enabled("1"));
    EXPECT_FALSE(le_prompt_enabled("0"));
    EXPECT_TRUE(le_prompt_enabled("00"));
}

TEST(LeFirstUse, DatesTheSubscriptionThenTheFirstAnswer) {
    LeFirstUse use;
    EXPECT_FALSE(use.observe(0, -1, false, false, false, 900));
    // The link came up at 1000 and is first looked at 30 ms later.
    EXPECT_FALSE(use.observe(1, 1000, true, false, false, 1030));
    EXPECT_FALSE(use.observe(1, 1000, true, false, false, 1130));
    auto event = use.observe(1, 1000, true, true, false, 1250);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Subscribed);
    EXPECT_EQ(event->after_ms, 250);
    // Said once.
    EXPECT_FALSE(use.observe(1, 1000, true, true, false, 1350));
    event = use.observe(1, 1000, true, true, true, 1600);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Live);
    EXPECT_EQ(event->after_ms, 600);
    EXPECT_EQ(event->after_subscribed_ms, 350);
    EXPECT_FALSE(use.observe(1, 1000, true, true, true, 1700));
}

TEST(LeFirstUse, SubscribedAndAnsweredInOnePassIsOneLine) {
    LeFirstUse use;
    use.observe(1, 1000, true, false, false, 1010);
    const auto event = use.observe(1, 1000, true, true, true, 1400);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Live);
    EXPECT_EQ(event->after_ms, 400);
    EXPECT_EQ(event->after_subscribed_ms, -1);
    EXPECT_FALSE(use.observe(1, 1000, true, true, true, 1500));
}

TEST(LeFirstUse, WhatTheLinkBeforeHadIsNotDatedOnTheNewOne) {
    LeFirstUse use;
    use.observe(1, 1000, true, false, false, 1010);
    ASSERT_TRUE(use.observe(1, 1000, true, true, true, 1400));
    // Dropped and back between two passes: the client still reads ready
    // from the link before in the pass that sees the new one, and after.
    EXPECT_FALSE(use.observe(2, 5000, true, true, true, 5020));
    EXPECT_FALSE(use.observe(2, 5000, true, true, true, 5120));
    // It has forgotten its session; then the new one comes to life.
    EXPECT_FALSE(use.observe(2, 5000, true, false, false, 5220));
    const auto event = use.observe(2, 5000, true, true, true, 5900);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, Kind::Live);
    EXPECT_EQ(event->after_ms, 900);
}

TEST(LeFirstUse, NothingIsDatedOnALinkThatIsDownOrWasNeverSeenComingUp) {
    LeFirstUse use;
    // Up before this process looked: no date to count from.
    EXPECT_FALSE(use.observe(0, -1, true, true, true, 100));
    use.observe(1, 1000, true, false, false, 1010);
    // The property reads down while BlueZ still answers for the old tree.
    EXPECT_FALSE(use.observe(1, 1000, false, true, false, 1100));
}
