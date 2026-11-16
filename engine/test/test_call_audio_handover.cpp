#include "tether/bluetooth/call_audio_handover.hpp"
#include "tether/bluetooth/telephony.hpp"

#include <gtest/gtest.h>

using namespace tether::bluetooth;

namespace {

    constexpr const char* ADDRESS = "02:00:00:00:00:01";

    // What `pw-dump` prints, cut down to what is read: one sound card, the
    // phone's card, and a node that carries the same address but is no card.
    const char* DUMP = R"json([
      { "id": 54, "type": "PipeWire:Interface:Device",
        "info": { "props": { "device.api": "alsa", "device.name": "alsa_card.usb-headset" },
                  "params": {
                    "EnumProfile": [ { "index": 0, "name": "off" }, { "index": 3, "name": "output:iec958-stereo" } ],
                    "Profile": [ { "index": 3, "name": "output:iec958-stereo", "save": false } ] } } },
      { "id": 91, "type": "PipeWire:Interface:Node",
        "info": { "props": { "api.bluez5.address": "02:00:00:00:00:01", "node.name": "bluez_input.02_00_00_00_00_01" } } },
      { "id": 88, "type": "PipeWire:Interface:Device",
        "info": { "props": { "device.api": "bluez5", "device.form-factor": "phone",
                             "device.name": "bluez_card.02_00_00_00_00_01",
                             "api.bluez5.address": "02:00:00:00:00:01" },
                  "params": {
                    "EnumProfile": [
                      { "index": 0, "name": "off", "description": "Off", "available": "yes" },
                      { "index": 65536, "name": "audio-gateway",
                        "description": "Audio Gateway (A2DP Source & HSP/HFP AG)", "available": "yes" } ],
                    "Profile": [ { "index": 65536, "name": "audio-gateway", "save": false } ] } } }
    ])json";

    PhoneCard phone_card(const std::string& active = "audio-gateway") {
        PhoneCard card;
        card.id = 88;
        card.name = "bluez_card.02_00_00_00_00_01";
        card.profiles = {{0, "off"}, {65536, "audio-gateway"}};
        card.active = active == "off" ? CardProfile{0, "off"} : CardProfile{65536, active};
        return card;
    }

    // A card and the voice link that depends on it. The link is the test's
    // model of PipeWire: it closes some waits after the card goes off, and may
    // reopen when the card comes back.
    class FakeCard : public CallAudioCard {
    public:
        bool present = true;
        bool has_off = true;
        bool switchable = true;
        bool link_up = true;
        // Waits before the link closes once the card is off; -1 never.
        int closes_after = 1;
        bool reopens_on_restore = false;
        // Stops reopening once the card has been set off this many times.
        std::string active = "audio-gateway";
        std::vector<std::string> switches;
        int waits = 0;
        int waits_since_off = -1;

        std::optional<PhoneCard> read(const std::string& address) override {
            if (!present || address != ADDRESS)
                return std::nullopt;
            PhoneCard card = phone_card(active);
            if (!has_off)
                card.profiles.erase(card.profiles.begin());
            return card;
        }

        bool set_profile(const PhoneCard&, const CardProfile& profile) override {
            if (!switchable)
                return false;
            switches.push_back(profile.name);
            active = profile.name;
            if (profile.name == "off") {
                waits_since_off = 0;
                if (closes_after == 0)
                    link_up = false;
            } else {
                waits_since_off = -1;
                if (reopens_on_restore)
                    link_up = true;
            }
            return true;
        }

        void wait(int) {
            ++waits;
            if (waits_since_off >= 0 && closes_after >= 0 && ++waits_since_off >= closes_after)
                link_up = false;
        }

        HandoverResult hand_over(const HandoverTiming& timing = {2000, 500, 100}) {
            return hand_call_audio_to_phone(
                *this, ADDRESS, [this] { return link_up; }, [this](int ms) { wait(ms); }, timing);
        }
    };

} // namespace

