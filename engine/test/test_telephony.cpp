#include <gtest/gtest.h>
#include <tether/bluetooth/objects.hpp>
#include <tether/bluetooth/telephony.hpp>

using namespace tether::bluetooth;

namespace {

    // Same shape BlueZ returns from GetManagedObjects, parsed from GVariant text
    // so the tests need no bus.
    struct Payload {
        GVariant* v = nullptr;

        explicit Payload(const char* text) {
            GError* error = nullptr;
            v = g_variant_parse(G_VARIANT_TYPE("a{oa{sa{sv}}}"), text, nullptr, nullptr, &error);
            if (!v) {
                ADD_FAILURE() << "bad fixture: " << (error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
        ~Payload() {
            if (v)
                g_variant_unref(v);
        }
        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;
    };

    // Two phones, each with a gateway; one is ringing, the other is on a call.
    // Recorded from bluetoothd 5.87 --experimental against a live iPhone.
    constexpr const char* TWO_GATEWAYS = R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Address': <'02:00:00:00:00:01'>, 'Alias': <'Zack 15 Pro'> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0': {
        'org.bluez.Telephony1': {
          'UUID': <'0000111f-0000-1000-8000-00805f9b34fb'>,
          'State': <'connected'>,
          'OperatorName': <'AT&T'>,
          'SupportedURISchemes': <['tel']>,
          'Signal': <byte 1>,
          'BattChg': <byte 4>,
          'Service': <true>,
          'Roaming': <false>,
          'InbandRingtone': <false>
        }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0/call1': {
        'org.bluez.Call1': {
          'LineIdentification': <'+15555550123'>,
          'Name': <''>,
          'IncomingLine': <''>,
          'State': <'incoming'>,
          'Multiparty': <false>
        }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF': {
        'org.bluez.Device1': { 'Address': <'AA:BB:CC:DD:EE:FF'>, 'Alias': <'Other'> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0': {
        'org.bluez.Telephony1': { 'State': <'connected'>, 'Signal': <byte 5> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0/call1': {
        'org.bluez.Call1': { 'LineIdentification': <'+15555559999'>, 'State': <'active'> }
      }
    })";

} // namespace

TEST(DialString, KeepsDigitsAndLeadingPlus) {
    EXPECT_EQ(normalize_dial_string("+15555550123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("5555550123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("+1 (555) 555-0123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("555.555.0123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("\t555 555 0123 "), "5555550123");
}

// The dial string reaches the cellular network. Anything that is not a number is
// refused here rather than passed to BlueZ to judge.
TEST(DialString, RefusesEverythingElse) {
    EXPECT_TRUE(normalize_dial_string("").empty());
    EXPECT_TRUE(normalize_dial_string("+").empty());
    EXPECT_TRUE(normalize_dial_string("   ").empty());
    EXPECT_TRUE(normalize_dial_string("not a number").empty());
    // Supplementary service and AT syntax, the reason this check exists. BlueZ
    // would accept these characters; nothing in the UI asks for them.
    EXPECT_TRUE(normalize_dial_string("*21*5551234#").empty());
    EXPECT_TRUE(normalize_dial_string("555;ATH").empty());
    EXPECT_TRUE(normalize_dial_string("555\r\nATD666").empty());
    EXPECT_TRUE(normalize_dial_string("555,,123").empty());
    // A '+' anywhere but the front is not a country code.
    EXPECT_TRUE(normalize_dial_string("555+123").empty());
    EXPECT_TRUE(normalize_dial_string("++15555550123").empty());
    // Long enough to be a paste accident.
    EXPECT_TRUE(normalize_dial_string(std::string(33, '5')).empty());
    EXPECT_FALSE(normalize_dial_string(std::string(32, '5')).empty());
}

TEST(Telephony, ParsesTheGateway) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const Telephony* gw = objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01");
    ASSERT_NE(gw, nullptr);
    EXPECT_EQ(gw->path, "/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0");
    EXPECT_EQ(gw->operator_name, "AT&T");
    EXPECT_EQ(gw->signal, 1);
    EXPECT_EQ(gw->battery, 4);
    EXPECT_TRUE(gw->service);
    EXPECT_FALSE(gw->roaming);
    EXPECT_TRUE(gw->ready());
    EXPECT_EQ(gw->uri_schemes, std::vector<std::string>{"tel"});

    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_11_22_33_44_55_66"), nullptr);
    EXPECT_EQ(objects.find_telephony(""), nullptr);
}

// A second phone's calls must never appear under the selected one.
TEST(Telephony, ParsesOnlyTheSelectedGatewaysCalls) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const auto calls = objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].number, "+15555550123");
    EXPECT_EQ(calls[0].state, "incoming");
    EXPECT_TRUE(calls[0].ringing());
    EXPECT_FALSE(calls[0].connected());

