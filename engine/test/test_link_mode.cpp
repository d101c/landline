#include <gtest/gtest.h>
#include <tether/bluetooth/link_mode.hpp>

using namespace tether::bluetooth;

TEST(LinkMode, LeanAppliesWhenEverythingItRestsOnIsThere) {
    const auto effective = effective_link_mode(LinkMode::Lean, true, true, true, false);
    EXPECT_EQ(effective.mode, LinkMode::Lean);
    EXPECT_EQ(effective.reason, "");
    EXPECT_TRUE(effective.lean());
}

TEST(LinkMode, FullAsSetNeedsNoReason) {
    const auto effective = effective_link_mode(LinkMode::Full, true, true, true, false);
    EXPECT_EQ(effective.mode, LinkMode::Full);
    EXPECT_EQ(effective.reason, "");
    // Whatever else is true.
    EXPECT_EQ(effective_link_mode(LinkMode::Full, false, false, false, true).reason, "");
}

TEST(LinkMode, WithoutAnLeHalfLeanFallsBackToFull) {
    const auto effective = effective_link_mode(LinkMode::Lean, false, true, true, false);
    EXPECT_EQ(effective.mode, LinkMode::Full);
    EXPECT_EQ(effective.reason, "no_le");
}

TEST(LinkMode, WithNotificationsOffLeanFallsBackToFull) {
    const auto off = effective_link_mode(LinkMode::Lean, true, false, true, false);
    EXPECT_EQ(off.mode, LinkMode::Full);
    EXPECT_EQ(off.reason, "notifications_off");
    // Never granted on the iPhone: nothing would say a message has arrived.
    const auto never = effective_link_mode(LinkMode::Lean, true, true, false, false);
    EXPECT_EQ(never.mode, LinkMode::Full);
    EXPECT_EQ(never.reason, "notifications_off");
}

TEST(LinkMode, AnIPhoneThatKeepsTheLinkGetsFullForAWhile) {
    const auto effective = effective_link_mode(LinkMode::Lean, true, true, true, true);
    EXPECT_EQ(effective.mode, LinkMode::Full);
    EXPECT_EQ(effective.reason, "phone_keeps_link");
}

TEST(LinkMode, TheReasonsAreOrdered) {
    EXPECT_EQ(effective_link_mode(LinkMode::Lean, false, false, false, true).reason, "no_le");
    EXPECT_EQ(effective_link_mode(LinkMode::Lean, true, false, false, true).reason, "notifications_off");
}

TEST(LinkMode, NamesAndParsing) {
    EXPECT_STREQ(to_string(LinkMode::Lean), "lean");
    EXPECT_STREQ(to_string(LinkMode::Full), "full");
    // A file written before the setting existed reads lean.
    EXPECT_EQ(link_mode_from_string(""), LinkMode::Lean);
    EXPECT_EQ(link_mode_from_string("lean"), LinkMode::Lean);
    EXPECT_EQ(link_mode_from_string("full"), LinkMode::Full);
    EXPECT_EQ(link_mode_from_string("Full"), LinkMode::Lean);
    EXPECT_EQ(parse_link_mode("lean"), LinkMode::Lean);
    EXPECT_EQ(parse_link_mode("full"), LinkMode::Full);
    EXPECT_FALSE(parse_link_mode("complete").has_value());
    EXPECT_FALSE(parse_link_mode("").has_value());
}

TEST(LinkMode, TheEnvironmentVariableOnlyCountsWhenItNamesAMode) {
    EXPECT_FALSE(link_mode_from_env(nullptr).has_value());
    EXPECT_FALSE(link_mode_from_env("").has_value());
    EXPECT_FALSE(link_mode_from_env("1").has_value());
    EXPECT_EQ(link_mode_from_env("full"), LinkMode::Full);
    EXPECT_EQ(link_mode_from_env("lean"), LinkMode::Lean);
}

