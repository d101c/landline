#include <gtest/gtest.h>

#include "scoped_env.hpp"

#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/phone_audio.hpp>
#include <tether/bluetooth/telephony.hpp>

#include <chrono>
#include <filesystem>

using namespace tether::bluetooth;
using std::chrono::seconds;

// --- Defaults ---------------------------------------------------------------

// The iPhone takes any computer for a speaker. Nothing comes here unasked.
TEST(PhoneAudioDefaults, TheSoundStaysOnTheIPhone) {
    const Config config;
    EXPECT_EQ(config.phone_audio, PhoneAudio::Phone);
    EXPECT_FALSE(config.call_audio_here);
    EXPECT_FALSE(config.phone_audio_rule) << "nothing is written outside Landline's own files unless asked";
    EXPECT_EQ(deserialize_config("{}").phone_audio, PhoneAudio::Phone);
    EXPECT_FALSE(deserialize_config("{}").call_audio_here);
}

// "auto" used to be the default and was written out with every other setting.
TEST(PhoneAudioDefaults, AnAutoWrittenByAnOlderVersionWasNotAChoice) {
    EXPECT_EQ(deserialize_config(R"({"config_version": 2, "phone_audio": "auto"})").phone_audio, PhoneAudio::Phone);
    EXPECT_EQ(deserialize_config(R"({"phone_audio": "auto"})").phone_audio, PhoneAudio::Phone);
    EXPECT_EQ(deserialize_config(R"({"config_version": 2, "phone_audio": "here"})").phone_audio, PhoneAudio::Here);
    EXPECT_EQ(deserialize_config(R"({"config_version": 3, "phone_audio": "auto"})").phone_audio, PhoneAudio::Auto);
    EXPECT_EQ(deserialize_config(R"({"config_version": 3, "phone_audio": "loud"})").phone_audio, PhoneAudio::Phone);
}

TEST(PhoneAudioDefaults, TheChoicesSurviveTheConfigFile) {
    Config config;
    config.phone_audio = PhoneAudio::Here;
    config.phone_audio_rule = true;
    config.call_audio_here = true;
    const Config read = deserialize_config(serialize_config(config));
    EXPECT_EQ(read.phone_audio, PhoneAudio::Here);
    EXPECT_TRUE(read.phone_audio_rule);
    EXPECT_TRUE(read.call_audio_here);
    EXPECT_EQ(nlohmann::json::parse(serialize_config(config))["call_audio"], "here");
    EXPECT_EQ(nlohmann::json::parse(serialize_config(Config{}))["call_audio"], "phone");
}

// --- What the follower does about the stream --------------------------------

TEST(PhoneAudioPlan, AutoTouchesNothing) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Auto, true, false, 0, seconds(1)), StreamMove::Done);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Auto, false, false, 0, seconds(1)), StreamMove::Done);
}

// iOS opens the stream by itself about a second after the link comes up.
TEST(PhoneAudioPlan, PhoneClosesTheStreamTheIPhoneOpens) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, false, false, 0, seconds(0)), StreamMove::Wait);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, 0, seconds(1)), StreamMove::Close);
}

TEST(PhoneAudioPlan, PhoneWaitsForACloseAlreadyAskedFor) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, true, 1, seconds(3)), StreamMove::Wait);
}

// WirePlumber may dial the stream again once it is closed.
TEST(PhoneAudioPlan, PhoneClosesAStreamThatComesBackButNotForEver) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, 1, seconds(8)), StreamMove::Close);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, PHONE_AUDIO_MAX_CLOSES - 1, seconds(20)),
              StreamMove::Close);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, PHONE_AUDIO_MAX_CLOSES, seconds(20)), StreamMove::Done);
}

