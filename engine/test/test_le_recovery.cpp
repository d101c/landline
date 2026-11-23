#include <gtest/gtest.h>
#include <tether/bluetooth/le_recovery.hpp>

using namespace tether::bluetooth;

namespace {
    LeRecoveryFacts at(int64_t now, bool le, bool classic, bool busy = false) {
        LeRecoveryFacts f;
        f.now = now;
        f.le_connected = le;
        f.classic_connected = classic;
        f.busy = busy;
        return f;
    }
} // namespace

// The iPhone near, the LE link gone, nothing over the classic link: probes at
// 1, 3, 10 minutes, then every 15.
TEST(LeRecovery, ProbesWithABackoff) {
    LeRecovery r;
    r.step(at(0, true, false));
    std::vector<int64_t> probes;
    for (int64_t t = 1; t <= 3600; ++t)
        if (r.step(at(t, false, false)).probe)
            probes.push_back(t);
    ASSERT_GE(probes.size(), 5u);
    // A minute after the loss, then 3 and 10 minutes after the last, then 15.
    EXPECT_EQ(probes[0], 61);
    EXPECT_EQ(probes[1], 61 + 180);
    EXPECT_EQ(probes[2], 61 + 180 + 600);
    EXPECT_EQ(probes[3], 61 + 180 + 600 + 900);
    EXPECT_EQ(probes[4], 61 + 180 + 600 + 900 + 900);
}

TEST(LeRecovery, NoProbeWhenTheIphoneWasLastSeenLongAgo) {
    LeRecovery r;
    r.step(at(0, true, false));
    bool probed_late = false;
    for (int64_t t = 1; t <= 4 * 3600; t += 10) {
        const bool p = r.step(at(t, false, false)).probe;
        if (p && t > LE_RECOVERY_NEAR_SECONDS)
            probed_late = true;
    }
    EXPECT_FALSE(probed_late);
}

// 23 November: the iPhone over the classic link, never over LE.
TEST(LeRecovery, DialsWhileTheIphoneIsThereThenNamesTheStuckState) {
    LeRecovery r;
    r.step(at(0, true, false));
    int dials = 0;
    int64_t stuck_at = -1;
    for (int64_t t = 1; t <= 600; ++t) {
        const auto s = r.step(at(t, false, true));
        dials += s.dial;
        if (s.stuck_started)
            stuck_at = t;
        EXPECT_FALSE(s.probe);
    }
    EXPECT_EQ(dials, 5);
    EXPECT_EQ(stuck_at, 1 + 2 * LE_RECOVERY_DIAL_SECONDS);
    EXPECT_TRUE(r.stuck());
    const auto back = r.step(at(601, true, false));
    EXPECT_TRUE(back.stuck_ended);
    EXPECT_FALSE(r.stuck());
}

TEST(LeRecovery, NothingDuringACall) {
    LeRecovery r;
    r.step(at(0, true, false));
    for (int64_t t = 1; t <= 600; ++t) {
        const auto s = r.step(at(t, false, true, true));
        EXPECT_FALSE(s.dial || s.probe || s.stuck_started) << t;
    }
}

TEST(LeRecovery, AnUnansweredProbeIsNotStuck) {
    LeRecovery r;
    r.step(at(0, true, false));
    for (int64_t t = 1; t <= 3600; ++t)
        r.step(at(t, false, false));
    EXPECT_FALSE(r.stuck());
    EXPECT_EQ(r.dials(), 0);
}

TEST(LeRecovery, AtStartTheIphoneCountsAsNear) {
    LeRecovery r;
    bool probed = false;
    for (int64_t t = 0; t <= 120; ++t)
        probed |= r.step(at(t, false, false)).probe;
    EXPECT_TRUE(probed);
}
