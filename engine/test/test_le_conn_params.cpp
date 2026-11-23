#include <gtest/gtest.h>
#include <tether/bluetooth/le_conn_params.hpp>
#include <tether/bluetooth/le_conn_update.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace tether::bluetooth;

namespace {

    LeConnParamsFacts at(int64_t now, bool connected, uint64_t link, bool busy = false) {
        LeConnParamsFacts facts;
        facts.now = now;
        facts.le_connected = connected;
        facts.link = link;
        facts.busy = busy;
        return facts;
    }

    LeConnParamsPlan enabled_plan() {
        LeConnParamsPlan plan;
        LeConnParamsConfig config;
        config.enabled = true;
        plan.configure(config);
        return plan;
    }

    LeConnUpdateResult applied(int timeout_ms, int interval_ms = 60) {
        LeConnUpdateResult result;
        result.outcome = LeConnUpdateOutcome::Applied;
        result.status = 0;
        result.handle = 3585;
        result.got.min_interval_ms = result.got.max_interval_ms = interval_ms;
        result.got.latency = 0;
        result.got.timeout_ms = timeout_ms;
        return result;
    }

    LeConnUpdateResult outcome(LeConnUpdateOutcome what, int status = -1) {
        LeConnUpdateResult result;
        result.outcome = what;
        result.status = status;
        return result;
    }

    LeConnUpdateResult changed(int timeout_ms, int interval_ms) {
        LeConnUpdateResult result = applied(timeout_ms, interval_ms);
        result.outcome = LeConnUpdateOutcome::Changed;
        return result;
    }

} // namespace

// Pebble's values of October 2026 without the latency, the ones the iPhone
// applied on 17 November: 0x24-0x30, 0, 0x258.
TEST(LeConnParams, TheDefaultsAreWhatAnIphoneTakes) {
    const LeConnParams defaults;
    EXPECT_EQ(defaults.min_interval_ms, 45);
    EXPECT_EQ(defaults.max_interval_ms, 60);
    EXPECT_EQ(le_interval_units(defaults.min_interval_ms), 0x24);
    EXPECT_EQ(le_interval_units(defaults.max_interval_ms), 0x30);
    EXPECT_EQ(le_timeout_units(defaults.timeout_ms), 0x258);
    EXPECT_EQ(defaults.latency, 0);
    EXPECT_EQ(defaults.timeout_ms, 6000);
    EXPECT_EQ(le_conn_params_fault(defaults), "");
}

// Accessory Design Guidelines R32 Update 1, 57.6.
TEST(LeConnParams, ApplesRules) {
    const auto fault = [](int min, int max, int latency, int timeout) {
        return le_conn_params_fault(LeConnParams{min, max, latency, timeout});
    };
    // The trial of the documentation: hcitool lecup --min 24 --max 36 is 30 to 45 ms.
    EXPECT_EQ(fault(30, 45, 0, 6000), "");
    EXPECT_EQ(fault(15, 15, 0, 6000), "");
    EXPECT_EQ(fault(15, 30, 0, 18000), "");
    EXPECT_EQ(fault(300, 315, 5, 18000), "");

    EXPECT_NE(fault(0, 15, 0, 6000), "");
    EXPECT_NE(fault(20, 40, 0, 6000), "");   // not a multiple of 15 ms
    EXPECT_NE(fault(30, 40, 0, 6000), "");   // max under min + 15
    EXPECT_NE(fault(30, 30, 0, 6000), "");   // equal only at 15/15
    EXPECT_NE(fault(2010, 2040, 0, 18000), ""); // min over 2 s
    EXPECT_NE(fault(30, 45, 31, 6000), "");  // latency over 30
    EXPECT_NE(fault(30, 45, -1, 6000), "");
    // The rule of 2017 (2 to 6 s) is no longer the one in force.
    EXPECT_NE(fault(30, 45, 0, 2000), "");
    EXPECT_NE(fault(30, 45, 0, 5990), "");
    EXPECT_NE(fault(30, 45, 0, 18010), "");
    // Interval max x (latency + 1) of 6 s or less.
    EXPECT_NE(fault(600, 615, 10, 18000), "");
    // Supervision over interval max x (latency + 1) x 3.
    EXPECT_NE(fault(1500, 2000, 0, 6000), "");
    EXPECT_EQ(fault(1500, 1995, 0, 6000), "");
}