    EXPECT_TRUE(objects.calls_for("").empty());
    EXPECT_TRUE(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony9").empty());
}

// The gateway path is a prefix of its calls' paths, and BlueZ numbers both per
// device: telephony1 must not collect telephony10's calls.
TEST(Telephony, GatewayPathIsMatchedWholeNotAsAPrefix) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony1/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony10/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony1").size(), 1u);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony10").size(), 1u);
}

// The objects exist only under bluetoothd --experimental and only while HFP is
// connected. Their absence is the normal state, not a parse failure.
TEST(Telephony, AbsentWhenHandsFreeIsNotConnected) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Address': <'02:00:00:00:00:01'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_TRUE(objects.telephony.empty());
    EXPECT_TRUE(objects.calls.empty());
    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01"), nullptr);
}

// A gateway that is still bringing up its service level connection is not ready
// to take a call.
TEST(Telephony, IsNotReadyBeforeTheServiceLevelConnection) {
    for (const char* state : {"connecting", "slc_connecting", "disconnecting"}) {
        Telephony gw;
        gw.state = state;
        EXPECT_FALSE(gw.ready()) << state;
    }
    Telephony gw;
    gw.state = "connected";
    EXPECT_TRUE(gw.ready());
}

TEST(Telephony, TracksACallThroughItsStates) {
    Call call;
    call.state = "incoming";
    EXPECT_TRUE(call.ringing());

    call.state = "active";
    EXPECT_FALSE(call.ringing());
    EXPECT_TRUE(call.connected());

    call.state = "held";
    EXPECT_TRUE(call.connected());

    call.state = "disconnected";
    EXPECT_FALSE(call.connected());
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.outgoing());

    // An outgoing call runs dialing then alerting before it connects.
    Call outgoing;
    outgoing.state = "dialing";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "alerting";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "active";
    EXPECT_FALSE(outgoing.outgoing());
}

// A state this build has not seen must still reach the UI rather than being
// dropped or guessed at. response_and_hold is one BlueZ can report.
TEST(Telephony, PassesUnknownStatesThrough) {
    Call call;
    call.state = "response_and_hold";
    EXPECT_EQ(call.state, "response_and_hold");
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.connected());
}

// "withheld" is the network refusing caller ID, not a number. Offering it to
// redial would dial nothing.
TEST(Telephony, WithheldCallerIdIsNotANumber) {
    Call call;
    call.number = "withheld";
    call.state = "incoming";
    EXPECT_TRUE(call.withheld());

    const nlohmann::json j = to_json(call);
    EXPECT_EQ(j["number"], "");
    EXPECT_TRUE(j["withheld"]);
    EXPECT_TRUE(normalize_dial_string(j["number"].get<std::string>()).empty());
}

TEST(Telephony, SerializesCallsForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0"));

    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["number"], "+15555550123");
    EXPECT_EQ(j[0]["state"], "incoming");
    EXPECT_TRUE(j[0]["ringing"]);
    EXPECT_FALSE(j[0]["connected"]);
    EXPECT_TRUE(to_json(std::vector<Call>{}).empty());
}

