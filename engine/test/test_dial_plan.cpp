#include <gtest/gtest.h>
#include <tether/bluetooth/dial_plan.hpp>

#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    DialFacts cold() { return DialFacts{}; }

    DialFacts link_only() {
        DialFacts facts;
        facts.link_up = true;
        return facts;
    }

    DialFacts ready() {
        DialFacts facts;
        facts.link_up = true;
        facts.hands_free_ready = true;
        return facts;
    }

} // namespace

TEST(DialPlan, WithHandsFreeAlreadyThereItDialsAtOnce) {
    DialOperation dial(100);
    const auto step = dial.step(ready(), 100);
    EXPECT_EQ(step.stage, DialStage::Dialing);
    EXPECT_TRUE(step.changed);
    // The only stage published is the dialling.
    EXPECT_STREQ(to_string(step.stage), "dialing");
    dial.dialed(true);
    EXPECT_EQ(dial.stage(), DialStage::Done);
    EXPECT_TRUE(dial.reason().empty());
    EXPECT_FALSE(dial.wants_link());
}

TEST(DialPlan, FromAClosedLinkItGoesThroughTheThreeStages) {
    DialOperation dial(100);
    std::vector<std::string> published;
    const auto look = [&](const DialFacts& facts, int64_t now) {
        const auto step = dial.step(facts, now);
        if (step.changed && dial_stage_published(step.stage))
            published.push_back(to_string(step.stage));
        return step.stage;
    };
    EXPECT_EQ(look(cold(), 100), DialStage::Connecting);
    EXPECT_EQ(look(cold(), 101), DialStage::Connecting);
    EXPECT_TRUE(dial.wants_link());
    EXPECT_EQ(look(link_only(), 103), DialStage::HandsFree);
    EXPECT_EQ(look(link_only(), 106), DialStage::HandsFree);
    EXPECT_EQ(look(ready(), 108), DialStage::Dialing);
    EXPECT_EQ(look(ready(), 109), DialStage::Dialing);
    EXPECT_EQ(published, (std::vector<std::string>{"connecting", "hands_free", "dialing"}));
}

TEST(DialPlan, NoLinkInTwentySecondsFails) {
    DialOperation dial(100);
    for (int64_t now = 100; now < 100 + DIAL_LINK_TIMEOUT_SECONDS; ++now)
        EXPECT_EQ(dial.step(cold(), now).stage, DialStage::Connecting) << now;
    const auto step = dial.step(cold(), 100 + DIAL_LINK_TIMEOUT_SECONDS);
    EXPECT_EQ(step.stage, DialStage::Failed);
    EXPECT_TRUE(step.changed);
    EXPECT_EQ(dial.reason(), "no_link");
    EXPECT_FALSE(dial.wants_link());
    // And it stays failed.
    EXPECT_FALSE(dial.step(ready(), 200).changed);
    EXPECT_EQ(dial.stage(), DialStage::Failed);
}

TEST(DialPlan, NoHandsFreeFifteenSecondsAfterTheLinkFails) {
    DialOperation dial(100);
    dial.step(cold(), 100);
    // The link took eighteen seconds: the fifteen run from there.
    ASSERT_EQ(dial.step(link_only(), 118).stage, DialStage::HandsFree);
    for (int64_t now = 119; now < 118 + DIAL_HANDS_FREE_TIMEOUT_SECONDS; ++now)
        EXPECT_EQ(dial.step(link_only(), now).stage, DialStage::HandsFree) << now;
    EXPECT_EQ(dial.step(link_only(), 118 + DIAL_HANDS_FREE_TIMEOUT_SECONDS).stage, DialStage::Failed);
    EXPECT_EQ(dial.reason(), "no_hands_free");
}

TEST(DialPlan, CancelWorksAtEachStageBeforeTheDialling) {
    {
        DialOperation dial(100);
        dial.step(cold(), 100);
        EXPECT_TRUE(dial.cancel());
        EXPECT_EQ(dial.step(cold(), 101).stage, DialStage::Cancelled);
        EXPECT_EQ(dial.reason(), "cancelled");
        EXPECT_FALSE(dial.wants_link());
    }
    {
        DialOperation dial(100);
        dial.step(cold(), 100);
        dial.step(link_only(), 103);
        EXPECT_TRUE(dial.cancel());
        // Even with hands-free arriving at the same moment.
        EXPECT_EQ(dial.step(ready(), 104).stage, DialStage::Cancelled);
    }
    {
        // Before the first look.
        DialOperation dial(100);
        EXPECT_TRUE(dial.cancel());
        EXPECT_EQ(dial.step(ready(), 100).stage, DialStage::Cancelled);
    }
}

TEST(DialPlan, CancelHasNoEffectOnceDialling) {
    DialOperation dial(100);
    ASSERT_EQ(dial.step(ready(), 100).stage, DialStage::Dialing);
    EXPECT_FALSE(dial.cancel());
    dial.dialed(true);
    EXPECT_EQ(dial.stage(), DialStage::Done);
    EXPECT_FALSE(dial.cancel());
}

TEST(DialPlan, ADialThePhoneRefusesFails) {
    DialOperation dial(100);
    dial.step(ready(), 100);
    dial.dialed(false);
    EXPECT_EQ(dial.stage(), DialStage::Failed);
    EXPECT_EQ(dial.reason(), "refused");
}

TEST(DialPlan, CallControlSwitchedOffMeanwhileFails) {
    DialOperation dial(100);
    dial.step(cold(), 100);
    DialFacts off = link_only();
    off.calls_enabled = false;
    EXPECT_EQ(dial.step(off, 102).stage, DialStage::Failed);
    EXPECT_EQ(dial.reason(), "calls_off");
}

TEST(DialPlan, OneOperationAtATime) {
    EXPECT_EQ(dial_refusal(true, false), nullptr);
    EXPECT_STREQ(dial_refusal(true, true), "busy");
    EXPECT_STREQ(dial_refusal(false, false), "calls_off");
    // Switched off outranks busy: nothing would come of waiting.
    EXPECT_STREQ(dial_refusal(false, true), "calls_off");
}

TEST(DialPlan, OnlyTheThreeWaitingStagesArePublished) {
    EXPECT_TRUE(dial_stage_published(DialStage::Connecting));
    EXPECT_TRUE(dial_stage_published(DialStage::HandsFree));
    EXPECT_TRUE(dial_stage_published(DialStage::Dialing));
    EXPECT_FALSE(dial_stage_published(DialStage::Done));
    EXPECT_FALSE(dial_stage_published(DialStage::Failed));
    EXPECT_FALSE(dial_stage_published(DialStage::Cancelled));
    EXPECT_STREQ(to_string(DialStage::Connecting), "connecting");
    EXPECT_STREQ(to_string(DialStage::HandsFree), "hands_free");
    EXPECT_STREQ(to_string(DialStage::Dialing), "dialing");
}
