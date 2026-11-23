#include <gtest/gtest.h>
#include <tether/bluetooth/call_audio_follow.hpp>
#include <tether/bluetooth/classic_demand.hpp>
#include <tether/bluetooth/dial_plan.hpp>
#include <tether/bluetooth/telephony.hpp>

using namespace tether::bluetooth;

// --- Where the sound of a call placed from here goes -------------------------

TEST(DialAudio, TheClientsWordWins) {
    EXPECT_TRUE(dial_audio_here("here", false, nullptr));
    EXPECT_FALSE(dial_audio_here("phone", true, nullptr));
    EXPECT_FALSE(dial_audio_here("phone", true, "setting"));
}

// The case of 17 November: the extension named nothing and the call was
// placed with its sound left on the iPhone.
TEST(DialAudio, NothingSaidMeansThisComputer) {
    EXPECT_TRUE(dial_audio_here("", false, nullptr));
    EXPECT_TRUE(dial_audio_here("", false, ""));
    EXPECT_TRUE(dial_audio_here("elsewhere", false, nullptr));
}

TEST(DialAudio, TheSwitchGivesTheStandingChoiceBack) {
    EXPECT_FALSE(dial_audio_here("", false, "setting"));
    EXPECT_TRUE(dial_audio_here("", true, "setting"));
    EXPECT_FALSE(dial_audio_here("", true, "phone"));
}

TEST(CallSound, OnlyACallBeingPlacedOrConnectedCarriesSound) {
    EXPECT_TRUE(call_carries_sound("dialing"));
    EXPECT_TRUE(call_carries_sound("alerting"));
    EXPECT_TRUE(call_carries_sound("active"));
    EXPECT_TRUE(call_carries_sound("held"));
    EXPECT_FALSE(call_carries_sound("incoming"));
    EXPECT_FALSE(call_carries_sound("waiting"));
    EXPECT_FALSE(any_call_carries_sound({}));
    EXPECT_TRUE(any_call_carries_sound({"incoming", "active"}));
}

// --- The wish, followed -------------------------------------------------------

TEST(CallAudioFollowUp, NothingAskedNothingDone) {
    CallAudioFollowUp follow;
    for (int64_t t = 0; t < 60000; t += 250)
        EXPECT_EQ(follow.step(true, "idle", t), CallAudioStep::None);
    EXPECT_FALSE(follow.wish().has_value());
}

// The iPhone opens the voice link with the call: nothing to ask.
TEST(CallAudioFollowUp, ASoundThatComesByItselfIsSaidOnce) {
    CallAudioFollowUp follow;
    follow.expect(true);
    EXPECT_EQ(follow.step(false, "idle", 0), CallAudioStep::None) << "no call yet";
    EXPECT_EQ(follow.step(true, "idle", 500), CallAudioStep::None);
    EXPECT_EQ(follow.step(true, "pending", 750), CallAudioStep::None);
    EXPECT_EQ(follow.step(true, "active", 1000), CallAudioStep::Arrived);
    EXPECT_EQ(follow.step(true, "active", 1250), CallAudioStep::None);
    EXPECT_TRUE(follow.arrived());
}

// Activate before the call exists is what failed on 17 November (the iPhone
// answers ERROR to AT+BCC without a call): asked only once a call carries
// sound, and only after the iPhone had its chance.
TEST(CallAudioFollowUp, TheVoiceLinkIsAskedForOnlyUnderACallAndAfterAWait) {
    CallAudioFollowUp follow;
    follow.expect(true);
    for (int64_t t = 0; t <= 20000; t += 250)
        EXPECT_EQ(follow.step(false, "idle", t), CallAudioStep::None) << "ringing here, or not placed yet";
    EXPECT_EQ(follow.step(true, "idle", 21000), CallAudioStep::None);
    EXPECT_EQ(follow.step(true, "idle", 23750), CallAudioStep::None);
    EXPECT_EQ(follow.step(true, "idle", 24000), CallAudioStep::Activate);
    EXPECT_EQ(follow.step(true, "idle", 24250), CallAudioStep::None);
    EXPECT_EQ(follow.step(true, "active", 25000), CallAudioStep::Arrived);
}