TEST(Telephony, SerializesTheGatewayForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(*objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01"));

    EXPECT_TRUE(j["available"]);
    EXPECT_EQ(j["operator"], "AT&T");
    EXPECT_EQ(j["signal"], 1);
    EXPECT_EQ(j["battery"], 4);
    EXPECT_FALSE(j["roaming"]);
}

namespace {

    // Recorded from pipewire 1.6.8 during a live call. The root manager reports
    // the gateways alone -- no calls, even with one active.
    constexpr const char* PIPEWIRE_ROOT = R"({
      '/org/pipewire/Telephony/ag1': {
        'org.pipewire.Telephony.AudioGateway1': {
          'Address': <'02:00:00:00:00:01'>,
          'SpeakerVolume': <byte 15>,
          'MicrophoneVolume': <byte 15>
        },
        'org.pipewire.Telephony.AudioGatewayTransport1': {
          'Codec': <byte 2>, 'State': <'active'>, 'RejectSCO': <false>
        }
      },
      '/org/pipewire/Telephony/ag2': {
        'org.pipewire.Telephony.AudioGateway1': { 'Address': <'AA:BB:CC:DD:EE:FF'> }
      }
    })";

    // ...and each gateway keeps its calls in an object manager of its own, whose
    // reply carries no gateway at all. Recorded from the same call.
    constexpr const char* PIPEWIRE_GATEWAY_CALLS = R"({
      '/org/pipewire/Telephony/ag1/call1': {
        'org.pipewire.Telephony.Call1': {
          'LineIdentification': <'18002662278'>,
          'IncomingLine': <''>,
          'Name': <''>,
          'Multiparty': <false>,
          'State': <'active'>
        }
      },
      '/org/pipewire/Telephony/ag2/call1': {
        'org.pipewire.Telephony.Call1': { 'LineIdentification': <'+15555559999'>, 'State': <'incoming'> }
      }
    })";

    // A source under the test's control, so TelephonyClient can be driven with
    // no bus at all.
    class FakeSource : public TelephonySource {
    public:
        TelephonySnapshot snap;
        TelephonyIds identity{"fake", "org.example", "org.example.Gateway1", nullptr, "org.example.Call1", true, true};

        TelephonyIds ids() const override { return identity; }
        // Null stands for "reachable but nothing to talk to", which is what
        // every invoke() in these tests should refuse on.
        GDBusConnection* connection() const override { return nullptr; }
        TelephonySnapshot snapshot() const override { return snap; }
    };

    std::unique_ptr<FakeSource> gateway_source(const char* path, const char* state = "connected") {
        auto source = std::make_unique<FakeSource>();
        source->snap.gateway.path = path;
        source->snap.gateway.state = state;
        return source;
    }

    std::unique_ptr<TelephonyClient> client_of(std::vector<std::unique_ptr<TelephonySource>> sources) {
        return std::make_unique<TelephonyClient>(std::move(sources), "02:00:00:00:00:01");
    }

} // namespace

TEST(PipewireTelephony, MatchesTheGatewayByAddress) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");

    EXPECT_EQ(snap.gateway.path, "/org/pipewire/Telephony/ag1") << "address match must be case-insensitive";
    EXPECT_TRUE(snap.gateway.ready()) << "a registered gateway means the service level connection is up";
    EXPECT_EQ(snap.audio_state, "active");
}

// The reply that carries the gateway carries no calls, so reading the call list
// from it is how the Hang up button goes missing.
TEST(PipewireTelephony, RootManagerCarriesNoCalls) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");
    EXPECT_TRUE(snap.calls.empty());
}

// The gateway's own manager has the calls and no gateway to match an address
// against, so they are collected by parentage instead.
TEST(PipewireTelephony, ScopesCallsToTheirOwnGateway) {
    Payload payload(PIPEWIRE_GATEWAY_CALLS);
    const std::vector<Call> calls = parse_pipewire_calls(payload.v, "/org/pipewire/Telephony/ag1");

    ASSERT_EQ(calls.size(), 1u) << "the other phone's call must not appear here";
    EXPECT_EQ(calls.front().number, "18002662278");
    EXPECT_TRUE(calls.front().connected());
    EXPECT_EQ(calls.front().path, "/org/pipewire/Telephony/ag1/call1");
    EXPECT_EQ(calls.front().telephony_path, "/org/pipewire/Telephony/ag1");
}