// A stream opened later is the user picking this computer on the iPhone.
TEST(PhoneAudioPlan, PhoneLeavesAStreamOpenedAfterTheWindow) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, false, false, 0, PHONE_AUDIO_WINDOW), StreamMove::Done);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, 0, PHONE_AUDIO_WINDOW), StreamMove::Done);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Phone, true, false, 0, PHONE_AUDIO_WINDOW - seconds(1)),
              StreamMove::Close);
}

TEST(PhoneAudioPlan, HereOpensTheStreamOnlyIfTheIPhoneDidNot) {
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Here, false, false, 0, seconds(1)), StreamMove::Wait);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Here, true, false, 0, seconds(1)), StreamMove::Done);
    EXPECT_EQ(plan_phone_audio(PhoneAudio::Here, false, false, 0, PHONE_AUDIO_HERE_DELAY), StreamMove::Open);
}

// --- Commands ---------------------------------------------------------------

TEST(AudioRequest, ReadsTheRouteAndRemembersByDefault) {
    const auto here = parse_audio_request({{"route", "here"}}, true);
    EXPECT_TRUE(here.valid);
    EXPECT_EQ(here.route, PhoneAudio::Here);
    EXPECT_TRUE(here.persist);
    EXPECT_FALSE(here.rule.has_value());

    const auto phone = parse_audio_request({{"route", "phone"}, {"persist", false}}, true);
    EXPECT_TRUE(phone.valid);
    EXPECT_EQ(phone.route, PhoneAudio::Phone);
    EXPECT_FALSE(phone.persist);
}

TEST(AudioRequest, StillReadsTheSwitchOfTheOlderClients) {
    EXPECT_EQ(parse_audio_request({{"enabled", true}}, true).route, PhoneAudio::Here);
    const auto off = parse_audio_request({{"enabled", false}}, true);
    EXPECT_TRUE(off.valid);
    EXPECT_EQ(off.route, PhoneAudio::Phone);
}

TEST(AudioRequest, AutoIsARememberedChoiceOfTheMediaSideOnly) {
    EXPECT_TRUE(parse_audio_request({{"route", "auto"}}, true).valid);
    EXPECT_FALSE(parse_audio_request({{"route", "auto"}, {"persist", false}}, true).valid);
    EXPECT_FALSE(parse_audio_request({{"route", "auto"}}, false).valid) << "calls are here or on the phone";
}

// A command that names nothing must not silently send the sound somewhere.
TEST(AudioRequest, RefusesWhatItCannotRead) {
    EXPECT_FALSE(parse_audio_request(nlohmann::json::object(), true).valid);
    EXPECT_FALSE(parse_audio_request({{"route", "speaker"}}, true).valid);
    EXPECT_FALSE(parse_audio_request({{"route", 1}}, true).valid);
    EXPECT_FALSE(parse_audio_request({{"enabled", "yes"}}, true).valid);
}

TEST(AudioRequest, CarriesTheRuleSwitch) {
    const auto request = parse_audio_request({{"route", "phone"}, {"rule", true}}, true);
    ASSERT_TRUE(request.rule.has_value());
    EXPECT_TRUE(*request.rule);
}

// --- The one file written outside Tether's own ------------------------------

TEST(PhoneAudioRule, LivesInTheUsersWireplumberFragments) {
    const auto dir = std::filesystem::temp_directory_path() / "landline-phone-audio-rule-test";
    std::filesystem::remove_all(dir);
    tether::testing::ScopedEnv xdg("XDG_CONFIG_HOME", dir);
    EXPECT_EQ(phone_audio_rule_path(),
              (dir / "wireplumber" / "wireplumber.conf.d" / "zz-landline-phone-audio.conf").string());
    EXPECT_FALSE(phone_audio_rule_installed());
}