TEST(PhoneCard, IsFoundByAddressAmongTheDevices) {
    const auto dump = nlohmann::json::parse(DUMP);
    const auto card = find_phone_card(dump, "02:00:00:00:00:01");

    ASSERT_TRUE(card.has_value());
    EXPECT_EQ(card->id, 88) << "the node that carries the same address is not the card";
    EXPECT_EQ(card->name, "bluez_card.02_00_00_00_00_01");
    EXPECT_EQ(card->active.name, "audio-gateway");
    EXPECT_EQ(card->active.index, 65536);
    ASSERT_EQ(card->profiles.size(), 2u);
    EXPECT_EQ(card->profiles.front().name, "off");
}

TEST(PhoneCard, AddressCaseAndCardNameBothMatch) {
    auto dump = nlohmann::json::parse(DUMP);
    EXPECT_TRUE(find_phone_card(dump, "02:00:00:00:00:01").has_value());
    dump[2]["info"]["props"]["api.bluez5.address"] = "02:00:00:00:00:01";
    dump[2]["info"]["props"]["device.name"] = "bluez_card.AA_BB_CC_DD_EE_0F";
    EXPECT_TRUE(find_phone_card(dump, "02:00:00:00:00:01").has_value());
    dump[2]["info"]["props"].erase("api.bluez5.address");
    EXPECT_TRUE(find_phone_card(dump, "aa:bb:cc:dd:ee:0f").has_value()) << "by the card's name when the address is absent";
    EXPECT_FALSE(find_phone_card(dump, "02:00:00:00:00:01").has_value());
}

// Another phone, a sound card, or an answer that is not a dump at all: no
// card, and so nothing is ever switched.
TEST(PhoneCard, NothingElseIsEverTakenForThePhone) {
    const auto dump = nlohmann::json::parse(DUMP);
    EXPECT_FALSE(find_phone_card(dump, "11:22:33:44:55:66").has_value());
    EXPECT_FALSE(find_phone_card(dump, "").has_value());
    EXPECT_FALSE(find_phone_card(nlohmann::json::object(), ADDRESS).has_value());
    EXPECT_FALSE(find_phone_card(nlohmann::json::parse(R"([1, "x", {"type":"PipeWire:Interface:Device"}])"), ADDRESS)
                     .has_value());
}

TEST(PhoneCard, ACardWithoutParamsIsFoundButHasNoLever) {
    auto dump = nlohmann::json::parse(DUMP);
    dump[2]["info"].erase("params");
    const auto card = find_phone_card(dump, ADDRESS);
    ASSERT_TRUE(card.has_value());
    EXPECT_EQ(card->active.index, -1);
    EXPECT_FALSE(off_profile(*card).has_value());
    EXPECT_FALSE(voice_profile(*card, "audio-gateway").has_value());
}

TEST(PhoneCard, TheProfileToReturnTo) {
    PhoneCard card = phone_card();
    card.profiles.push_back({65537, "headset-audio-gateway"});

    EXPECT_EQ(off_profile(card)->index, 0);
    EXPECT_EQ(voice_profile(card, "headset-audio-gateway")->index, 65537) << "the one that was active";
    EXPECT_EQ(voice_profile(card, "")->name, "audio-gateway") << "the phone's usual one";
    EXPECT_EQ(voice_profile(card, "gone")->name, "audio-gateway");
    EXPECT_EQ(voice_profile(card, "off")->name, "audio-gateway") << "never back to off";

    card.profiles = {{0, "off"}, {7, "something-else"}};
    EXPECT_EQ(voice_profile(card, "")->name, "something-else");
    card.profiles = {{0, "off"}};
    EXPECT_FALSE(voice_profile(card, "").has_value());
}

TEST(CallAudioHandover, NothingIsTouchedWhenTheSoundIsAlreadyOnThePhone) {
    FakeCard card;
    card.link_up = false;
    const HandoverResult result = card.hand_over();

    EXPECT_EQ(result.outcome, HandoverOutcome::AlreadyOnPhone);
    EXPECT_TRUE(released(result.outcome));
    EXPECT_TRUE(card.switches.empty());
}