TEST(PipewireTelephony, CallsNeedAGatewayToBelongTo) {
    Payload payload(PIPEWIRE_GATEWAY_CALLS);
    EXPECT_TRUE(parse_pipewire_calls(payload.v, "").empty());
    EXPECT_TRUE(parse_pipewire_calls(payload.v, "/org/pipewire/Telephony/ag9").empty());
}

// A phone PipeWire is not serving must read as absent, never as "the only
// gateway there is" -- that would answer calls on someone else's phone.
TEST(PipewireTelephony, UnknownAddressYieldsNoGateway) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "11:22:33:44:55:66");

    EXPECT_TRUE(snap.gateway.path.empty());
    EXPECT_TRUE(snap.calls.empty());
    EXPECT_TRUE(snap.audio_state.empty());
}

// The indicators BlueZ reports have no PipeWire equivalent. They must stay at
// their defaults rather than being invented.
TEST(PipewireTelephony, ReportsNoCellularIndicators) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");

    EXPECT_TRUE(snap.gateway.operator_name.empty());
    EXPECT_EQ(snap.gateway.signal, 0);
    EXPECT_EQ(snap.gateway.battery, 0);
    EXPECT_FALSE(snap.gateway.service);
}

TEST(PipewireTelephony, EmptyPayloadIsNotAFailure) {
    Payload payload("@a{oa{sa{sv}}} {}");
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");
    EXPECT_TRUE(snap.gateway.path.empty());
}

// Order in the source vector is the whole selection policy: BlueZ carries the
// cellular indicators, so it wins wherever it has the profile.
TEST(TelephonyClientSources, PrefersTheFirstSourceWithAGateway) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(gateway_source("/org/bluez/hci0/dev_X/telephony0"));
    sources.push_back(gateway_source("/org/pipewire/Telephony/ag0"));
    const auto client = client_of(std::move(sources));

    EXPECT_EQ(client->status().value("path", ""), "/org/bluez/hci0/dev_X/telephony0");
    EXPECT_TRUE(client->available());
}

TEST(TelephonyClientSources, FallsThroughToTheNextSource) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(std::make_unique<FakeSource>());
    auto pipewire = gateway_source("/org/pipewire/Telephony/ag0");
    pipewire->identity = {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
    sources.push_back(std::move(pipewire));
    const auto client = client_of(std::move(sources));

    const nlohmann::json status = client->status();
    EXPECT_EQ(status.value("backend", ""), "pipewire");
    EXPECT_FALSE(status.value("indicators", true)) << "PipeWire reports no indicators, and must say so";
}

// With no source serving, the payload must read exactly as it did before a
// second stack existed.
TEST(TelephonyClientSources, NoSourceReadsAsUnavailable) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(std::make_unique<FakeSource>());
    const auto client = client_of(std::move(sources));

    const nlohmann::json status = client->status();
    EXPECT_FALSE(client->available());
    EXPECT_FALSE(status.value("available", true));
    EXPECT_EQ(status.value("backend", "unset"), "");
    EXPECT_TRUE(status.value("indicators", false)) << "an absent gateway must not claim indicators are missing";
    EXPECT_TRUE(client->calls().empty());
}

// Moving the audio is only meaningful where the stack carries it. BlueZ opens
// no voice link at all, and must say that rather than "unknown action".
TEST(TelephonyClientSources, AudioActionIsRefusedWithoutATransport) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(gateway_source("/org/bluez/hci0/dev_X/telephony0"));
    const auto client = client_of(std::move(sources));

    std::string err;
    EXPECT_FALSE(client->call_action("", "audio_here", err));
    EXPECT_NE(err.find("iPhone"), std::string::npos);
    EXPECT_EQ(err.find("Unknown"), std::string::npos);
}