TEST(LeConnParams, OnUnlessTurnedOff) {
    LeConnParamsConfig config = le_conn_params_config_from(nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.wanted, LeConnParams{});
    EXPECT_EQ(config.delay_seconds, 0);
    EXPECT_TRUE(config.wait_encryption);
    EXPECT_TRUE(config.note.empty());

    EXPECT_FALSE(le_conn_params_config_from("0", nullptr, nullptr, nullptr, nullptr).enabled);
    EXPECT_TRUE(le_conn_params_config_from("1", nullptr, nullptr, nullptr, nullptr).enabled);
    EXPECT_TRUE(le_conn_params_config_from("", nullptr, nullptr, nullptr, nullptr).enabled);
    EXPECT_FALSE(le_conn_params_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, "0").wait_encryption);
    EXPECT_TRUE(le_conn_params_config_from(nullptr, nullptr, nullptr, nullptr, nullptr, "1").wait_encryption);
}

TEST(LeConnParams, ValuesFromTheEnvironment) {
    LeConnParamsConfig config = le_conn_params_config_from("1", "30-45", "0", "6000", "8");
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.wanted, (LeConnParams{30, 45, 0, 6000}));
    EXPECT_EQ(config.delay_seconds, 8);
    EXPECT_TRUE(config.note.empty());

    // A set an iPhone would refuse is not sent: the defaults are.
    config = le_conn_params_config_from("1", "30-45", "0", "2000", nullptr);
    EXPECT_EQ(config.wanted, LeConnParams{});
    EXPECT_FALSE(config.note.empty());
    config = le_conn_params_config_from("1", "150", nullptr, nullptr, nullptr);
    EXPECT_EQ(config.wanted, LeConnParams{});
    EXPECT_FALSE(config.note.empty());
    config = le_conn_params_config_from("1", "fast-slow", nullptr, nullptr, "soon");
    EXPECT_EQ(config.wanted, LeConnParams{});
    EXPECT_FALSE(config.note.empty());
    EXPECT_EQ(config.delay_seconds, LE_CONN_PARAMS_DELAY_SECONDS);
    // Pebble's own set, latency 4, is one an iPhone takes.
    config = le_conn_params_config_from(nullptr, "45-60", "4", "6000", nullptr);
    EXPECT_EQ(config.wanted, (LeConnParams{45, 60, 4, 6000}));
    config = le_conn_params_config_from("1", nullptr, nullptr, nullptr, "601");
    EXPECT_EQ(config.delay_seconds, LE_CONN_PARAMS_DELAY_SECONDS);
}

TEST(LeConnParams, ControllerIndex) {
    EXPECT_EQ(hci_index_from("hci0"), 0);
    EXPECT_EQ(hci_index_from("hci12"), 12);
    EXPECT_FALSE(hci_index_from(""));
    EXPECT_FALSE(hci_index_from("hci"));
    EXPECT_FALSE(hci_index_from("hciX"));
    EXPECT_FALSE(hci_index_from("00:11:22:33:44:55"));
}