TEST(LinkMemory, RoundTripsAndBelongsToOnePhone) {
    LinkMemory memory;
    memory.address = "02:00:00:00:00:04";
    memory.ancs_ready_once = true;
    memory.map = ProfileVerdict::Granted;
    memory.map_at = 1700000000;
    memory.pbap = ProfileVerdict::Forbidden;
    memory.pbap_at = 1700000100;
    const std::string text = serialize_link_memory(memory);
    EXPECT_EQ(deserialize_link_memory(text, "02:00:00:00:00:04"), memory);

    const LinkMemory other = deserialize_link_memory(text, "02:00:00:00:00:05");
    EXPECT_EQ(other.address, "02:00:00:00:00:05");
    EXPECT_FALSE(other.ancs_ready_once);
    EXPECT_EQ(other.map, ProfileVerdict::Unknown);

    const LinkMemory broken = deserialize_link_memory("{not json", "02:00:00:00:00:04");
    EXPECT_FALSE(broken.ancs_ready_once);
    EXPECT_EQ(broken.pbap, ProfileVerdict::Unknown);
}

TEST(ServiceState, AnOpenSessionIsReady) {
    ServiceFacts facts;
    facts.open = true;
    facts.verdict = ProfileVerdict::Forbidden;
    EXPECT_STREQ(service_state(facts), "ready");
}

TEST(ServiceState, LeanWithTheLinkClosedSaysOnDemandOnceGranted) {
    ServiceFacts facts;
    facts.lean = true;
    facts.le_connected = true;
    EXPECT_STREQ(service_state(facts), "unknown");
    facts.verdict = ProfileVerdict::Granted;
    EXPECT_STREQ(service_state(facts), "on_demand");
    // Without the LE link nothing can be opened.
    facts.le_connected = false;
    EXPECT_STREQ(service_state(facts), "unavailable");
}

TEST(ServiceState, ARefusalIsKeptBetweenOpenings) {
    ServiceFacts facts;
    facts.lean = true;
    facts.le_connected = true;
    facts.verdict = ProfileVerdict::Forbidden;
    EXPECT_STREQ(service_state(facts), "forbidden");
    facts = ServiceFacts{};
    facts.error_forbidden = true;
    EXPECT_STREQ(service_state(facts), "forbidden");
}

TEST(ServiceState, AReleasedProfileIsUnavailable) {
    ServiceFacts facts;
    facts.held = true;
    facts.lean = true;
    facts.le_connected = true;
    facts.verdict = ProfileVerdict::Granted;
    EXPECT_STREQ(service_state(facts), "unavailable");
}

TEST(ServiceState, FullModeNeverSaysOnDemand) {
    ServiceFacts facts;
    facts.verdict = ProfileVerdict::Granted;
    facts.le_connected = true;
    EXPECT_STREQ(service_state(facts), "unknown");
    facts.error_other = true;
    EXPECT_STREQ(service_state(facts), "unavailable");
}

TEST(CallsState, EveryValue) {
    CallsFacts facts;
    EXPECT_STREQ(calls_state(facts), "off");
    facts.calls_enabled = true;
    EXPECT_STREQ(calls_state(facts), "off");
    facts.le_connected = true;
    facts.ancs_ready = true;
    // Full mode, no hands-free: the notification's actions are all there is.
    EXPECT_STREQ(calls_state(facts), "control_only");
    facts.lean = true;
    EXPECT_STREQ(calls_state(facts), "on_demand");
    facts.classic_failed = true;
    EXPECT_STREQ(calls_state(facts), "control_only");
    facts.hands_free_ready = true;
    EXPECT_STREQ(calls_state(facts), "ready");
    facts.calls_enabled = false;
    EXPECT_STREQ(calls_state(facts), "off");
}

#include <tether/bluetooth/config.hpp>

TEST(LinkModeConfig, DefaultsToLeanAndRoundTrips) {
    Config config;
    EXPECT_EQ(config.link_mode, LinkMode::Lean);
    config.link_mode = LinkMode::Full;
    EXPECT_EQ(deserialize_config(serialize_config(config)).link_mode, LinkMode::Full);
    config.link_mode = LinkMode::Lean;
    EXPECT_EQ(deserialize_config(serialize_config(config)).link_mode, LinkMode::Lean);
}

TEST(LinkModeConfig, AFileWrittenBeforeTheSettingReadsLean) {
    EXPECT_EQ(deserialize_config(R"({"config_version": 3, "calls_enabled": true})").link_mode, LinkMode::Lean);
    EXPECT_EQ(deserialize_config(R"({"config_version": 4, "link_mode": "full"})").link_mode, LinkMode::Full);
    EXPECT_EQ(deserialize_config(R"({"config_version": 4, "link_mode": "turbo"})").link_mode, LinkMode::Lean);
    EXPECT_EQ(deserialize_config(R"({"config_version": 4, "link_mode": 2})").link_mode, LinkMode::Lean);
}
