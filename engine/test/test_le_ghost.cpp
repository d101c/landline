#include <gtest/gtest.h>
#include <tether/bluetooth/le_ghost.hpp>

using namespace tether::bluetooth;

// 23 November: BlueZ up, the controller holding nothing, no notifications.
TEST(LeGhost, TwoMissesInARowResyncBlueZ) {
    LeGhost g;
    int64_t t = 0;
    for (; t < LE_GHOST_WAIT_SECONDS; ++t)
        EXPECT_FALSE(g.check_due(t, true, false)) << t;
    ASSERT_TRUE(g.check_due(t, true, false));
    EXPECT_FALSE(g.listed(t, false));
    EXPECT_FALSE(g.check_due(t + 1, true, false));
    t += LE_GHOST_CHECK_SECONDS;
    ASSERT_TRUE(g.check_due(t, true, false));
    EXPECT_TRUE(g.listed(t, false));
    EXPECT_EQ(g.resyncs(), 1);
    // A fresh wait before another verdict.
    EXPECT_FALSE(g.check_due(t + 1, true, false));
}

TEST(LeGhost, NothingWhileTheNotificationsWork) {
    LeGhost g;
    for (int64_t t = 0; t < 600; ++t)
        EXPECT_FALSE(g.check_due(t, true, true));
}

TEST(LeGhost, ALinkTheKernelListsResetsTheCount) {
    LeGhost g;
    int64_t t = LE_GHOST_WAIT_SECONDS;
    g.check_due(0, true, false);
    ASSERT_TRUE(g.check_due(t, true, false));
    EXPECT_FALSE(g.listed(t, false));
    t += LE_GHOST_CHECK_SECONDS;
    ASSERT_TRUE(g.check_due(t, true, false));
    EXPECT_FALSE(g.listed(t, true));
    t += LE_GHOST_CHECK_SECONDS;
    ASSERT_TRUE(g.check_due(t, true, false));
    EXPECT_FALSE(g.listed(t, false));
}

TEST(LeGhost, AnUnreadableListProvesNothing) {
    LeGhost g;
    g.check_due(0, true, false);
    for (int64_t t = LE_GHOST_WAIT_SECONDS; t < 600; t += LE_GHOST_CHECK_SECONDS) {
        ASSERT_TRUE(g.check_due(t, true, false));
        EXPECT_FALSE(g.listed(t, std::nullopt)) << t;
    }
    EXPECT_EQ(g.resyncs(), 0);
}

TEST(LeGhost, NotTwiceWithinFiveMinutes) {
    LeGhost g;
    g.check_due(0, true, false);
    int64_t t = LE_GHOST_WAIT_SECONDS;
    g.check_due(t, true, false);
    g.listed(t, false);
    t += LE_GHOST_CHECK_SECONDS;
    g.check_due(t, true, false);
    ASSERT_TRUE(g.listed(t, false));
    const int64_t resync = t;
    for (++t; t < resync + LE_GHOST_MIN_GAP_SECONDS; ++t)
        EXPECT_FALSE(g.check_due(t, true, false)) << t;
}

// From the review: a count left from a link long gone must not cut short the
// wait on the next one.
TEST(LeGhost, ForgetStartsAfresh) {
    LeGhost g;
    g.check_due(0, true, false);
    ASSERT_TRUE(g.check_due(LE_GHOST_WAIT_SECONDS, true, false));
    g.listed(LE_GHOST_WAIT_SECONDS, false);
    g.forget();
    const int64_t back = 3600;
    for (int64_t t = back; t < back + LE_GHOST_WAIT_SECONDS; ++t)
        EXPECT_FALSE(g.check_due(t, true, false)) << t;
}