// The default configuration, and every one without the rule, writes nothing.
TEST(PhoneAudioRule, IsNotWrittenUnlessAskedFor) {
    const auto dir = std::filesystem::temp_directory_path() / "landline-phone-audio-rule-test";
    std::filesystem::remove_all(dir);
    tether::testing::ScopedEnv xdg("XDG_CONFIG_HOME", dir);

    Config config;
    config.device_address = "02:00:00:00:00:01";
    EXPECT_TRUE(sync_wireplumber_rule(config));
    config.phone_audio = PhoneAudio::Here;
    EXPECT_TRUE(sync_wireplumber_rule(config));
    // Asked for, but with nothing to name or nothing remembered: still no file.
    config.phone_audio_rule = true;
    config.phone_audio = PhoneAudio::Auto;
    EXPECT_TRUE(sync_wireplumber_rule(config));
    config.phone_audio = PhoneAudio::Phone;
    config.device_address.clear();
    EXPECT_TRUE(sync_wireplumber_rule(config));

    EXPECT_FALSE(std::filesystem::exists(dir / "wireplumber"));
}

// --- The sound of calls ------------------------------------------------------

namespace {
    constexpr const char* GATEWAY = "/org/pipewire/Telephony/ag0";
}

// PipeWire accepts the voice link unless told otherwise (RejectSCO false).
TEST(CallAudioPolicy, RefusesTheVoiceLinkAsSoonAsTheGatewayAppears) {
    CallAudioPolicy policy;
    EXPECT_FALSE(policy.step("", std::nullopt, 0));
    const auto fix = policy.step(GATEWAY, false, 0);
    ASSERT_TRUE(fix.has_value());
    EXPECT_TRUE(*fix) << "the sound of calls stays on the iPhone by default";
    EXPECT_FALSE(policy.step(GATEWAY, true, 0)) << "and it is written once";
}

TEST(CallAudioPolicy, WritesNothingWhenTheSwitchAlreadyAgrees) {
    CallAudioPolicy policy;
    EXPECT_FALSE(policy.step(GATEWAY, true, 0));

    CallAudioPolicy here;
    here.set_here(true);
    EXPECT_FALSE(here.step(GATEWAY, false, 0));
}

TEST(CallAudioPolicy, HereAcceptsTheVoiceLink) {
    CallAudioPolicy policy;
    policy.set_here(true);
    const auto fix = policy.step(GATEWAY, true, 0);
    ASSERT_TRUE(fix.has_value());
    EXPECT_FALSE(*fix);
}

// audio_here during a call flips the switch behind the policy's back.
TEST(CallAudioPolicy, AChoiceMadeForOneCallLastsForThatCall) {
    CallAudioPolicy policy;
    EXPECT_FALSE(policy.step(GATEWAY, true, 0));
    for (int tick = 0; tick < 30; ++tick)
        EXPECT_FALSE(policy.step(GATEWAY, false, 1)) << "the call in progress keeps its route";
    const auto fix = policy.step(GATEWAY, false, 0);
    ASSERT_TRUE(fix.has_value()) << "the call ended: back to the standing choice";
    EXPECT_TRUE(*fix);
}

// The same without a call: "the next call here", then back.
TEST(CallAudioPolicy, AOneOffChoiceMadeBeforeTheCallIsLeftAloneUntilItEnds) {
    CallAudioPolicy policy;
    EXPECT_FALSE(policy.step(GATEWAY, true, 0));
    EXPECT_FALSE(policy.step(GATEWAY, false, 0)) << "flipped by hand between calls: not fought";
    EXPECT_FALSE(policy.step(GATEWAY, false, 1));
    EXPECT_TRUE(policy.step(GATEWAY, false, 0).value_or(false));
}

TEST(CallAudioPolicy, AChangedStandingChoiceWaitsForTheCallToEnd) {
    CallAudioPolicy policy;
    EXPECT_FALSE(policy.step(GATEWAY, true, 1));
    policy.set_here(true);
    EXPECT_FALSE(policy.step(GATEWAY, true, 1));
    const auto fix = policy.step(GATEWAY, true, 0);
    ASSERT_TRUE(fix.has_value());
    EXPECT_FALSE(*fix);
}