TEST(LeConnParams, TheCommandAsBytes) {
    // What "hcitool lecup --handle 3585 --min 120 --max 144 --latency 0
    // --timeout 600" writes.
    const auto bytes = le_conn_update_command(0x0e01, LeConnParams{150, 180, 0, 6000});
    const std::array<uint8_t, 18> expected{0x01, 0x13, 0x20, 14,   0x01, 0x0e, 120,  0x00, 144,
                                           0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(le_interval_units(30), 24);
    EXPECT_EQ(le_interval_units(45), 36);
    EXPECT_EQ(le_timeout_units(6000), 600);
    EXPECT_EQ(le_interval_ms(132), 165);
}

TEST(LeConnParams, ReadsTheControllersAnswers) {
    const uint16_t handle = 0x0e01;
    // Command Status, status 0, for opcode 0x2013.
    const uint8_t taken[] = {0x04, 0x0f, 0x04, 0x00, 0x01, 0x13, 0x20};
    auto event = parse_le_conn_update_event(taken, sizeof(taken), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, LeConnUpdateEvent::Kind::CommandStatus);
    EXPECT_EQ(event->status, 0);
    // Refused by the controller: command disallowed.
    const uint8_t disallowed[] = {0x04, 0x0f, 0x04, 0x0c, 0x01, 0x13, 0x20};
    event = parse_le_conn_update_event(disallowed, sizeof(disallowed), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->status, 0x0c);
    // The status of another command is not ours.
    const uint8_t other[] = {0x04, 0x0f, 0x04, 0x00, 0x01, 0x0d, 0x20};
    EXPECT_FALSE(parse_le_conn_update_event(other, sizeof(other), handle));

    // LE Connection Update Complete: 165 ms (132), latency 0, 6 s (600).
    const uint8_t done[] = {0x04, 0x3e, 0x0a, 0x03, 0x00, 0x01, 0x0e, 132, 0x00, 0x00, 0x00, 0x58, 0x02};
    event = parse_le_conn_update_event(done, sizeof(done), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, LeConnUpdateEvent::Kind::UpdateComplete);
    EXPECT_EQ(event->status, 0);
    EXPECT_EQ(event->now.max_interval_ms, 165);
    EXPECT_EQ(event->now.latency, 0);
    EXPECT_EQ(event->now.timeout_ms, 6000);
    // On another link, another LE event, cut short, not an event at all.
    EXPECT_FALSE(parse_le_conn_update_event(done, sizeof(done), 0x0e02));
    const uint8_t advert[] = {0x04, 0x3e, 0x0a, 0x02, 0x00, 0x01, 0x0e, 132, 0x00, 0x00, 0x00, 0x58, 0x02};
    EXPECT_FALSE(parse_le_conn_update_event(advert, sizeof(advert), handle));
    EXPECT_FALSE(parse_le_conn_update_event(done, sizeof(done) - 2, handle));
    EXPECT_FALSE(parse_le_conn_update_event(done + 1, sizeof(done) - 1, handle));
    EXPECT_FALSE(parse_le_conn_update_event(nullptr, 0, handle));
    // Refused by the phone: unacceptable connection parameters.
    const uint8_t refused[] = {0x04, 0x3e, 0x0a, 0x03, 0x3b, 0x01, 0x0e, 24, 0x00, 0x00, 0x00, 0x48, 0x00};
    event = parse_le_conn_update_event(refused, sizeof(refused), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->status, 0x3b);
}

TEST(LeConnParams, TheHelpersLineGoesRoundTrip) {
    LeConnUpdateResult result = applied(6000);
    EXPECT_EQ(format_le_conn_update_result(result),
              "outcome=applied status=0 handle=3585 interval_ms=60 latency=0 timeout_ms=6000");
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(result) + "\n"), result);

    result = outcome(LeConnUpdateOutcome::Refused, 0x3b);
    result.handle = 64;
    result.detail = "the iPhone or the controller refused the parameters";
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(result)), result);

    result = outcome(LeConnUpdateOutcome::NoRight);
    result.detail = "HCI command: Operation not permitted";
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(result)), result);

    for (LeConnUpdateOutcome what : {LeConnUpdateOutcome::NoLink, LeConnUpdateOutcome::NoVerdict, LeConnUpdateOutcome::Failed})
        EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(outcome(what))).outcome, what);

    // Anything else is a failure, never a success.
    EXPECT_EQ(parse_le_conn_update_result("").outcome, LeConnUpdateOutcome::Failed);
    EXPECT_EQ(parse_le_conn_update_result("usage: landline-le-params").outcome, LeConnUpdateOutcome::Failed);
    EXPECT_EQ(parse_le_conn_update_result("outcome=splendid status=0").outcome, LeConnUpdateOutcome::Failed);
}

