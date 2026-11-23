#include <gtest/gtest.h>
#include <tether/bluetooth/incoming_call.hpp>

using namespace tether::bluetooth;

namespace {

    constexpr auto RING = ancs::CategoryId::IncomingCall;
    constexpr auto MISSED = ancs::CategoryId::MissedCall;

    // The call rings at t=100, notification 7, both actions offered.
    IncomingCallTracker::Event ring(IncomingCallTracker& tracker, int64_t now = 100, uint32_t uid = 7) {
        return tracker.on_notification(uid, RING, "Camille", true, true, now, 1700000000 + now);
    }

    std::vector<CallSight> gateway_with(const char* state, const char* path = "/call1") {
        return {CallSight{path, state}};
    }

} // namespace

TEST(IncomingCall, TheNotificationStartsTheRingBeforeHandsFree) {
    IncomingCallTracker tracker;
    const auto event = ring(tracker);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->uid, 7u);
    EXPECT_EQ(event->state, "ringing");
    EXPECT_EQ(event->caller, "Camille");
    EXPECT_TRUE(event->can_answer_on_phone);
    EXPECT_TRUE(event->can_decline);
    EXPECT_EQ(event->answer_here, "connecting");
    EXPECT_TRUE(event->path.empty());
    EXPECT_EQ(event->since, 1700000100);
    EXPECT_TRUE(tracker.ringing());
    EXPECT_TRUE(tracker.wants_link(100));
    ASSERT_TRUE(tracker.current().has_value());
    EXPECT_EQ(tracker.current()->uid, 7u);
}

TEST(IncomingCall, TheActionFlagsComeFromTheNotification) {
    IncomingCallTracker tracker;
    const auto event = tracker.on_notification(7, RING, "Unknown", false, true, 100);
    ASSERT_TRUE(event.has_value());
    EXPECT_FALSE(event->can_answer_on_phone);
    EXPECT_TRUE(event->can_decline);
}

TEST(IncomingCall, OtherNotificationsAreIgnored) {
    IncomingCallTracker tracker;
    EXPECT_FALSE(tracker.on_notification(7, ancs::CategoryId::Social, "Camille", true, true, 100).has_value());
    EXPECT_FALSE(tracker.on_notification(8, MISSED, "Camille", false, false, 100).has_value());
    EXPECT_FALSE(tracker.ringing());
    EXPECT_FALSE(tracker.wants_link(100));
}

TEST(IncomingCall, TheSameNotificationAgainIsNotPublishedTwice) {
    IncomingCallTracker tracker;
    ring(tracker);
    EXPECT_FALSE(tracker.on_notification(7, RING, "Camille", true, true, 101).has_value());
    // Modified: published.
    const auto renamed = tracker.on_notification(7, RING, "Camille Martin", true, true, 102);
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(renamed->caller, "Camille Martin");
    EXPECT_EQ(renamed->since, 1700000100);
}

TEST(IncomingCall, HandsFreeShowingTheCallMakesAnswerHereReady) {
    IncomingCallTracker tracker;
    ring(tracker);
    // The gateway is there but has not shown the call yet.
    EXPECT_FALSE(tracker.on_calls({}, true, 105).has_value());
    const auto ready = tracker.on_calls(gateway_with("incoming"), true, 108);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(ready->state, "ringing");
    EXPECT_EQ(ready->answer_here, "ready");
    EXPECT_EQ(ready->path, "/call1");
    // Said once.
    EXPECT_FALSE(tracker.on_calls(gateway_with("incoming"), true, 109).has_value());
    // And it does not turn unavailable after the fifteen seconds.
    EXPECT_FALSE(tracker.step(100 + ANSWER_HERE_TIMEOUT_SECONDS + 5).has_value());
}

TEST(IncomingCall, HandsFreeAlreadyThereIsReadyFromTheFirstEvent) {
    IncomingCallTracker tracker;
    tracker.on_calls(gateway_with("incoming"), true, 99);
    const auto event = ring(tracker);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->answer_here, "ready");
    EXPECT_EQ(event->path, "/call1");
}

TEST(IncomingCall, FifteenSecondsWithoutHandsFreeIsUnavailable) {
    IncomingCallTracker tracker;
    ring(tracker);
    for (int64_t now = 101; now < 100 + ANSWER_HERE_TIMEOUT_SECONDS; ++now)
        EXPECT_FALSE(tracker.step(now).has_value()) << now;
    const auto late = tracker.step(100 + ANSWER_HERE_TIMEOUT_SECONDS);
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(late->state, "ringing");
    EXPECT_EQ(late->answer_here, "unavailable");
    // The lease is kept: hands-free can still come.
    EXPECT_TRUE(tracker.wants_link(100 + ANSWER_HERE_TIMEOUT_SECONDS + 1));
    EXPECT_FALSE(tracker.step(100 + ANSWER_HERE_TIMEOUT_SECONDS + 1).has_value());
    // And if it does, the button comes alive after all.
    const auto ready = tracker.on_calls(gateway_with("incoming"), true, 120);
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(ready->answer_here, "ready");
}