// The one place a silent wire-format bug could hide: BlueZ's Dial takes a URI,
// PipeWire's takes the bare number and builds the AT command itself.
TEST(TelephonyClientSources, DialArgumentMatchesTheStack) {
    const TelephonyIds bluez{"bluez", "org.bluez", "g", nullptr, "c", true, true};
    const TelephonyIds pipewire{"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};

    EXPECT_EQ(dial_argument(bluez, "+15555550123"), "tel:+15555550123");
    EXPECT_EQ(dial_argument(pipewire, "+15555550123"), "+15555550123");
}

// The journal must say that a call happened without saying to whom.
TEST(CallJournal, MasksAllButTheLastTwoDigits) {
    EXPECT_EQ(mask_number("+41791234567"), "***67");
    EXPECT_EQ(mask_number("079 123 45 67"), "***67");
    EXPECT_EQ(mask_number("112"), "***12");
    // Nothing would be left hidden.
    EXPECT_EQ(mask_number("42"), "***");
    EXPECT_EQ(mask_number(""), "withheld number");
    EXPECT_EQ(mask_number("+"), "withheld number");
}

namespace {

    nlohmann::json call_entry(const std::string& path, const std::string& number, const std::string& state) {
        return {{"path", path}, {"number", number}, {"state", state}};
    }

} // namespace

TEST(CallJournal, AnIncomingCallIsAnsweredThenEnds) {
    const nlohmann::json none = nlohmann::json::array();
    const nlohmann::json ringing = nlohmann::json::array({call_entry("/call1", "+41791234567", "incoming")});
    const nlohmann::json talking = nlohmann::json::array({call_entry("/call1", "+41791234567", "active")});

    const auto arrived = describe_call_changes(none, ringing);
    ASSERT_EQ(arrived.size(), 1u);
    EXPECT_EQ(arrived[0], "call: incoming from ***67 (incoming)");

    const auto answered = describe_call_changes(ringing, talking);
    ASSERT_EQ(answered.size(), 1u);
    EXPECT_EQ(answered[0], "call: answered, ***67 (incoming -> active)");

    const auto ended = describe_call_changes(talking, none);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0], "call: ended, ***67 (was active)");
}

// A call nobody picked up still leaves its two lines.
TEST(CallJournal, AMissedCallIsSeenArrivingAndEnding) {
    const nlohmann::json none = nlohmann::json::array();
    const nlohmann::json ringing = nlohmann::json::array({call_entry("/call1", "", "incoming")});

    EXPECT_EQ(describe_call_changes(none, ringing).front(), "call: incoming from withheld number (incoming)");
    EXPECT_EQ(describe_call_changes(ringing, none).front(), "call: ended, withheld number (was incoming)");
}

TEST(CallJournal, OutgoingHoldAndUnchangedCalls) {
    const nlohmann::json none = nlohmann::json::array();
    const nlohmann::json dialing = nlohmann::json::array({call_entry("/call1", "0791234567", "dialing")});
    const nlohmann::json alerting = nlohmann::json::array({call_entry("/call1", "0791234567", "alerting")});
    const nlohmann::json active = nlohmann::json::array({call_entry("/call1", "0791234567", "active")});
    const nlohmann::json held = nlohmann::json::array({call_entry("/call1", "0791234567", "held")});

    EXPECT_EQ(describe_call_changes(none, dialing).front(), "call: outgoing to ***67 (dialing)");
    EXPECT_EQ(describe_call_changes(dialing, alerting).front(), "call: ***67 dialing -> alerting");
    EXPECT_EQ(describe_call_changes(alerting, active).front(), "call: answered, ***67 (alerting -> active)");
    EXPECT_EQ(describe_call_changes(active, held).front(), "call: ***67 active -> held");
    // Taking a call off hold is not answering it.
    EXPECT_EQ(describe_call_changes(held, active).front(), "call: ***67 held -> active");
    EXPECT_TRUE(describe_call_changes(active, active).empty());
}