TEST(LeConnParams, TheEndOfTheLinkIsRead) {
    const uint16_t handle = 0x0e01;
    // Disconnect Complete: status 0, handle 0x0e01, reason 0x08 (timeout).
    const uint8_t ended[] = {0x04, 0x05, 0x04, 0x00, 0x01, 0x0e, 0x08};
    const auto event = parse_le_conn_update_event(ended, sizeof(ended), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, LeConnUpdateEvent::Kind::Disconnected);
    EXPECT_EQ(event->status, 0x08);
    // Another link's end, or a Disconnect that failed, is not this one's.
    EXPECT_FALSE(parse_le_conn_update_event(ended, sizeof(ended), 0x0e02));
    const uint8_t failed_end[] = {0x04, 0x05, 0x04, 0x0c, 0x01, 0x0e, 0x08};
    EXPECT_FALSE(parse_le_conn_update_event(failed_end, sizeof(failed_end), handle));
}

// The two lines of the relevé of 17 November (lecup-suivi.txt.btmon), as
// the controller sent them: the iPhone's own renegotiation at 45 ms with
// 720 ms kept, then ours, at 60 ms and 6 s.
TEST(LeConnParams, TheTrialOfTheSeventeenth) {
    const uint16_t handle = 3585;
    const uint8_t theirs[] = {0x04, 0x3e, 0x0a, 0x03, 0x00, 0x01, 0x0e, 0x24, 0x00, 0x00, 0x00, 0x48, 0x00};
    auto event = parse_le_conn_update_event(theirs, sizeof(theirs), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->now.max_interval_ms, 45);
    EXPECT_EQ(event->now.timeout_ms, 720);
    const uint8_t ours[] = {0x04, 0x3e, 0x0a, 0x03, 0x00, 0x01, 0x0e, 0x30, 0x00, 0x00, 0x00, 0x58, 0x02};
    event = parse_le_conn_update_event(ours, sizeof(ours), handle);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->now.max_interval_ms, 60);
    EXPECT_EQ(event->now.timeout_ms, 6000);
    // What hcitool lecup 0x0e01 0x0024 0x0030 0x0000 0x0258 wrote.
    const auto bytes = le_conn_update_command(handle, LeConnParams{});
    const std::array<uint8_t, 18> expected{0x01, 0x13, 0x20, 14,   0x01, 0x0e, 0x24, 0x00, 0x30,
                                           0x00, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(bytes, expected);
}

TEST(LeConnParams, TheWatchLinesGoRoundTrip) {
    LeConnUpdateResult result = changed(720, 45);
    EXPECT_EQ(format_le_conn_update_result(result),
              "outcome=changed status=0 handle=3585 interval_ms=45 latency=0 timeout_ms=720");
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(result)), result);
    result = outcome(LeConnUpdateOutcome::Ended, 8);
    result.handle = 3585;
    EXPECT_EQ(format_le_conn_update_result(result), "outcome=ended status=8 handle=3585");
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(result)), result);
    EXPECT_EQ(parse_le_conn_update_result(format_le_conn_update_result(outcome(LeConnUpdateOutcome::NotReady))).outcome,
              LeConnUpdateOutcome::NotReady);
}

TEST(LeConnParamsPlan, NothingIsAskedWhenTurnedOff) {
    LeConnParamsPlan plan;
    LeConnParamsConfig config;
    config.enabled = false;
    plan.configure(config);
    for (int64_t t = 0; t < 60; ++t)
        EXPECT_FALSE(plan.due(at(t, true, 1)));
    EXPECT_STREQ(plan.state(), "off");
}