TEST(IncomingCall, ARemovalWithoutHandsFreeEndsWithinTwentySeconds) {
    IncomingCallTracker tracker;
    ring(tracker);
    // Removed: answered on the iPhone, declined there, or the caller gave up.
    EXPECT_FALSE(tracker.on_removed(7, 110).has_value());
    EXPECT_FALSE(tracker.ringing());
    EXPECT_FALSE(tracker.current().has_value());
    EXPECT_TRUE(tracker.wants_link(110 + CALL_AFTER_RING_SECONDS - 1));
    for (int64_t now = 111; now < 110 + CALL_AFTER_RING_SECONDS; ++now)
        EXPECT_FALSE(tracker.step(now).has_value()) << now;
    const auto ended = tracker.step(110 + CALL_AFTER_RING_SECONDS);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
    EXPECT_EQ(ended->uid, 7u);
    EXPECT_FALSE(tracker.wants_link(110 + CALL_AFTER_RING_SECONDS));
    EXPECT_FALSE(tracker.step(200).has_value());
}

TEST(IncomingCall, ACallTakenOnTheIPhoneIsHandedToTheCallListWithoutAnEnd) {
    IncomingCallTracker tracker;
    ring(tracker);
    EXPECT_FALSE(tracker.on_removed(7, 104).has_value());
    // Hands-free arrives and shows the call in progress.
    EXPECT_FALSE(tracker.on_calls(gateway_with("active"), true, 109).has_value());
    EXPECT_FALSE(tracker.active());
    for (int64_t now = 110; now < 200; ++now)
        EXPECT_FALSE(tracker.step(now).has_value());
}

TEST(IncomingCall, ACallAnsweredHereIsHandedOverAtOnce) {
    IncomingCallTracker tracker;
    ring(tracker);
    tracker.on_calls(gateway_with("incoming"), true, 108);
    tracker.on_calls(gateway_with("active"), true, 112);
    EXPECT_FALSE(tracker.on_removed(7, 112).has_value());
    EXPECT_FALSE(tracker.active());
}

TEST(IncomingCall, AMissedCallNotificationEndsItAtOnce) {
    IncomingCallTracker tracker;
    ring(tracker);
    EXPECT_FALSE(tracker.on_removed(7, 103).has_value());
    const auto ended = tracker.on_notification(8, MISSED, "Camille", false, false, 104);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
    EXPECT_EQ(ended->uid, 7u);
    // The link stays wanted for the rest of the twenty seconds: the call
    // history is read on it if it opens.
    EXPECT_TRUE(tracker.wants_link(120));
    EXPECT_FALSE(tracker.wants_link(103 + CALL_AFTER_RING_SECONDS));
    // And "ended" is not said twice.
    EXPECT_FALSE(tracker.step(103 + CALL_AFTER_RING_SECONDS).has_value());
}

TEST(IncomingCall, AMissedCallBeforeTheRemovalEndsItToo) {
    IncomingCallTracker tracker;
    ring(tracker);
    const auto ended = tracker.on_notification(8, MISSED, "Camille", false, false, 104);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
    EXPECT_FALSE(tracker.on_removed(7, 105).has_value());
}

TEST(IncomingCall, DecliningFromHereEndsOnTheRemoval) {
    IncomingCallTracker tracker;
    ring(tracker);
    tracker.on_declined(7);
    const auto ended = tracker.on_removed(7, 103);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
    // The lease is returned at once: the link closes after its linger.
    EXPECT_FALSE(tracker.wants_link(103));
}

TEST(IncomingCall, ARemovalSeenByHandsFreeWithNoCallLeftEndsAtOnce) {
    IncomingCallTracker tracker;
    ring(tracker);
    tracker.on_calls(gateway_with("incoming"), true, 108);
    // The caller hangs up: the gateway's list empties, the notification goes.
    tracker.on_calls({}, true, 111);
    const auto ended = tracker.on_removed(7, 111);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
}

TEST(IncomingCall, ASecondCallDuringACallInProgressRingsWithHandsFreeReady) {
    IncomingCallTracker tracker;
    ring(tracker);
    tracker.on_calls(gateway_with("incoming"), true, 105);
    tracker.on_calls(gateway_with("active"), true, 110);
    tracker.on_removed(7, 110);
    ASSERT_FALSE(tracker.active());

    // The gateway shows the first call and the one waiting.
    tracker.on_calls({CallSight{"/call1", "active"}, CallSight{"/call2", "waiting"}}, true, 300);
    const auto second = tracker.on_notification(9, RING, "Noah", true, true, 300, 1700000300);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->uid, 9u);
    EXPECT_EQ(second->state, "ringing");
    EXPECT_EQ(second->answer_here, "ready");
    EXPECT_EQ(second->path, "/call2");
}