// The expected course: off, the link closes, the profile comes back and the
// link stays closed.
TEST(CallAudioHandover, TheCardGoesOffThenBackAndTheLinkIsClosed) {
    FakeCard card;
    card.closes_after = 2;
    const HandoverResult result = card.hand_over();

    EXPECT_EQ(result.outcome, HandoverOutcome::Released);
    EXPECT_TRUE(released(result.outcome));
    EXPECT_EQ(result.released_after_ms, 200);
    EXPECT_EQ(card.switches, (std::vector<std::string>{"off", "audio-gateway"}));
    EXPECT_EQ(card.active, "audio-gateway") << "the card is never left off";
    EXPECT_FALSE(card.link_up);
    EXPECT_TRUE(result.parked_profile.empty());
}

TEST(CallAudioHandover, ALinkThatClosesAtOnceCostsNoWaitBeforeTheRestore) {
    FakeCard card;
    card.closes_after = 0;
    const HandoverResult result = card.hand_over();
    EXPECT_EQ(result.outcome, HandoverOutcome::Released);
    EXPECT_EQ(result.released_after_ms, 0);
    EXPECT_EQ(card.waits, 5) << "only the settling wait: 500 ms in steps of 100";
}

// The link never closes: say so, and put the card back so the call can still
// be heard here.
TEST(CallAudioHandover, ALinkThatStaysUpIsReportedAndTheCardRestored) {
    FakeCard card;
    card.closes_after = -1;
    const HandoverResult result = card.hand_over();

    EXPECT_EQ(result.outcome, HandoverOutcome::StillHere);
    EXPECT_FALSE(released(result.outcome));
    EXPECT_EQ(result.released_after_ms, -1);
    EXPECT_EQ(card.switches, (std::vector<std::string>{"off", "audio-gateway"}));
    EXPECT_EQ(card.waits, 20) << "2000 ms in steps of 100, and no settling wait";
    EXPECT_TRUE(card.link_up);
}

// If restoring the profile reopens the link, the card is put off again and
// left there: the sound stays on the iPhone and the caller is told what to
// put back later.
TEST(CallAudioHandover, AProfileThatReopensTheLinkIsLeftOff) {
    FakeCard card;
    card.reopens_on_restore = true;
    const HandoverResult result = card.hand_over();

    EXPECT_EQ(result.outcome, HandoverOutcome::ReleasedParked);
    EXPECT_TRUE(released(result.outcome));
    EXPECT_EQ(result.parked_profile, "audio-gateway");
    EXPECT_EQ(card.switches, (std::vector<std::string>{"off", "audio-gateway", "off"}));
    EXPECT_EQ(card.active, "off");
    EXPECT_FALSE(card.link_up);
}

TEST(CallAudioHandover, NoCardNoOffProfileOrNoSwitchMeansNoLever) {
    {
        FakeCard card;
        card.present = false;
        EXPECT_EQ(card.hand_over().outcome, HandoverOutcome::NoLever);
    }
    {
        FakeCard card;
        card.has_off = false;
        EXPECT_EQ(card.hand_over().outcome, HandoverOutcome::NoLever);
        EXPECT_TRUE(card.switches.empty());
    }
    {
        FakeCard card;
        card.switchable = false;
        const HandoverResult result = card.hand_over();
        EXPECT_EQ(result.outcome, HandoverOutcome::NoLever);
        EXPECT_FALSE(released(result.outcome));
        EXPECT_EQ(card.waits, 0);
    }
}

// A card found already off with the link up: wait for it, then leave it on
// the phone's usual profile.
TEST(CallAudioHandover, ACardAlreadyOffIsNotSwitchedOffTwice) {
    FakeCard card;
    card.active = "off";
    card.waits_since_off = 0;
    const HandoverResult result = card.hand_over();
    EXPECT_EQ(result.outcome, HandoverOutcome::Released);
    EXPECT_EQ(card.switches, (std::vector<std::string>{"audio-gateway"}));
}