TEST(LeConnParamsPlan, AsksInThePassThatSeesTheLink) {
    LeConnParamsPlan plan = enabled_plan();
    EXPECT_FALSE(plan.due(at(0, false, 0)));
    EXPECT_STREQ(plan.state(), "waiting");
    EXPECT_TRUE(plan.due(at(10, true, 1)));
    EXPECT_EQ(plan.attempts(), 1);
    EXPECT_STREQ(plan.state(), "pending");
    // Not again while that one is on its way.
    EXPECT_FALSE(plan.due(at(11, true, 1, true)));
    EXPECT_TRUE(plan.report(12, applied(6000)));
    EXPECT_TRUE(plan.settled());
    EXPECT_STREQ(plan.state(), "applied");
    ASSERT_TRUE(plan.in_force());
    EXPECT_EQ(plan.in_force()->max_interval_ms, 60);
    for (int64_t t = 12; t < 600; ++t)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
}

TEST(LeConnParamsPlan, ALinkThatWasUpAtStartIsAskedToo) {
    LeConnParamsPlan plan = enabled_plan();
    // Never seen coming up: the monitor's count is still zero.
    EXPECT_TRUE(plan.due(at(0, true, 0)));
}

TEST(LeConnParamsPlan, ARefusalIsAskedAgainThreeTimesAtMostFiveSecondsApart) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_FALSE(plan.report(1, outcome(LeConnUpdateOutcome::Refused, 0x3b)));
    for (int64_t t = 1; t < 1 + LE_CONN_PARAMS_RETRY_SECONDS; ++t)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
    ASSERT_TRUE(plan.due(at(6, true, 1)));
    EXPECT_FALSE(plan.report(14, outcome(LeConnUpdateOutcome::NoVerdict)));
    EXPECT_FALSE(plan.due(at(18, true, 1)));
    ASSERT_TRUE(plan.due(at(19, true, 1)));
    // The third was the last on this link, which is kept.
    EXPECT_TRUE(plan.report(20, outcome(LeConnUpdateOutcome::Refused, 0x3b)));
    EXPECT_FALSE(plan.settled());
    EXPECT_TRUE(plan.gave_up());
    EXPECT_STREQ(plan.state(), "refused");
    for (int64_t t = 20; t < 3600; t += 7)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
    // The next link starts afresh.
    EXPECT_TRUE(plan.due(at(4000, true, 2)));
}

TEST(LeConnParamsPlan, CollisionsAreNotAttemptsAndAreAskedAgainInTwoSeconds) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    for (int i = 1; i <= LE_CONN_PARAMS_COLLISIONS; ++i) {
        const int status = i % 2 ? 0x2a : 0x23;
        const int64_t now = static_cast<int64_t>(i) * 3;
        EXPECT_FALSE(plan.report(now, outcome(LeConnUpdateOutcome::Refused, status))) << i;
        EXPECT_EQ(plan.attempts(), 0);
        EXPECT_FALSE(plan.due(at(now + 1, true, 1))) << i;
        ASSERT_TRUE(plan.due(at(now + LE_CONN_PARAMS_COLLISION_RETRY_SECONDS, true, 1))) << i;
    }
    // The eleventh in a row: given up on this link.
    EXPECT_TRUE(plan.report(40, outcome(LeConnUpdateOutcome::Refused, 0x2a)));
    EXPECT_TRUE(plan.gave_up());
    EXPECT_FALSE(plan.due(at(60, true, 1)));
}

// 18 November, 03:08, right after a wake: the request collided with the
// iPhone's own procedure (HCI 42), whose result (45 ms, 720 ms) came next.
// The retry stays two seconds away, not the minute between re-asks.
TEST(LeConnParamsPlan, ACollisionFollowedByThePhonesOwnSetIsStillAskedAgainInTwoSeconds) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_FALSE(plan.report(2, outcome(LeConnUpdateOutcome::Refused, 0x2a)));
    EXPECT_FALSE(plan.report(2, changed(720, 45)));
    EXPECT_FALSE(plan.due(at(3, true, 1)));
    EXPECT_TRUE(plan.due(at(2 + LE_CONN_PARAMS_COLLISION_RETRY_SECONDS, true, 1)));
}