TEST(CallAudioFollowUp, GivesUpAfterThreeRequestsAndSaysSoOnce) {
    CallAudioFollowUp follow;
    follow.expect(true);
    std::vector<int64_t> asked;
    int64_t failed_at = -1;
    for (int64_t t = 0; t <= 30000; t += 250) {
        const CallAudioStep step = follow.step(true, "idle", t);
        if (step == CallAudioStep::Activate)
            asked.push_back(t);
        if (step == CallAudioStep::Failed) {
            EXPECT_EQ(failed_at, -1) << "said once";
            failed_at = t;
        }
    }
    EXPECT_EQ(asked, (std::vector<int64_t>{3000, 6000, 9000}));
    EXPECT_EQ(failed_at, 12000);
    EXPECT_TRUE(follow.failed());
}

// Moved to the iPhone on the iPhone, after it was here: the user's doing.
TEST(CallAudioFollowUp, ASoundThatLeavesAfterArrivingIsNotFought) {
    CallAudioFollowUp follow;
    follow.expect(true);
    ASSERT_EQ(follow.step(true, "active", 0), CallAudioStep::Arrived);
    for (int64_t t = 250; t <= 30000; t += 250)
        EXPECT_EQ(follow.step(true, "idle", t), CallAudioStep::None);
}

TEST(CallAudioFollowUp, AWishForTheIPhoneAsksNothing) {
    CallAudioFollowUp follow;
    follow.expect(false);
    for (int64_t t = 0; t <= 30000; t += 250)
        EXPECT_EQ(follow.step(true, "idle", t), CallAudioStep::None);
    ASSERT_TRUE(follow.wish().has_value());
    EXPECT_FALSE(*follow.wish());
}

TEST(CallAudioFollowUp, ANewWishStartsOver) {
    CallAudioFollowUp follow;
    follow.expect(true);
    ASSERT_EQ(follow.step(true, "active", 0), CallAudioStep::Arrived);
    follow.clear();
    EXPECT_FALSE(follow.wish().has_value());
    follow.expect(true);
    EXPECT_EQ(follow.step(true, "active", 1000), CallAudioStep::Arrived);
}

// --- The switch, with a choice made for one call ------------------------------

namespace {
    constexpr const char* GATEWAY = "/org/pipewire/Telephony/ag0";
}

// The race of 17 November, 16:05: the link is opened for the call, the dial
// thread chooses "here", and the supervisor then sees the new gateway and
// puts it back on the standing choice before the call shows.
TEST(CallAudioPolicyOnce, AChoiceForTheNextCallSurvivesTheGatewayAppearing) {
    CallAudioPolicy policy;
    policy.set_once(true);
    EXPECT_FALSE(policy.step(GATEWAY, false, 0)) << "already accepting: left alone";
    EXPECT_FALSE(policy.step(GATEWAY, false, 1));
    const auto after = policy.step(GATEWAY, false, 0);
    ASSERT_TRUE(after.has_value()) << "the call ended: the standing choice again";
    EXPECT_TRUE(*after);
    EXPECT_FALSE(policy.once().has_value());
}

TEST(CallAudioPolicyOnce, AChoiceForTheIPhoneIsWrittenBeforeTheCall) {
    CallAudioPolicy policy;
    policy.set_here(true);
    EXPECT_FALSE(policy.step(GATEWAY, false, 0));
    policy.set_once(false);
    const auto fix = policy.step(GATEWAY, false, 0);
    ASSERT_TRUE(fix.has_value());
    EXPECT_TRUE(*fix);
}

// 16:07 the same day: the gateway was first seen with the call already on
// it, the switch was left as PipeWire creates it (open), and the sound of a
// call nobody asked for here came here.
TEST(CallAudioPolicyOnce, AGatewayFirstSeenUnderACallIsStillBroughtInLine) {
    CallAudioPolicy policy;
    const auto fix = policy.step(GATEWAY, false, 1);
    ASSERT_TRUE(fix.has_value());
    EXPECT_TRUE(*fix);
    // And only then: the call in progress keeps what was chosen for it.
    EXPECT_FALSE(policy.step(GATEWAY, false, 1));
}

TEST(CallAudioPolicyOnce, AWithdrawnChoiceGoesBackToTheStandingOne) {
    CallAudioPolicy policy;
    policy.set_once(true);
    EXPECT_FALSE(policy.step(GATEWAY, false, 0));
    policy.set_once(std::nullopt);
    EXPECT_TRUE(policy.step(GATEWAY, false, 0).value_or(false));
}

TEST(CallAudioPolicyOnce, TheChoiceGoesWithTheHandsFreeLink) {
    CallAudioPolicy policy;
    policy.set_once(true);
    EXPECT_FALSE(policy.step(GATEWAY, false, 0));
    EXPECT_FALSE(policy.step("", std::nullopt, 0));
    EXPECT_FALSE(policy.once().has_value());
    EXPECT_TRUE(policy.step(GATEWAY, false, 0).value_or(false));
}