TEST(CallAudioHandover, RestoringLeavesAVoiceProfileAlone) {
    FakeCard card;
    EXPECT_TRUE(restore_voice_profile(card, ADDRESS, "audio-gateway"));
    EXPECT_TRUE(card.switches.empty());

    card.active = "off";
    EXPECT_TRUE(restore_voice_profile(card, ADDRESS, ""));
    EXPECT_EQ(card.switches, (std::vector<std::string>{"audio-gateway"}));

    card.present = false;
    EXPECT_FALSE(restore_voice_profile(card, ADDRESS, ""));
}

TEST(CallAudioHandover, OutcomesHaveNames) {
    EXPECT_STREQ(to_string(HandoverOutcome::Released), "released");
    EXPECT_STREQ(to_string(HandoverOutcome::ReleasedParked), "released_parked");
    EXPECT_STREQ(to_string(HandoverOutcome::StillHere), "still_here");
    EXPECT_STREQ(to_string(HandoverOutcome::NoLever), "no_lever");
    EXPECT_STREQ(to_string(HandoverOutcome::AlreadyOnPhone), "already_on_phone");
}

// ---- The same, through the client that serves bt_call_action ---------------

namespace {

    // PipeWire's hands-free under the test's control: every call on the bus
    // succeeds and is written down, and the transport state follows the card.
    class ScriptedSource : public TelephonySource {
    public:
        TelephonySnapshot snap;
        FakeCard* card = nullptr;
        mutable std::vector<std::string> calls;

        TelephonyIds ids() const override {
            return {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
        }
        GDBusConnection* connection() const override { return nullptr; }
        TelephonySnapshot snapshot() const override {
            TelephonySnapshot now = snap;
            if (card)
                now.audio_state = card->link_up ? "active" : "idle";
            return now;
        }
        bool invoke(const std::string&, const char*, const char* method, GVariant* args, std::string&) override {
            std::string line = method;
            if (args) {
                g_variant_ref_sink(args);
                if (g_variant_is_of_type(args, G_VARIANT_TYPE("(ssv)"))) {
                    const char* iface = nullptr;
                    const char* name = nullptr;
                    GVariant* value = nullptr;
                    g_variant_get(args, "(&s&sv)", &iface, &name, &value);
                    line += std::string(" ") + name + "=" + (g_variant_get_boolean(value) ? "true" : "false");
                    if (std::string(name) == "RejectSCO")
                        snap.reject_sco = g_variant_get_boolean(value) != FALSE;
                    g_variant_unref(value);
                }
                g_variant_unref(args);
            }
            calls.push_back(line);
            return true;
        }
    };

    struct Rig {
        ScriptedSource* source = nullptr;
        FakeCard* card = nullptr;
        std::unique_ptr<TelephonyClient> client;
    };