TEST(CallAudioPolicy, EveryNewHandsFreeLinkIsBroughtInLine) {
    CallAudioPolicy policy;
    EXPECT_TRUE(policy.step(GATEWAY, false, 0).value_or(false));
    EXPECT_FALSE(policy.step("", std::nullopt, 0));
    // PipeWire builds a new gateway, with its own default, on each connection.
    EXPECT_TRUE(policy.step(GATEWAY, false, 0).value_or(false));
    EXPECT_TRUE(policy.step("/org/pipewire/Telephony/ag1", false, 0).value_or(false));
}

// BlueZ's hands-free carries no call audio and has no such switch.
TEST(CallAudioPolicy, AStackWithoutTheSwitchIsLeftAlone) {
    CallAudioPolicy policy;
    for (int tick = 0; tick < 5; ++tick)
        EXPECT_FALSE(policy.step(GATEWAY, std::nullopt, 0));
}

// --- The Audio lease: in the lean link mode the phone's sound wanted here is
// --- what keeps the classic link open.

TEST(PhoneAudioLease, TheStandingChoiceHoldsAndReturnsTheLease) {
    PhoneAudioFollower follower;
    std::vector<bool> told;
    follower.set_lease_handler([&told](bool here) {
        told.push_back(here);
        return true;
    });
    follower.configure("02:00:00:00:00:04", PhoneAudio::Here);
    EXPECT_EQ(told, std::vector<bool>{true});
    EXPECT_TRUE(follower.wants_here());
    // Said once, not on every configure.
    follower.configure("02:00:00:00:00:04", PhoneAudio::Here);
    EXPECT_EQ(told.size(), 1u);
    follower.configure("02:00:00:00:00:04", PhoneAudio::Phone);
    EXPECT_EQ(told, (std::vector<bool>{true, false}));
    EXPECT_FALSE(follower.wants_here());
}

TEST(PhoneAudioLease, NoPhoneSelectedIsNoLease) {
    PhoneAudioFollower follower;
    std::vector<bool> told;
    follower.set_lease_handler([&told](bool here) {
        told.push_back(here);
        return true;
    });
    follower.configure("", PhoneAudio::Here);
    ASSERT_FALSE(told.empty());
    EXPECT_FALSE(told.back());
    EXPECT_FALSE(follower.wants_here());
}

TEST(PhoneAudioLease, AOneOffHereWithNoLinkOpensOneInTheLeanMode) {
    PhoneAudioFollower follower;
    std::vector<bool> told;
    follower.set_lease_handler([&told](bool here) {
        told.push_back(here);
        return true;
    });
    follower.configure("02:00:00:00:00:04", PhoneAudio::Phone);
    EXPECT_TRUE(follower.apply_now(nullptr, true));
    EXPECT_EQ(told, (std::vector<bool>{false, true}));
    EXPECT_TRUE(follower.wants_here());
}

TEST(PhoneAudioLease, AOneOffHereWithNoLinkIsRefusedWhenNoLinkWillBeOpened) {
    PhoneAudioFollower follower;
    // The full link mode: the handler opens nothing.
    follower.set_lease_handler([](bool) { return false; });
    follower.configure("02:00:00:00:00:04", PhoneAudio::Phone);
    EXPECT_FALSE(follower.apply_now(nullptr, true));
    EXPECT_FALSE(follower.wants_here());
}

TEST(PhoneAudioLease, WithoutAHandlerNothingChanges) {
    PhoneAudioFollower follower;
    follower.configure("02:00:00:00:00:04", PhoneAudio::Phone);
    EXPECT_FALSE(follower.apply_now(nullptr, true));
    EXPECT_FALSE(follower.apply_now(nullptr));
    // A one-off "phone" with no link has nothing to move either.
    follower.set_lease_handler([](bool) { return true; });
    EXPECT_FALSE(follower.apply_now(nullptr, false));
}
