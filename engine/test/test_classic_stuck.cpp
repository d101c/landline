#include <gtest/gtest.h>
#include <tether/bluetooth/classic_stuck.hpp>

using namespace tether::bluetooth;
using Step = ClassicStuck::Step;

// 24 November, second call: "In Progress" at 16:02:55, :57, 16:03:00, ...
TEST(ClassicStuck, InProgressWithNoLinkIsClearedInTwoSteps) {
    ClassicStuck s;
    EXPECT_EQ(s.observe(0, 1, false, true, true), Step::None);
    EXPECT_EQ(s.observe(3, 2, false, true, true), Step::None);
    EXPECT_EQ(s.observe(6, 3, false, true, true), Step::Disconnect);
    EXPECT_TRUE(s.recovering());
    // The LE link has not dropped yet: it is not the one that came back.
    EXPECT_EQ(s.observe(7, 3, false, true, true), Step::None);
    EXPECT_EQ(s.observe(8, 0, false, false, true), Step::None);
    EXPECT_EQ(s.observe(10, 0, false, true, true), Step::Connect);
    // Said again until the call went out.
    EXPECT_EQ(s.observe(11, 0, false, true, true), Step::Connect);
    s.asked();
    EXPECT_FALSE(s.recovering());
    EXPECT_EQ(s.observe(12, 0, false, true, true), Step::None);
    EXPECT_EQ(s.recoveries(), 1);
}

TEST(ClassicStuck, AConnectThatIsReallyInProgressIsLeftAlone) {
    ClassicStuck s;
    // A page: two answers, then the link.
    EXPECT_EQ(s.observe(0, 1, false, true, true), Step::None);
    EXPECT_EQ(s.observe(3, 2, false, true, true), Step::None);
    EXPECT_EQ(s.observe(5, 2, true, true, true), Step::None);
    // The count starts again from there.
    EXPECT_EQ(s.observe(6, 3, false, true, true), Step::None);
    EXPECT_EQ(s.observe(9, 4, false, true, true), Step::None);
    EXPECT_EQ(s.observe(12, 5, false, true, true), Step::Disconnect);
}

TEST(ClassicStuck, ManyAnswersAtOnceAreNotEnough) {
    ClassicStuck s;
    EXPECT_EQ(s.observe(0, 5, false, true, true), Step::None);
    EXPECT_EQ(s.observe(CLASSIC_STUCK_SECONDS - 1, 6, false, true, true), Step::None);
    EXPECT_EQ(s.observe(CLASSIC_STUCK_SECONDS, 6, false, true, true), Step::Disconnect);
}

TEST(ClassicStuck, AnotherAnswerEndsTheCount) {
    ClassicStuck s;
    EXPECT_EQ(s.observe(0, 2, false, true, true), Step::None);
    EXPECT_EQ(s.observe(3, 0, false, true, true), Step::None);
    EXPECT_EQ(s.observe(6, 3, false, true, true), Step::None);
}

TEST(ClassicStuck, NothingIsDoneToAPhoneBeingPaired) {
    ClassicStuck s;
    for (int64_t t = 0; t < 60; t += 3)
        EXPECT_EQ(s.observe(t, static_cast<int>(t / 3) + 1, false, true, false), Step::None) << t;
}

TEST(ClassicStuck, ALinkThatNeverDropsIsTakenAfterAWhile) {
    ClassicStuck s;
    s.observe(0, 3, false, true, true);
    ASSERT_EQ(s.observe(6, 3, false, true, true), Step::Disconnect);
    EXPECT_EQ(s.observe(6 + CLASSIC_STUCK_SETTLE_SECONDS - 1, 3, false, true, true), Step::None);
    EXPECT_EQ(s.observe(6 + CLASSIC_STUCK_SETTLE_SECONDS, 3, false, true, true), Step::Connect);
}

TEST(ClassicStuck, GivesUpWhenTheLeLinkStaysAway) {
    ClassicStuck s;
    s.observe(0, 3, false, true, true);
    ASSERT_EQ(s.observe(6, 3, false, true, true), Step::Disconnect);
    EXPECT_EQ(s.observe(6 + CLASSIC_STUCK_GIVE_UP_SECONDS - 1, 0, false, false, true), Step::None);
    EXPECT_EQ(s.observe(6 + CLASSIC_STUCK_GIVE_UP_SECONDS, 0, false, false, true), Step::GaveUp);
    EXPECT_FALSE(s.recovering());
}

TEST(ClassicStuck, NotTwiceInFiveMinutes) {
    ClassicStuck s;
    s.observe(0, 3, false, true, true);
    ASSERT_EQ(s.observe(6, 3, false, true, true), Step::Disconnect);
    s.observe(8, 0, false, false, true);
    ASSERT_EQ(s.observe(10, 0, false, true, true), Step::Connect);
    s.asked();
    // Stuck again at once.
    int answers = 0;
    for (int64_t t = 12; t < 6 + CLASSIC_STUCK_MIN_GAP_SECONDS; t += 3)
        EXPECT_EQ(s.observe(t, ++answers, false, true, true), Step::None) << t;
    EXPECT_EQ(s.observe(6 + CLASSIC_STUCK_MIN_GAP_SECONDS, ++answers, false, true, true), Step::Disconnect);
    EXPECT_EQ(s.recoveries(), 2);
}