TEST(IncomingCall, ANewRingReplacesOneThatNeverEnded) {
    IncomingCallTracker tracker;
    ring(tracker);
    const auto second = tracker.on_notification(9, RING, "Noah", true, true, 130, 1700000130);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->uid, 9u);
    EXPECT_EQ(second->since, 1700000130);
    // The first notification going away no longer matters.
    EXPECT_FALSE(tracker.on_removed(7, 131).has_value());
    EXPECT_TRUE(tracker.ringing());
}

TEST(IncomingCall, ARingNobodyEndedIsOverAfterNinetySeconds) {
    IncomingCallTracker tracker;
    ring(tracker);
    tracker.step(100 + ANSWER_HERE_TIMEOUT_SECONDS);
    const auto ended = tracker.step(100 + RING_MAX_SECONDS);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, "ended");
    EXPECT_FALSE(tracker.active());
}

TEST(IncomingCall, TheEventIsTheOneOfTheSocket) {
    IncomingCallTracker tracker;
    const auto connecting = to_json(*ring(tracker));
    EXPECT_EQ(connecting.value("command", ""), "bt_incoming_call");
    EXPECT_EQ(connecting.value("uid", 0u), 7u);
    EXPECT_EQ(connecting.value("state", ""), "ringing");
    EXPECT_EQ(connecting.value("caller", ""), "Camille");
    EXPECT_TRUE(connecting.value("can_answer_on_phone", false));
    EXPECT_TRUE(connecting.value("can_decline", false));
    EXPECT_EQ(connecting.value("answer_here", ""), "connecting");
    EXPECT_EQ(connecting.value("since", 0), 1700000100);
    // The path is there once the gateway shows the call, not before.
    EXPECT_FALSE(connecting.contains("path"));
    const auto ready = to_json(*tracker.on_calls(gateway_with("incoming"), true, 108));
    EXPECT_EQ(ready.value("path", ""), "/call1");
}

TEST(IncomingCall, TheSwitchReadsItsVariable) {
    EXPECT_TRUE(ancs_calls_enabled(nullptr));
    EXPECT_TRUE(ancs_calls_enabled(""));
    EXPECT_TRUE(ancs_calls_enabled("1"));
    EXPECT_FALSE(ancs_calls_enabled("0"));
}

// Which way an answer or a decline goes.
TEST(CallActionPlan, WithHandsFreeShowingTheCallEverythingGoesThroughIt) {
    const auto answer = plan_call_action("answer", "here", true, true, true, true);
    EXPECT_EQ(answer.via, CallRoute::Hfp);
    EXPECT_EQ(answer.hfp_action, "answer");
    const auto on_phone = plan_call_action("answer", "phone", true, true, true, true);
    EXPECT_EQ(on_phone.via, CallRoute::Hfp);
    EXPECT_EQ(on_phone.hfp_action, "answer");
    const auto decline = plan_call_action("decline", "", true, true, true, true);
    EXPECT_EQ(decline.via, CallRoute::Hfp);
    EXPECT_EQ(decline.hfp_action, "hangup");
}

TEST(CallActionPlan, WithoutHandsFreeAnswerOnThePhoneAndDeclineUseTheNotification) {
    for (const char* audio : {"phone", ""}) {
        const auto answer = plan_call_action("answer", audio, false, true, true, true);
        EXPECT_EQ(answer.via, CallRoute::Ancs) << audio;
        EXPECT_EQ(answer.ancs_action, ancs::ActionId::Positive);
    }
    const auto decline = plan_call_action("decline", "", false, true, true, true);
    EXPECT_EQ(decline.via, CallRoute::Ancs);
    EXPECT_EQ(decline.ancs_action, ancs::ActionId::Negative);
}

TEST(CallActionPlan, AnswerHereNeedsHandsFree) {
    const auto plan = plan_call_action("answer", "here", false, true, true, true);
    EXPECT_EQ(plan.via, CallRoute::None);
    EXPECT_FALSE(plan.error.empty());
}

TEST(CallActionPlan, ANotificationWithoutTheActionCannotBeUsed) {
    EXPECT_EQ(plan_call_action("answer", "phone", false, true, false, true).via, CallRoute::None);
    EXPECT_EQ(plan_call_action("decline", "", false, true, true, false).via, CallRoute::None);
    EXPECT_EQ(plan_call_action("answer", "", false, false, true, true).via, CallRoute::None);
    EXPECT_EQ(plan_call_action("decline", "", false, false, true, true).via, CallRoute::None);
}