// --- The whole sequence through the client ------------------------------------

namespace {

    constexpr const char* ADDRESS = "02:00:00:00:00:01";

    class RecordingSource : public TelephonySource {
    public:
        TelephonySnapshot snap;
        bool activate_ok = true;
        // What an accepted Activate does to the transport.
        std::string after_activate;
        mutable std::vector<std::string> calls;

        TelephonyIds ids() const override {
            return {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
        }
        GDBusConnection* connection() const override { return nullptr; }
        TelephonySnapshot snapshot() const override { return snap; }
        bool invoke(const std::string&, const char*, const char* method, GVariant* args, std::string& err) override {
            std::string line = method;
            if (args) {
                g_variant_ref_sink(args);
                if (g_variant_is_of_type(args, G_VARIANT_TYPE("(ssv)"))) {
                    const char* iface = nullptr;
                    const char* name = nullptr;
                    GVariant* value = nullptr;
                    g_variant_get(args, "(&s&sv)", &iface, &name, &value);
                    line += std::string(" ") + name + "=" + (g_variant_get_boolean(value) ? "true" : "false");
                    snap.reject_sco = g_variant_get_boolean(value) != FALSE;
                    g_variant_unref(value);
                }
                g_variant_unref(args);
            }
            calls.push_back(line);
            if (line == "Activate") {
                if (!activate_ok) {
                    err = "Method call failed";
                    return false;
                }
                if (!after_activate.empty())
                    snap.audio_state = after_activate;
            }
            return true;
        }
    };

    struct Rig {
        RecordingSource* source = nullptr;
        std::unique_ptr<TelephonyClient> client;
        int64_t now = 0;
        std::vector<std::string> events;

        Rig() {
            auto owned = std::make_unique<RecordingSource>();
            source = owned.get();
            source->snap.gateway.path = "/org/pipewire/Telephony/ag0";
            source->snap.gateway.state = "connected";
            source->snap.audio_state = "idle";
            // As PipeWire creates the gateway.
            source->snap.reject_sco = false;
            std::vector<std::unique_ptr<TelephonySource>> sources;
            sources.push_back(std::move(owned));
            client = std::make_unique<TelephonyClient>(std::move(sources), ADDRESS);
            client->set_clock([this] { return now; });
            client->set_audio_event_handler([this](const std::string& stage, const std::string& reason) {
                events.push_back(reason.empty() ? stage : stage + ":" + reason);
            });
        }

        void call(const char* state) {
            Call call;
            call.path = "/org/pipewire/Telephony/ag0/call1";
            call.state = state;
            source->snap.calls = {call};
        }

        void poll_until(int64_t until) {
            for (; now <= until; now += 250)
                client->poll_calls();
        }