// Without a collision, a renegotiation on a link is re-asked a minute after
// the last request at the soonest, as before.
// Review of 18 November: a collision long settled must not make every
// later renegotiation on the link re-asked at once.
TEST(LeConnParamsPlan, ACollisionSettledEarlierDoesNotHurryALaterRenegotiation) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_FALSE(plan.report(2, outcome(LeConnUpdateOutcome::Refused, 0x2a)));
    ASSERT_TRUE(plan.due(at(4, true, 1)));
    EXPECT_TRUE(plan.report(4, applied(6000)));
    EXPECT_FALSE(plan.report(24, changed(720, 45)));
    EXPECT_FALSE(plan.due(at(25, true, 1)));
    EXPECT_FALSE(plan.due(at(4 + LE_CONN_PARAMS_REASK_SECONDS - 1, true, 1)));
    EXPECT_TRUE(plan.due(at(4 + LE_CONN_PARAMS_REASK_SECONDS, true, 1)));
}

TEST(LeConnParamsPlan, ARenegotiationWithoutCollisionWaitsTheMinute) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_TRUE(plan.report(1, applied(6000)));
    EXPECT_FALSE(plan.report(100, changed(720, 45)));
    EXPECT_FALSE(plan.due(at(LE_CONN_PARAMS_REASK_SECONDS - 1, true, 1)));
}

TEST(LeConnParamsPlan, ParametersShortOfTheTimeoutAskedForAreAskedAgain) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    // The phone answered with a set of its own: 2 s of supervision.
    EXPECT_FALSE(plan.report(1, applied(2000, 30)));
    EXPECT_FALSE(plan.settled());
    ASSERT_TRUE(plan.due(at(1 + LE_CONN_PARAMS_RETRY_SECONDS, true, 1)));
    // More than was asked for is as good.
    EXPECT_TRUE(plan.report(7, applied(8000, 165)));
    EXPECT_TRUE(plan.settled());
}

// The second fact of 17 November: on a new link that reused handle 3585,
// the iPhone renegotiated by itself (45 ms, 720 ms kept). The request is
// made on the new link whatever its handle, and only the count of links
// says it is new.
TEST(LeConnParamsPlan, EachLinkIsAskedAfreshWhateverItsHandle) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    plan.report(1, applied(6000));
    // Lost, and back: what was obtained went with the old link.
    EXPECT_FALSE(plan.due(at(100, false, 1)));
    EXPECT_FALSE(plan.settled());
    EXPECT_FALSE(plan.in_force());
    EXPECT_TRUE(plan.due(at(101, true, 2)));
    plan.report(102, applied(6000));
    // Dropped and back between two passes: the count alone says so.
    EXPECT_TRUE(plan.due(at(200, true, 3)));
    EXPECT_FALSE(plan.settled());
}

TEST(LeConnParamsPlan, ARenegotiationShortOfTheTimeoutIsAskedAgainAtMostOnceAMinute) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_TRUE(plan.report(1, applied(6000)));
    // The phone goes back to 720 ms twenty seconds in.
    EXPECT_FALSE(plan.report(20, changed(720, 45)));
    EXPECT_FALSE(plan.settled());
    EXPECT_STREQ(plan.state(), "pending");
    ASSERT_TRUE(plan.in_force());
    EXPECT_EQ(plan.in_force()->timeout_ms, 720);
    for (int64_t t = 20; t < 60; ++t)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
    ASSERT_TRUE(plan.due(at(60, true, 1)));
    EXPECT_TRUE(plan.report(61, applied(6000)));
    // Back to 720 ms at once: a minute after the last request.
    plan.report(62, changed(720, 30));
    EXPECT_FALSE(plan.due(at(100, true, 1)));
    EXPECT_TRUE(plan.due(at(120, true, 1)));
    // A change that keeps 6 s or more is no reason to ask.
    plan.report(121, applied(6000));
    plan.report(500, changed(6000, 30));
    EXPECT_TRUE(plan.settled());
    EXPECT_FALSE(plan.due(at(600, true, 1)));
}