    Rig rig_in_a_call() {
        Rig rig;
        auto source = std::make_unique<ScriptedSource>();
        auto card = std::make_unique<FakeCard>();
        rig.source = source.get();
        rig.card = card.get();
        source->card = rig.card;
        source->snap.gateway.path = "/org/pipewire/Telephony/ag0";
        source->snap.gateway.state = "connected";
        source->snap.reject_sco = false;
        Call call;
        call.path = "/org/pipewire/Telephony/ag0/call1";
        call.state = "active";
        source->snap.calls.push_back(call);

        std::vector<std::unique_ptr<TelephonySource>> sources;
        sources.push_back(std::move(source));
        rig.client = std::make_unique<TelephonyClient>(std::move(sources), ADDRESS, std::move(card));
        FakeCard* raw = rig.card;
        rig.client->set_sleep([raw](int ms) { raw->wait(ms); });
        return rig;
    }

} // namespace

TEST(CallAudioHandoverClient, AudioPhoneDuringACallClosesTheVoiceLink) {
    Rig rig = rig_in_a_call();
    ASSERT_EQ(rig.client->status().value("audio_route", ""), "here");

    std::string err;
    EXPECT_TRUE(rig.client->call_action("", "audio_phone", err)) << err;

    EXPECT_EQ(rig.source->calls, (std::vector<std::string>{"Set RejectSCO=true"}))
        << "the next link is refused first; the call itself is never touched";
    EXPECT_EQ(rig.card->switches, (std::vector<std::string>{"off", "audio-gateway"}));
    const nlohmann::json status = rig.client->status();
    EXPECT_EQ(status.value("audio_route", ""), "phone");
    EXPECT_EQ(status["audio_reject"], true);
    EXPECT_FALSE(status.value("audio_parked", true));
    EXPECT_EQ(status.value("calls", 0), 1) << "the call is still there";
}

// The answer is the truth, not the request: a link that would not close is a
// failure, and the status keeps saying the sound is here.
TEST(CallAudioHandoverClient, ALinkThatStaysUpFailsTheAction) {
    Rig rig = rig_in_a_call();
    rig.card->closes_after = -1;

    std::string err;
    EXPECT_FALSE(rig.client->call_action("", "audio_phone", err));
    EXPECT_NE(err.find("still on this computer"), std::string::npos);
    EXPECT_EQ(rig.client->status().value("audio_route", ""), "here");
    EXPECT_EQ(rig.card->active, "audio-gateway");
}

TEST(CallAudioHandoverClient, WithoutACardTheActionFailsWhileTheLinkIsUp) {
    auto source = std::make_unique<ScriptedSource>();
    source->snap.gateway.path = "/org/pipewire/Telephony/ag0";
    source->snap.audio_state = "active";
    ScriptedSource* raw = source.get();
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(std::move(source));
    TelephonyClient client(std::move(sources), ADDRESS);

    std::string err;
    EXPECT_FALSE(client.call_action("", "audio_phone", err));
    EXPECT_FALSE(err.empty());

    // Before a call is taken there is no link to close: only the switch.
    raw->snap.audio_state = "idle";
    err.clear();
    EXPECT_TRUE(client.call_action("", "audio_phone", err));
    EXPECT_TRUE(err.empty());
}

// A card left off for the call goes back on its profile when the call ends.
TEST(CallAudioHandoverClient, AParkedCardIsRestoredWhenTheLastCallEnds) {
    Rig rig = rig_in_a_call();
    rig.card->reopens_on_restore = true;

    std::string err;
    ASSERT_TRUE(rig.client->call_action("", "audio_phone", err)) << err;
    EXPECT_EQ(rig.card->active, "off");
    EXPECT_TRUE(rig.client->status().value("audio_parked", false));

    rig.client->poll_calls();
    EXPECT_EQ(rig.card->active, "off") << "not under a call in progress";

    rig.card->reopens_on_restore = false;
    rig.source->snap.calls.clear();
    rig.client->poll_calls();
    EXPECT_EQ(rig.card->active, "audio-gateway");
    EXPECT_FALSE(rig.client->status().value("audio_parked", true));
}

// Taking the sound back during the call: the switch opens, a card left off is
// put back, and PipeWire is asked for the voice link.
TEST(CallAudioHandoverClient, AudioHereDuringACallRestoresTheCardThenActivates) {
    Rig rig = rig_in_a_call();
    rig.card->link_up = false;
    rig.card->active = "off";
    rig.source->snap.reject_sco = true;

    std::string err;
    EXPECT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    EXPECT_EQ(rig.source->calls, (std::vector<std::string>{"Set RejectSCO=false", "Activate"}));
    EXPECT_EQ(rig.card->switches, (std::vector<std::string>{"audio-gateway"}));
    EXPECT_EQ(rig.client->status()["audio_reject"], false);
}

TEST(CallAudioHandoverClient, AudioHereLeavesAHealthyCardAlone) {
    Rig rig = rig_in_a_call();
    rig.card->link_up = false;

    std::string err;
    EXPECT_TRUE(rig.client->call_action("", "audio_here", err)) << err;
    EXPECT_TRUE(rig.card->switches.empty());
    EXPECT_EQ(rig.source->calls.back(), "Activate");
}