        int count(const std::string& what) const {
            return static_cast<int>(std::count(source->calls.begin(), source->calls.end(), what));
        }
    };

} // namespace

// An outgoing call with its sound here, as it should have gone at 16:05: the
// switch is opened before the dial, nothing is asked of the iPhone while it
// has no call, and the link it offers with the call is simply taken.
TEST(CallAudioSequence, AnOutgoingCallHereAcceptsTheLinkAndNeverActivatesEarly) {
    Rig rig;
    rig.client->poll_calls();
    ASSERT_EQ(rig.source->calls, (std::vector<std::string>{"Set RejectSCO=true"})) << "standing choice: the iPhone";

    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    EXPECT_EQ(rig.source->calls.back(), "Set RejectSCO=false");
    EXPECT_EQ(rig.count("Activate"), 0) << "the iPhone refuses AT+BCC without a call";
    EXPECT_EQ(rig.client->status().value("audio_wanted", ""), "here");
    EXPECT_EQ(rig.client->status().value("audio_route", ""), "phone") << "the truth, not the wish";

    rig.poll_until(1000);
    EXPECT_EQ(rig.source->snap.reject_sco, false) << "not put back on the standing choice before the call";

    rig.call("dialing");
    rig.poll_until(1500);
    rig.source->snap.audio_state = "active";
    rig.poll_until(2000);
    EXPECT_EQ(rig.events, (std::vector<std::string>{"audio_here"}));
    EXPECT_EQ(rig.count("Activate"), 0);
    EXPECT_EQ(rig.client->status().value("audio_route", ""), "here");

    // The call ends: the standing choice again, and the wish is gone.
    rig.source->snap.calls.clear();
    rig.source->snap.audio_state = "idle";
    rig.poll_until(2500);
    EXPECT_EQ(rig.source->snap.reject_sco, true);
    EXPECT_TRUE(rig.client->status()["audio_wanted"].is_null());
}

TEST(CallAudioSequence, ALinkTheIPhoneDoesNotOfferIsAskedForThenReported) {
    Rig rig;
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    rig.call("alerting");
    rig.source->activate_ok = false;
    rig.poll_until(20000);
    EXPECT_EQ(rig.count("Activate"), 3);
    EXPECT_EQ(rig.events, (std::vector<std::string>{"audio_failed:no_voice_link"}));
    EXPECT_TRUE(rig.client->status().value("audio_failed", false));
    EXPECT_EQ(rig.client->status().value("audio_route", ""), "phone");
}

TEST(CallAudioSequence, ARequestTheIPhoneGrantsBringsTheSound) {
    Rig rig;
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    rig.call("active");
    rig.source->after_activate = "active";
    rig.poll_until(10000);
    EXPECT_EQ(rig.count("Activate"), 1);
    EXPECT_EQ(rig.events, (std::vector<std::string>{"audio_here"}));
}

// The voice link taken and nothing playing it (the card on `off`).
TEST(CallAudioSequence, ALinkNobodyPlaysIsReportedAsSuch) {
    Rig rig;
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    rig.call("active");
    rig.source->snap.audio_state = "pending";
    rig.poll_until(20000);
    EXPECT_EQ(rig.events, (std::vector<std::string>{"audio_failed:voice_link_unused"}));
}

// "Answer here" on a link opened for the ring: the gateway is first seen with
// the call on it. Nothing is asked while it only rings; the answer follows.
TEST(CallAudioSequence, AnIncomingCallAnsweredHere) {
    Rig rig;
    rig.call("incoming");
    rig.client->poll_calls();
    EXPECT_EQ(rig.source->calls, (std::vector<std::string>{"Set RejectSCO=true"}))
        << "the ring is not played here while the standing choice is the iPhone";

    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    EXPECT_EQ(rig.source->calls.back(), "Set RejectSCO=false");
    rig.poll_until(8000);
    EXPECT_EQ(rig.count("Activate"), 0) << "still ringing";

    rig.call("active");
    rig.poll_until(8500);
    rig.source->snap.audio_state = "active";
    rig.poll_until(9000);
    EXPECT_EQ(rig.events, (std::vector<std::string>{"audio_here"}));
}

TEST(CallAudioSequence, ACallForTheIPhoneRefusesTheLinkAndAsksNothing) {
    Rig rig;
    rig.client->set_call_audio_here(true);
    rig.client->poll_calls();
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_phone", err)) << err;
    EXPECT_EQ(rig.source->snap.reject_sco, true);
    rig.call("dialing");
    rig.poll_until(20000);
    EXPECT_EQ(rig.count("Activate"), 0);
    EXPECT_TRUE(rig.events.empty());
    EXPECT_EQ(rig.client->status().value("audio_wanted", ""), "phone");

    rig.source->snap.calls.clear();
    rig.poll_until(21000);
    EXPECT_EQ(rig.source->snap.reject_sco, false) << "back to the standing choice: here";
}

// A dial the iPhone accepted and that brought no call: the choice made for
// it must not fall on the next call.
TEST(CallAudioSequence, AForgottenWishLeavesTheStandingChoice) {
    Rig rig;
    rig.client->poll_calls();
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    rig.client->forget_call_audio();
    rig.poll_until(1000);
    EXPECT_EQ(rig.source->snap.reject_sco, true);
    EXPECT_TRUE(rig.client->status()["audio_wanted"].is_null());
}

// Mid-call, as before: asked for at once.
TEST(CallAudioSequence, AudioHereUnderACallActivatesAtOnce) {
    Rig rig;
    rig.call("active");
    rig.client->poll_calls();
    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    EXPECT_EQ(rig.source->calls.back(), "Activate");
}

// --- Calls first: OBEX waits ---------------------------------------------------

TEST(CallBusy, ADialARingOrACallListed) {
    EXPECT_FALSE(call_busy(false, false, false));
    EXPECT_TRUE(call_busy(true, false, false));
    EXPECT_TRUE(call_busy(false, true, false));
    EXPECT_TRUE(call_busy(false, false, true));
}

TEST(CallBusy, TheSwitchPutsTheReadsBackUnderCalls) {
    EXPECT_TRUE(obex_waits_for_calls(nullptr));
    EXPECT_TRUE(obex_waits_for_calls(""));
    EXPECT_TRUE(obex_waits_for_calls("0"));
    EXPECT_FALSE(obex_waits_for_calls("1"));
}

namespace {

