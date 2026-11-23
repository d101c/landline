#include <gtest/gtest.h>
#include <tether/bluetooth/le_after_reset.hpp>

using namespace tether::bluetooth;

namespace {
    constexpr int64_t S = 1000;
    constexpr int64_t MIN = 60 * S;
} // namespace

// The evening of #59: the link was up, flapped, the controller was reset,
// and the iPhone never came back.
TEST(LeAfterReset, ThePhoneThatWasNearAndNeverCameBack) {
    LeAfterReset rule;
    EXPECT_FALSE(rule.observe(0, -1, true));
    EXPECT_FALSE(rule.observe(30 * S, -1, false));
    const int64_t back = 60 * S;
    EXPECT_FALSE(rule.observe(back, back, false));
    EXPECT_FALSE(rule.observe(back + 2 * MIN, back, false));
    EXPECT_TRUE(rule.observe(back + 3 * MIN, back, false));
    EXPECT_TRUE(rule.observe(back + 60 * MIN, back, false));
    // Long after, the hint goes.
    EXPECT_FALSE(rule.observe(back + 2 * 60 * MIN, back, false));
}

TEST(LeAfterReset, GoneAsSoonAsTheLinkIsBack) {
    LeAfterReset rule;
    rule.observe(0, -1, true);
    EXPECT_TRUE(rule.observe(5 * MIN, S, false));
    EXPECT_FALSE(rule.observe(5 * MIN + S, S, true));
    // A later drop is not the reset's doing.
    EXPECT_FALSE(rule.observe(20 * MIN, S, false));
}

TEST(LeAfterReset, NothingWhenThePhoneWasAwayBeforeTheReset) {
    LeAfterReset rule;
    rule.observe(0, -1, true);
    const int64_t back = 30 * MIN;
    EXPECT_FALSE(rule.observe(back + 5 * MIN, back, false));
}

TEST(LeAfterReset, NothingWithoutAResetOrAnyLinkSeen) {
    LeAfterReset never_linked;
    EXPECT_FALSE(never_linked.observe(10 * MIN, MIN, false));
    LeAfterReset no_reset;
    no_reset.observe(0, -1, true);
    EXPECT_FALSE(no_reset.observe(10 * MIN, -1, false));
}

TEST(LeAfterResetStep, RestartOnceWhenNothingElseIsConnected) {
    EXPECT_EQ(after_reset_step(false, false, 10 * MIN, -1), AfterResetStep::None);
    EXPECT_EQ(after_reset_step(true, false, 10 * MIN, -1), AfterResetStep::Restart);
    // Restarted: time for the iPhone to come back, then its turn.
    EXPECT_EQ(after_reset_step(true, false, 10 * MIN + 2 * MIN, 10 * MIN), AfterResetStep::None);
    EXPECT_EQ(after_reset_step(true, false, 10 * MIN + 3 * MIN, 10 * MIN), AfterResetStep::AskPhone);
}

TEST(LeAfterResetStep, HeadphonesAreNotCutWithoutAsking) {
    EXPECT_EQ(after_reset_step(true, true, 10 * MIN, -1), AfterResetStep::AskComputer);
}