// The first reading has nothing to compare with: a null list is an empty one.
TEST(CallJournal, TheFirstReadingAndTwoCallsAtOnce) {
    const nlohmann::json two = nlohmann::json::array(
        {call_entry("/call1", "0791234567", "active"), call_entry("/call2", "0227654321", "waiting")});

    const auto lines = describe_call_changes(nullptr, two);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "call: in progress with ***67 (active)");
    EXPECT_EQ(lines[1], "call: incoming from ***21 (waiting)");

    for (const auto& line : describe_call_changes(two, nullptr))
        EXPECT_EQ(line.find("0791234567"), std::string::npos);
    EXPECT_EQ(describe_call_changes(two, nullptr).size(), 2u);
}

// --- Where the sound of a call goes -----------------------------------------

TEST(PipewireTelephony, ReadsWhetherTheVoiceLinkIsRefused) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");
    ASSERT_TRUE(snap.reject_sco.has_value());
    EXPECT_FALSE(*snap.reject_sco) << "recorded on a stock PipeWire: the voice link is accepted";

    // The other gateway carries no transport: nothing to say.
    const TelephonySnapshot other = parse_pipewire_telephony(payload.v, "AA:BB:CC:DD:EE:FF");
    EXPECT_FALSE(other.reject_sco.has_value());
}

TEST(TelephonyClientSources, StatusSaysWhereTheCallAudioIs) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    auto pipewire = gateway_source("/org/pipewire/Telephony/ag0");
    pipewire->identity = {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
    pipewire->snap.audio_state = "active";
    pipewire->snap.reject_sco = false;
    FakeSource* raw = pipewire.get();
    sources.push_back(std::move(pipewire));
    const auto client = client_of(std::move(sources));

    nlohmann::json status = client->status();
    EXPECT_TRUE(status.value("audio_switchable", false));
    EXPECT_EQ(status.value("audio_route", ""), "here");
    EXPECT_EQ(status["audio_reject"], false);

    raw->snap.audio_state = "idle";
    raw->snap.reject_sco = true;
    status = client->status();
    EXPECT_EQ(status.value("audio_route", ""), "phone");
    EXPECT_EQ(status["audio_reject"], true);
}

// BlueZ's hands-free never carries the audio: on the phone, and not switchable.
TEST(TelephonyClientSources, AStackWithoutCallAudioSaysSo) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(gateway_source("/org/bluez/hci0/dev_X/telephony0"));
    const auto client = client_of(std::move(sources));

    const nlohmann::json status = client->status();
    EXPECT_FALSE(status.value("audio_switchable", true));
    EXPECT_EQ(status.value("audio_route", ""), "phone");
    EXPECT_TRUE(status["audio_reject"].is_null());
}

TEST(TelephonyClientSources, WifiCallingIsReadOffTheNetworkName) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    auto bluez = gateway_source("/org/bluez/hci0/dev_X/telephony0");
    bluez->snap.gateway.operator_name = "Carrier WiFi-Calling";
    FakeSource* raw = bluez.get();
    sources.push_back(std::move(bluez));
    const auto client = client_of(std::move(sources));
    EXPECT_TRUE(client->status().value("wifi_calling", false));
    raw->snap.gateway.operator_name = "Carrier";
    EXPECT_FALSE(client->status().value("wifi_calling", true));
}

// A switch that cannot be written must not cost the call list.
TEST(TelephonyClientSources, PollingReturnsTheCallsEvenWhenTheSwitchCannotBeWritten) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    auto pipewire = gateway_source("/org/pipewire/Telephony/ag0");
    pipewire->identity = {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
    pipewire->snap.reject_sco = false;
    Call call;
    call.path = "/org/pipewire/Telephony/ag0/call1";
    call.state = "incoming";
    pipewire->snap.calls.push_back(call);
    sources.push_back(std::move(pipewire));
    const auto client = client_of(std::move(sources));

    EXPECT_EQ(client->poll_calls().size(), 1u);
    EXPECT_EQ(client->poll_calls(), client->calls());
}