    // A link opened for a call, hands-free there.
    ClassicDemand link_for_a_call(DemandFacts& facts) {
        ClassicDemand demand;
        facts.le_connected = true;
        EXPECT_TRUE(demand.take(Lease::Call, 10));
        facts.now = 10;
        EXPECT_EQ(demand.step(facts), DemandAction::OpenVoice);
        facts.classic_connected = true;
        facts.now = 11;
        demand.step(facts);
        facts.hands_free = true;
        facts.now = 12;
        demand.step(facts);
        return demand;
    }

} // namespace

// 16:05 and 16:07 on 17 November: the sessions were opened one second after
// the dial, on the link opened for it, and their reads held the thread that
// follows the call for half a minute.
TEST(CallsFirst, NoSessionIsOpenedUnderACallLease) {
    DemandFacts facts;
    ClassicDemand demand = link_for_a_call(facts);
    EXPECT_EQ(demand.state(), ClassicState::Open);
    EXPECT_FALSE(demand.want_data());
    EXPECT_TRUE(demand.keep_data()) << "sessions already open are not closed for it";
    facts.now = 60;
    demand.step(facts);
    EXPECT_FALSE(demand.want_data()) << "however long the call lasts";
}

TEST(CallsFirst, ACallOnALinkOpenedForSomethingElseHoldsTheSessionsToo) {
    ClassicDemand demand;
    DemandFacts facts;
    facts.le_connected = true;
    ASSERT_TRUE(demand.take(Lease::Reading, 10));
    facts.now = 10;
    ASSERT_EQ(demand.step(facts), DemandAction::OpenData);
    facts.classic_connected = facts.map_open = true;
    facts.now = 11;
    demand.step(facts);
    EXPECT_TRUE(demand.want_data());
    facts.call_active = true;
    facts.now = 12;
    demand.step(facts);
    EXPECT_FALSE(demand.want_data());
    facts.call_active = false;
    facts.now = 13;
    demand.step(facts);
    EXPECT_TRUE(demand.want_data());
}

TEST(CallsFirst, AMessageBeingSentIsNotHeldBack) {
    DemandFacts facts;
    ClassicDemand demand = link_for_a_call(facts);
    ASSERT_TRUE(demand.take(Lease::Send, 13));
    facts.now = 13;
    demand.step(facts);
    EXPECT_TRUE(demand.want_data());
}

TEST(CallsFirst, AfterTheCallTheLinkItLeavesServesOneSync) {
    DemandFacts facts;
    ClassicDemand demand = link_for_a_call(facts);
    facts.call_active = true;
    facts.now = 20;
    demand.step(facts);
    // The call ends: the lease is returned and a sync is taken for the history.
    facts.call_active = false;
    demand.release(Lease::Call, 30);
    ASSERT_TRUE(demand.take(Lease::Sync, 30));
    facts.now = 30;
    EXPECT_EQ(demand.step(facts), DemandAction::None);
    EXPECT_EQ(demand.state(), ClassicState::Open);
    EXPECT_TRUE(demand.want_data());
}

TEST(CallsFirst, TheSwitchOpensTheSessionsUnderTheCallAsBefore) {
    DemandFacts facts;
    ClassicDemand demand = link_for_a_call(facts);
    demand.set_data_waits_for_calls(false);
    EXPECT_TRUE(demand.want_data());
}

// The lease of a call lasts as long as the call: nothing closes the link
// under one, whatever else comes and goes.
TEST(CallsFirst, TheLinkIsNeverClosedUnderACallOrADial) {
    DemandFacts facts;
    ClassicDemand demand = link_for_a_call(facts);
    for (int64_t now = 13; now < 13 + 3600; ++now) {
        facts.now = now;
        facts.call_active = now > 20;
        ASSERT_EQ(demand.step(facts), DemandAction::None) << "at " << now;
    }
    // Even with the lease gone (the dial gave up before the call showed).
    demand.release(Lease::Call, 4000);
    for (int64_t now = 4000; now < 4600; ++now) {
        facts.now = now;
        facts.call_active = true;
        ASSERT_EQ(demand.step(facts), DemandAction::None) << "at " << now;
    }
    facts.call_active = false;
    facts.now = 4600;
    EXPECT_EQ(demand.step(facts), DemandAction::Close);
}
