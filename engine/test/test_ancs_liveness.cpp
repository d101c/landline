#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/liveness.hpp>

using namespace tether::bluetooth::ancs;
using Action = AncsLiveness::Action;

TEST(AncsLiveness, TheAnswersOfThePhone) {
    EXPECT_EQ(classify_probe(true, false, ""), ProbeAnswer::Alive);
    // What BlueZ says when iOS refuses the UID: an answer, so alive.
    EXPECT_EQ(classify_probe(false, false, "GDBus.Error:org.bluez.Error.Failed: Operation failed with ATT error: 0xa2"),
              ProbeAnswer::Alive);
    EXPECT_EQ(classify_probe(false, true, "Timeout was reached"), ProbeAnswer::Silent);
    EXPECT_EQ(classify_probe(false, false, "GDBus.Error:org.freedesktop.DBus.Error.NoReply: Timeout was reached"),
              ProbeAnswer::Silent);
    EXPECT_EQ(classify_probe(false, false, "GDBus.Error:org.bluez.Error.NotConnected: Not connected"),
              ProbeAnswer::Inconclusive);
}

TEST(AncsLiveness, ConfigFromTheEnvironment) {
    LivenessConfig config = liveness_config_from(nullptr, nullptr, nullptr);
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.probe_seconds, 900);
    EXPECT_EQ(config.quiet_seconds, 6 * 3600);
    EXPECT_FALSE(liveness_config_from("0", nullptr, nullptr).enabled);
    EXPECT_EQ(liveness_config_from(nullptr, "120", "3600").probe_seconds, 120);
    EXPECT_EQ(liveness_config_from(nullptr, "120", "3600").quiet_seconds, 3600);
    EXPECT_EQ(liveness_config_from(nullptr, "5", "60").probe_seconds, 900);
    EXPECT_EQ(liveness_config_from(nullptr, "5", "60").quiet_seconds, 6 * 3600);
    EXPECT_EQ(liveness_config_from(nullptr, "soon", "99999999999").probe_seconds, 900);
}

TEST(AncsLiveness, NothingBeforeTheSessionWorks) {
    AncsLiveness liveness;
    for (int64_t t = 0; t < 100000; t += 100)
        EXPECT_EQ(liveness.due(t, false), Action::None);
}

TEST(AncsLiveness, AProbeEveryFifteenMinutesAndTheSilenceRebuilds) {
    AncsLiveness liveness;
    liveness.ready(0);
    for (int64_t t = 0; t < 900; ++t)
        EXPECT_EQ(liveness.due(t, false), Action::None) << t;
    ASSERT_EQ(liveness.due(900, false), Action::Probe);
    EXPECT_EQ(liveness.probe_answer(900, ProbeAnswer::Alive), Action::None);
    EXPECT_EQ(liveness.due(901, false), Action::None);
    // Waits for a request in flight.
    EXPECT_EQ(liveness.due(1800, true), Action::None);
    ASSERT_EQ(liveness.due(1801, false), Action::Probe);
    EXPECT_EQ(liveness.probe_answer(1806, ProbeAnswer::Silent), Action::ResubscribeSilent);
    EXPECT_FALSE(liveness.live());
    // Nothing more until the session works again.
    EXPECT_EQ(liveness.due(5000, false), Action::None);
    liveness.ready(5000);
    EXPECT_EQ(liveness.due(5899, false), Action::None);
    EXPECT_EQ(liveness.due(5900, false), Action::Probe);
}

TEST(AncsLiveness, AnUnclearAnswerIsAskedAgainInAMinute) {
    AncsLiveness liveness;
    liveness.ready(0);
    ASSERT_EQ(liveness.due(900, false), Action::Probe);
    EXPECT_EQ(liveness.probe_answer(900, ProbeAnswer::Inconclusive), Action::None);
    EXPECT_EQ(liveness.due(959, false), Action::None);
    EXPECT_EQ(liveness.due(960, false), Action::Probe);
}

TEST(AncsLiveness, NotificationsAreProofOfLifeAndSixQuietHoursRebuild) {
    AncsLiveness liveness;
    liveness.ready(0);
    liveness.packet(800);
    EXPECT_EQ(liveness.due(900, false), Action::None);
    EXPECT_EQ(liveness.due(1700, false), Action::Probe);
    liveness.probe_answer(1700, ProbeAnswer::Alive);
    // The probe answered all along, and no notification for six hours.
    int64_t t = 1701;
    for (; t < 800 + 6 * 3600; ++t) {
        const Action action = liveness.due(t, false);
        if (action == Action::Probe)
            liveness.probe_answer(t, ProbeAnswer::Alive);
        else
            ASSERT_EQ(action, Action::None) << t;
    }
    EXPECT_EQ(liveness.due(800 + 6 * 3600, false), Action::ResubscribeQuiet);
    EXPECT_FALSE(liveness.live());
}

TEST(AncsLiveness, SwitchedOff) {
    AncsLiveness liveness;
    LivenessConfig config;
    config.enabled = false;
    liveness.configure(config);
    liveness.ready(0);
    for (int64_t t = 0; t < 30000; t += 60)
        EXPECT_EQ(liveness.due(t, false), Action::None);
}