TEST(LeConnParamsPlan, AWatchThatRanOutIsFollowedByAnotherRequest) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    plan.report(1, applied(6000));
    plan.watch_ended(LE_CONN_PARAMS_WATCH_SECONDS);
    EXPECT_TRUE(plan.due(at(LE_CONN_PARAMS_WATCH_SECONDS, true, 1)));
}

TEST(LeConnParamsPlan, NoRightIsFinalAndSaidOnce) {
    LeConnParamsPlan plan = enabled_plan();
    ASSERT_TRUE(plan.due(at(0, true, 1)));
    EXPECT_TRUE(plan.report(0, outcome(LeConnUpdateOutcome::NoRight)));
    EXPECT_TRUE(plan.stopped());
    EXPECT_STREQ(plan.state(), "no_right");
    for (int64_t t = 5; t < 600; t += 3)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
    // Not on the next link either.
    EXPECT_FALSE(plan.due(at(700, false, 1)));
    EXPECT_FALSE(plan.due(at(701, true, 2)));
    EXPECT_STREQ(plan.state(), "no_right");
}

TEST(LeConnParamsPlan, TheDelayCanBeSet) {
    LeConnParamsPlan plan;
    LeConnParamsConfig config;
    config.delay_seconds = 5;
    plan.configure(config);
    EXPECT_FALSE(plan.due(at(0, false, 0)));
    for (int64_t t = 1; t < 6; ++t)
        EXPECT_FALSE(plan.due(at(t, true, 1))) << t;
    EXPECT_TRUE(plan.due(at(6, true, 1)));
}

TEST(LeConnParamsHelper, AStrictHelperMustBeRootsAndCarryTheCapability) {
    // An ordinary executable of the user is not one: not root's, no
    // capability. Unless it was named in the environment, for trials.
    EXPECT_FALSE(le_params_helper_usable("/nonexistent/landline-le-params", false));
    EXPECT_TRUE(le_params_helper_usable("/bin/sh", false));
    EXPECT_FALSE(le_params_helper_usable("/bin/sh", true));
    EXPECT_FALSE(le_params_helper_usable("", false));
}

namespace {

    // A stand-in for the helper: a script that prints what it is given to
    // print, line by line, as the real one does.
    std::string fake_helper(const std::string& name, const std::string& body) {
        const std::string path = ::testing::TempDir() + "/" + name;
        FILE* f = std::fopen(path.c_str(), "w");
        std::fputs(("#!/bin/sh\n" + body).c_str(), f);
        std::fclose(f);
        chmod(path.c_str(), 0755);
        return path;
    }

} // namespace

TEST(LeConnParamsHelper, TheVerdictThenTheWatchLineByLine) {
    const std::string helper = fake_helper("le-params-watch",
                                           "echo 'outcome=applied status=0 handle=3585 interval_ms=60 latency=0 timeout_ms=6000'\n"
                                           "/bin/sleep 0.2\n"
                                           "echo 'outcome=changed status=0 handle=3585 interval_ms=45 latency=0 timeout_ms=720'\n"
                                           "echo 'outcome=ended status=8 handle=3585'\n");
    std::vector<LeConnUpdateResult> seen;
    run_le_params_helper(helper, 0, "A0:B1:C2:D3:E4:F5", LeConnParams{}, 1000, 60, [&seen](const LeConnUpdateResult& r) {
        seen.push_back(r);
        return true;
    });
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].outcome, LeConnUpdateOutcome::Applied);
    EXPECT_EQ(seen[0].got.timeout_ms, 6000);
    EXPECT_EQ(seen[1].outcome, LeConnUpdateOutcome::Changed);
    EXPECT_EQ(seen[1].got.timeout_ms, 720);
    EXPECT_EQ(seen[2].outcome, LeConnUpdateOutcome::Ended);
    EXPECT_EQ(seen[2].status, 8);
}

TEST(LeConnParamsHelper, ItsArgumentsAndAnEmptyEnvironment) {
    // The helper is told everything on its command line, and nothing of the
    // daemon's environment reaches it.
    setenv("LANDLINE_SECRET_FOR_TEST", "leaked", 1);
    const std::string helper = fake_helper(
        "le-params-args", "echo \"outcome=failed status=-1 handle=-1 detail=$*|${LANDLINE_SECRET_FOR_TEST}|$HOME\"\n");
    std::vector<LeConnUpdateResult> seen;
    run_le_params_helper(helper, 1, "A0:B1:C2:D3:E4:F5", LeConnParams{}, 2500, 21600, [&seen](const LeConnUpdateResult& r) {
        seen.push_back(r);
        return true;
    });
    unsetenv("LANDLINE_SECRET_FOR_TEST");
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].detail, "hci1 A0:B1:C2:D3:E4:F5 45 60 0 6000 2500 21600||");
}

TEST(LeConnParamsHelper, AWatchIsStoppedAtOnce) {
    const std::string helper = fake_helper("le-params-long",
                                           "echo 'outcome=applied status=0 handle=1 interval_ms=60 latency=0 timeout_ms=6000'\n"
                                           "exec /bin/sleep 30\n");
    std::atomic<bool> stop{false};
    std::vector<LeConnUpdateResult> seen;
    const auto started = std::chrono::steady_clock::now();
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop = true;
    });
    run_le_params_helper(
        helper,
        0,
        "A0:B1:C2:D3:E4:F5",
        LeConnParams{},
        1000,
        60,
        [&seen](const LeConnUpdateResult& r) {
            seen.push_back(r);
            return true;
        },
        &stop);
    stopper.join();
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].outcome, LeConnUpdateOutcome::Applied);
    EXPECT_LT(took, 2000);
}

TEST(LeConnParamsHelper, AWatchSleepsUntilStoppedWithoutTicking) {
    // #57: with a stop descriptor the watch sleeps until something happens,
    // instead of waking every 50 ms to look at the flag.
    const std::string helper = fake_helper("le-params-quiet",
                                           "echo 'outcome=applied status=0 handle=1 interval_ms=60 latency=0 timeout_ms=6000'\n"
                                           "exec /bin/sleep 30\n");
    std::atomic<bool> stop{false};
    const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(wake, 0);
    std::vector<LeConnUpdateResult> seen;
    long switches = -1;
    std::chrono::steady_clock::time_point stopped_at;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        stopped_at = std::chrono::steady_clock::now();
        stop = true;
        const uint64_t one = 1;
        ASSERT_EQ(write(wake, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    });
    rusage before{};
    long at_verdict = -1;
    run_le_params_helper(
        helper,
        0,
        "A0:B1:C2:D3:E4:F5",
        LeConnParams{},
        1000,
        60,
        [&](const LeConnUpdateResult& r) {
            seen.push_back(r);
            getrusage(RUSAGE_THREAD, &before);
            at_verdict = before.ru_nvcsw;
            return true;
        },
        &stop,
        wake);
    rusage after{};
    getrusage(RUSAGE_THREAD, &after);
    const auto late =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopped_at).count();
    stopper.join();
    close(wake);
    switches = after.ru_nvcsw - at_verdict;
    ASSERT_EQ(seen.size(), 1u);
    // About a second of watching: twenty wake-ups with the old tick, a
    // handful now (the stop itself, the helper's end).
    EXPECT_LT(switches, 8);
    EXPECT_LT(late, 500);
}

TEST(LeConnParamsHelper, SilenceAndGarbageAreFailures) {
    const std::string silent = fake_helper("le-params-silent", "exit 1\n");
    std::vector<LeConnUpdateResult> seen;
    const auto sink = [&seen](const LeConnUpdateResult& r) {
        seen.push_back(r);
        return true;
    };
    run_le_params_helper(silent, 0, "A0:B1:C2:D3:E4:F5", LeConnParams{}, 500, 0, sink);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].outcome, LeConnUpdateOutcome::Failed);
    seen.clear();
    run_le_params_helper("/nonexistent/landline-le-params", 0, "A0:B1:C2:D3:E4:F5", LeConnParams{}, 500, 0, sink);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].outcome, LeConnUpdateOutcome::NoRight);
}
