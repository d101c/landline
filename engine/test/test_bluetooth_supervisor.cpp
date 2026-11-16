#include <gtest/gtest.h>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/objects.hpp>
#include <tether/bluetooth/profile_supervisor.hpp>

#include <tether/log.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    class FakeBearer : public BearerOps {
    public:
        bool present = true;
        bool paired = true;
        bool classic = false;
        bool le_available = true;
        bool le = false;
        bool classic_succeeds = true;
        bool le_succeeds = true;

        int classic_attempts = 0;
        int le_attempts = 0;

        bool device_present() const override { return present; }
        bool device_paired() const override { return paired; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return le_available; }
        bool le_connected() const override { return le; }

        // BlueZ's default is "last-seen"; only the pin the Classic fallback
        // writes matters here.
        std::string bearer_pref = "last-seen";
        std::vector<std::string> bearer_writes;
        std::string preferred_bearer() const override { return bearer_pref; }
        void set_preferred_bearer(const std::string& bearer) override {
            bearer_pref = bearer;
            bearer_writes.push_back(bearer);
        }

        std::string classic_error = "br/edr refused";
        std::string le_error = "le refused";

        ConnectResult connect_classic(std::string& err) override {
            ++classic_attempts;
            if (!classic_succeeds) {
                err = classic_error;
                return ConnectResult::Failed;
            }
            classic = true;
            return ConnectResult::Requested;
        }

        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            if (!le_succeeds) {
                err = le_error;
                return ConnectResult::Failed;
            }
            le = true;
            return ConnectResult::Requested;
        }

        // Models BlueZ holding the accept-list registration open. Tests that
        // care set this; the default of "nothing outstanding" keeps the older
        // cases reading as they did.
        bool outstanding = false;
        bool le_connect_outstanding() const override { return outstanding; }

        // The healthy case: the adapter accepted the solicitation. Only a test
        // about an adapter that will not advertise clears this.
        bool on_air = true;
        bool solicitation_on_air() const override { return on_air; }

        // Hands-free connected, as the bus shows it. Tests about the BR/EDR
        // cycle clear this.
        bool telephony = true;
        int classic_disconnects = 0;

        bool telephony_present() const override { return telephony; }

        // Counts the requests for hands-free over a live link, and can model
        // one that works.
        int hands_free_requests = 0;
        bool hands_free_request_works = false;
        bool request_hands_free() override {
            ++hands_free_requests;
            if (hands_free_request_works)
                telephony = true;
            return true;
        }

        int refusals_forgotten = 0;
        void forget_classic_refusal() override { ++refusals_forgotten; }

        bool disconnect_classic(std::string&) override {
            ++classic_disconnects;
            classic = false;
            return true;
        }

        // The keys as BlueZ reports them. Unset, only Device1.Paired is known,
        // which is what the older cases assume.
        bool facts_known = false;
        BondFacts facts;
        BondFacts bond_facts() const override {
            if (facts_known)
                return facts;
            return BearerOps::bond_facts();
        }

        // Disconnected signals with Reason.Authentication, as the monitor counts them.
        uint64_t auth_failures = 0;
        uint64_t auth_failure_count() const override { return auth_failures; }
    };

    class FakeProfiles : public ProfileOps {
    public:
        std::string map_error;
        std::string pbap_error;
        int map_attempts = 0;
        int pbap_attempts = 0;
        std::vector<std::string> removed;
        int session_counter = 0;

        std::string create_session(const std::string& target, std::string& err) override {
            const bool is_map = target == "map";
            (is_map ? map_attempts : pbap_attempts)++;
            const std::string& failure = is_map ? map_error : pbap_error;
            if (!failure.empty()) {
                err = failure;
                return {};
            }
            return "/session" + std::to_string(++session_counter);
        }

        void remove_session(const std::string& path) override { removed.push_back(path); }
    };

} // namespace

// BR/EDR must come up first; LE must wait out the settle window. Connecting LE
// too early leaves the bond half-connected and flapping.
TEST(BearerSupervisor, ConnectsClassicThenLeAfterSettling) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE must not be attempted before Classic is up";

    // Classic is up at t=1, but the settle window has not elapsed.
    sup.tick(1);
    EXPECT_EQ(ops.le_attempts, 0);
    sup.tick(1 + BEARER_SETTLE_SECONDS - 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE attempted before the settle window elapsed";

    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(ops.le_attempts, 1);
    EXPECT_FALSE(sup.status().le_connected) << "BlueZ accepting a connect is not a link";

    // Only an observed Connected is one.
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().le_connected);
}

// #301: the old wording blamed BlueZ, which sent reporters into bluetoothd config.
TEST(BearerSupervisor, ClassicOnlyBondPointsAtTheAdapter) {
    FakeBearer ops;
    ops.classic = true;
    ops.le_available = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    const auto& reason = sup.status().reason;
    EXPECT_NE(reason.find("no LE keys"), std::string::npos) << reason;
    EXPECT_NE(reason.find(ADAPTER_DOC_URL), std::string::npos) << reason;
}

TEST(BearerSupervisor, ClassicBackoffGrowsAndCaps) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);

    // Ticking inside the backoff window must not retry.
    sup.tick(BEARER_BACKOFF_MIN_SECONDS - 1);
    EXPECT_EQ(ops.classic_attempts, 1) << "retried before the backoff expired";

    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    EXPECT_EQ(ops.classic_attempts, 2);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS * 2);

    // Drive many failures and confirm the cap holds.
    int64_t now = BEARER_BACKOFF_MIN_SECONDS;
    for (int i = 0; i < 20; ++i) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MAX_SECONDS);
}

// InProgress means BlueZ is still finishing an operation of its own, which
// clears without help. Treating it like a refusal pushed the retry out to
// minutes and left LE — and so ANCS — down long after the cause was gone.
// InProgress means BlueZ is still working on the last connect, which it keeps
// doing long after a caller gives up. Retrying at the same cadence guarantees
// the next attempt collides with it too, so one stalled connect becomes a
// permanent storm. Standing further back is the only thing that clears it.
TEST(BearerSupervisor, InProgressWidensTheClassicWindow) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    int previous = 0;
    for (int i = 0; i < 5; ++i) {
        sup.tick(now);
        EXPECT_GT(sup.status().classic_backoff, previous) << "collided again at the same cadence";
        previous = sup.status().classic_backoff;
        now += previous;
    }
    EXPECT_EQ(ops.classic_attempts, 5);
}

// One dial per daemon, then the advert owns the radio. Measured 2026-08-22: the
// solicitation went on air at t=127.668 and the iPhone opened the link inbound
// 1.3s later, while three captures totalling ~700s produced no link from a dial
// -- including one where the phone was advertising with a dial armed. reset()
// must not refund the dial: a Classic flap would otherwise take the advert back
// off air for the length of another attempt.
TEST(BearerSupervisor, DialsLeOnceThenLeavesItToTheSolicitation) {
    FakeBearer ops;
    ops.le_succeeds = false; // BlueZ answers, no link follows
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    for (; now < 400; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.le_attempts, 1) << "kept dialling past the one attempt";

    // A Classic flap, and the phone coming back, must not buy another.
    ops.classic = false;
    sup.tick(now++);
    sup.reset();
    ops.classic_succeeds = true;
    for (int i = 0; i < 400; ++i)
        sup.tick(now++);
    EXPECT_EQ(ops.le_attempts, 1) << "reset() refunded the dial and took the advert off air again";
}

// The advert must not go up while a dial is in flight -- that is what aborted
// the connect once per cycle (2026-08-20). le_dialling gates the solicitation,
// so it stays true until the reply lands and false forever after.
TEST(BearerSupervisor, SolicitationIsHeldOffOnlyWhileTheOneDialIsInFlight) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true;
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ASSERT_EQ(ops.le_attempts, 1);
    EXPECT_TRUE(sup.status().le_dialling) << "solicited on top of a dial still in flight";

    ops.outstanding = false;
    sup.tick(++now);
    EXPECT_FALSE(sup.status().le_dialling);
    for (int i = 0; i < 500; ++i) {
        sup.tick(++now);
        ASSERT_FALSE(sup.status().le_dialling) << "took the solicitation off air again at t=" << now;
    }
}

// A dial outstanding when BR/EDR drops must not latch the solicitation off air.
// Nothing else on the Classic-down path re-reads it, and the advert is the only
// thing that brings LE back -- so a stale `true` here is a permanent outage on a
// host whose Classic link never returns (2026-08-23).
TEST(BearerSupervisor, ClassicDroppingUnderADialDoesNotLatchTheSolicitationOffAir) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true;
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ASSERT_EQ(ops.le_attempts, 1);
    ASSERT_TRUE(sup.status().le_dialling);

    // BR/EDR goes and does not come back, which is what a host with no locally
    // connectable profile looks like.
    ops.classic = false;
    ops.classic_succeeds = false;
    sup.tick(++now);
    EXPECT_TRUE(sup.status().le_dialling) << "the dial really is still in flight";

    // BlueZ answers. Nothing re-enters the LE branch, so this is the only place
    // the flag can be cleared.
    ops.outstanding = false;
    for (int i = 0; i < 500; ++i)
        sup.tick(++now);
    EXPECT_FALSE(sup.status().le_dialling)
        << "a finished dial kept the advert off air for the whole time BR/EDR was down";

    BearerStatus bearer = sup.status();
    bearer.le_available = true;
    EXPECT_TRUE(should_solicit_ancs(true, bearer, now, -1)) << "the advert is what brings LE back";
}

// A dialled LE link can come up carrying no ANCS at all, and while it reads
// connected the solicitation stays off air -- so nothing ever asks the phone for
// the service, and notifications never arrive (2026-08-23).
TEST(AncsSolicitation, GoesBackOnAirOverAnLeLinkThatCarriesNoAncs) {
    BearerStatus bearer;
    bearer.le_available = true;
    bearer.le_connected = true;

    // Offered, or not yet timed: the link is doing its job, leave it alone.
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, -1));
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 100, 100)) << "solicited before discovery could finish";
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 100 + ANCS_ABSENT_GRACE_SECONDS - 1, 100));

    EXPECT_TRUE(should_solicit_ancs(true, bearer, 100 + ANCS_ABSENT_GRACE_SECONDS, 100))
        << "left the advert off air over a link that carries no ANCS";

    // A dial in flight still owns the radio: the advert aborts it (2026-08-20).
    bearer.le_dialling = true;
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, 100));
    bearer.le_dialling = false;

    // LE down is the original case, and the timer is irrelevant to it.
    bearer.le_connected = false;
    EXPECT_TRUE(should_solicit_ancs(true, bearer, 10'000, -1));

    // Nothing goes on air for a disabled feature or an adapter without LE.
    EXPECT_FALSE(should_solicit_ancs(false, bearer, 10'000, -1));
    bearer.le_available = false;
    EXPECT_FALSE(should_solicit_ancs(true, bearer, 10'000, -1));
}

// A phone that is answering answers in about a second. A long silence with the
// advert up is the iPhone having gone quiet, and only cycling Bluetooth on the
// phone clears that -- so the status has to say so rather than "waiting" forever.
TEST(BearerSupervisor, NamesThePhoneCycleAfterALongSilence) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_NE(sup.status().reason.find("Waiting for the iPhone to open the LE link"), std::string::npos);

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not answering on LE"), std::string::npos);
    EXPECT_NE(sup.status().reason.find("turn Bluetooth off"), std::string::npos);
}

// Same silence, but the adapter never put the solicitation on air, so the phone
// was never asked for the service. Sending that user to cycle Bluetooth on the
// iPhone is the #128 dead end: nothing on the phone is wrong.
TEST(BearerSupervisor, BlamesTheAdapterWhenNothingWasEverSolicited) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.on_air = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not putting the notification request on air"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("turn Bluetooth off"), std::string::npos) << "blames the phone";
}

// #128: the Classic fallback pins the bond to bredr, and a bond left pinned
// refuses the inbound LE link the iPhone opens. Nothing used to hand it back, so
// the pin outlived every re-pair and LE stayed down for good.
TEST(BearerSupervisor, HandsTheBearerPreferenceBackToLeOnceClassicSettles) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    EXPECT_TRUE(ops.bearer_writes.empty()) << "wrote before the Classic link settled";

    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_EQ(ops.bearer_writes, std::vector<std::string>{"le"});

    // One write per Classic session, not one per poll.
    sup.tick(++now);
    sup.tick(++now);
    EXPECT_EQ(ops.bearer_writes.size(), 1u);
}

// Writing the property under a dial in flight is what cancelled the connect as
// le-connection-abort-by-local.
TEST(BearerSupervisor, DoesNotWriteTheBearerPreferenceOverADialInFlight) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    ops.outstanding = true;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_TRUE(ops.bearer_writes.empty()) << "wrote over an outstanding connect";

    ops.outstanding = false;
    sup.tick(++now);
    EXPECT_EQ(ops.bearer_writes, std::vector<std::string>{"le"});
}

// A bond still reading bredr after the silence is this computer's doing. Sending
// that user to cycle Bluetooth on the iPhone is the other #128 dead end.
TEST(BearerSupervisor, BlamesThePinnedBondRatherThanThePhone) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.bearer_pref = "bredr";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ops.bearer_pref = "bredr"; // the write has not taken effect on the bus yet

    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("pinned the bond to BR/EDR"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("turn Bluetooth off"), std::string::npos) << "blames the phone";
}

// The advert is legitimately off air while a dial owns the radio. One tick of
// that must not turn into "your adapter is broken".
TEST(BearerSupervisor, OneMomentOffAirDoesNotBlameTheAdapter) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);

    ops.on_air = false;
    sup.tick(now + LE_SILENT_SECONDS);
    EXPECT_NE(sup.status().reason.find("not answering on LE"), std::string::npos);
}

TEST(BearerSupervisor, RecoversAfterDisconnect) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);

    // The phone walks out of range.
    ops.classic = false;
    ops.le = false;
    sup.tick(100);
    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_FALSE(sup.status().le_connected);
    EXPECT_EQ(ops.classic_attempts, 2);

    // ...and comes back, with the iPhone answering the solicitation as it does
    // on real hardware -- the daemon spent its one dial long ago.
    sup.tick(101);
    ops.le = true;
    sup.tick(102 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected);
}

// reset() models a resume: recovery should start at once, not after waiting out
// a backoff accumulated before the machine slept.
TEST(BearerSupervisor, ResetClearsBackoffForImmediateRetry) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    ASSERT_EQ(ops.classic_attempts, 2);

    sup.reset();
    ops.classic_succeeds = true;
    sup.tick(BEARER_BACKOFF_MIN_SECONDS + 1);
    EXPECT_EQ(ops.classic_attempts, 3) << "reset must allow an immediate retry";
}

TEST(BearerSupervisor, SkipsLeWhenAncsDisabled) {
    FakeBearer ops;
    BearerSupervisor sup(ops, false);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(sup.status().classic_connected);
}

TEST(BearerSupervisor, SkipsLeWhenBearerUnavailable) {
    FakeBearer ops;
    ops.le_available = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_FALSE(sup.status().le_available);
}

TEST(BearerSupervisor, ReportsMissingDevice) {
    FakeBearer ops;
    ops.present = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_FALSE(sup.status().device_present);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial a device BlueZ does not know";
}

// A known-but-unpaired device happens after an unpair or after the phone forgets
// this computer. Dialing it collides with the pairing transaction, and BlueZ
// answers both attempts with br-connection-busy.
TEST(BearerSupervisor, StandsDownWhenDeviceIsUnpaired) {
    FakeBearer ops;
    ops.paired = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_POLL_SECONDS * 20);
    EXPECT_TRUE(sup.status().device_present);
    EXPECT_FALSE(sup.status().device_paired);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial an unpaired device";
    EXPECT_EQ(ops.le_attempts, 0);
}

// Forbidden and Busy look similar but call for opposite advice. Reporting a
// permissions problem as a busy session sends the user off re-pairing for
// nothing, and vice versa.
TEST(ObexErrors, DistinguishesForbiddenFromBusy) {
    EXPECT_EQ(classify_obex_error("Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("org.bluez.obex.Error.Forbidden: Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("OBEX response 0x43"), ObexError::Forbidden);

    EXPECT_EQ(classify_obex_error("Connection refused (111)"), ObexError::Busy);
    EXPECT_EQ(classify_obex_error("connection refused"), ObexError::Busy);
}

// Observed on real hardware with the iPhone bonded, connected, and all three
// permission toggles verified on. Classifying it as Forbidden would send the
// user to settings that are already correct.
TEST(ObexErrors, ServiceRecordMissingIsNotAPermissionProblem) {
    const std::string real = "GDBus.Error:org.bluez.obex.Error.Failed: Unable to find service record";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoRecord);
    EXPECT_NE(classify_obex_error(real), ObexError::Forbidden);

    // obexd reports a missing record whenever its SDP fetch is refused, so the
    // advice must name both plausible causes and must not send the user
    // re-pairing, which fixes neither.
    const std::string advice = obex_error_advice(ObexError::NoRecord, "map");
    EXPECT_NE(advice.find("another computer"), std::string::npos);
    EXPECT_NE(advice.find("Show Message Notifications"), std::string::npos);
    EXPECT_NE(advice.find("re-pairing is not the fix"), std::string::npos);
}

// With bluetooth.disable_ertm set, obexd's L2CAP connect fails locally and is
// reported as a missing record; blaming the phone toggle sends the user nowhere.
TEST(ObexErrors, DisabledErtmIsNamedInsteadOfThePhone) {
    const std::string advice = obex_error_advice(ObexError::NoRecord, "pbap", true);
    EXPECT_NE(advice.find("disable_ertm"), std::string::npos);
    EXPECT_EQ(advice.find("Sync Contacts"), std::string::npos);
    EXPECT_EQ(advice.find("another computer"), std::string::npos);
}

TEST(ObexErrors, ClassifiesUnavailableAndOther) {
    EXPECT_EQ(classify_obex_error("Host is down"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error("Page Timeout"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error(""), ObexError::None);
    EXPECT_EQ(classify_obex_error("something entirely new"), ObexError::Other);
}

TEST(ObexErrors, MissingObexdIsNotBlamedOnThePhone) {
    // Verbatim from a machine where bluez-obex had been uninstalled.
    const std::string real = "GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: The name is not activatable";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoDaemon);

    const std::string advice = obex_error_advice(ObexError::NoDaemon, "map");
    EXPECT_NE(advice.find("bluez-obex"), std::string::npos);
    // Nothing about this is fixable on the phone, so the advice must not say so.
    EXPECT_EQ(advice.find("iPhone's Settings"), std::string::npos);
    EXPECT_EQ(advice.find("re-pair"), std::string::npos);
}

TEST(ObexErrors, AdviceNamesThePermissionToggle) {
    const std::string map_advice = obex_error_advice(ObexError::Forbidden, "map");
    EXPECT_NE(map_advice.find("Show Message Notifications"), std::string::npos);
    // Must not send the user re-pairing over a permission.
    EXPECT_EQ(map_advice.find("re-pair"), std::string::npos);

    EXPECT_NE(obex_error_advice(ObexError::Forbidden, "pbap").find("Sync Contacts"), std::string::npos);
    EXPECT_NE(obex_error_advice(ObexError::Busy, "map").find("another computer"), std::string::npos);
}

TEST(ProfileSupervisor, OpensBothSessionsWhenTheDeviceIsReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_FALSE(sup.map_session().empty());
    EXPECT_FALSE(sup.pbap_session().empty());
}

TEST(ProfileSupervisor, DoesNothingUntilTheDeviceIsReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, false);
    EXPECT_EQ(ops.map_attempts, 0);
    EXPECT_EQ(ops.pbap_attempts, 0);
}

// A Forbidden may be a stale obexd session from a previous run, so exactly one
// retry is allowed. It must never escalate to restarting obexd.
TEST(ProfileSupervisor, RetriesOnceAfterForbidden) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_EQ(ops.map_attempts, 2) << "expected exactly one retry after Forbidden";
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);

    // PBAP opened, so the supervisor is on the steady cadence even though MAP is
    // still failing. The extra Forbidden retry is spent, so this poll makes
    // exactly one attempt — a persistent permission problem must not turn into a
    // retry burst against obexd.
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(ops.map_attempts, 3);
}

// While nothing has opened yet, the fast cadence applies so a user flipping the
// permission toggles is picked up promptly.
TEST(ProfileSupervisor, UsesFastCadenceUntilSomethingOpens) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;

    sup.tick(PROFILE_INITIAL_POLL_SECONDS - 1, true);
    EXPECT_EQ(ops.map_attempts, attempts);

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, PartialSuccessKeepsRetryingTheOtherProfile) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_FALSE(sup.status().pbap_open);

    const int map_attempts_after_open = ops.map_attempts;
    ops.pbap_error.clear();
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_EQ(ops.map_attempts, map_attempts_after_open) << "must not reopen an already-open session";
}

TEST(ProfileSupervisor, RespectsPollInterval) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ops.pbap_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;
    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts, attempts) << "retried before the poll interval elapsed";

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, ResetRemovesSessions) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);

    sup.reset();
    EXPECT_EQ(ops.removed.size(), 2u);
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.map_session().empty());

    // And reopens on the next tick.
    sup.tick(100, true);
    EXPECT_TRUE(sup.status().map_open);
}

// The iPhone serves one MAP session at a time, so a supervisor that is dropped
// without releasing its sessions blocks the next one until obexd is restarted.
TEST(ProfileSupervisor, DestructionReleasesSessions) {
    FakeProfiles ops;
    {
        ProfileSupervisor sup(ops);
        sup.tick(0, true);
        ASSERT_TRUE(sup.status().map_open);
        ASSERT_TRUE(sup.status().pbap_open);
    }
    EXPECT_EQ(ops.removed.size(), 2u);
}

TEST(ProfileSupervisor, BusySessionKeepsPollingWithoutRetryBurst) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    // Busy is not Forbidden, so it gets no extra immediate retry.
    EXPECT_EQ(ops.map_attempts, 1);
    EXPECT_EQ(sup.status().map_error, ObexError::Busy);
}

TEST(BearerSupervisor, TransientFailuresDoNotEscalateTheBackoff) {
    for (const char* message : {"org.bluez.Error.NotReady: Resource Not Ready",
                                "org.bluez.Error.Failed: Software caused connection abort",
                                "org.bluez.Error.Failed: le-connection-abort-by-local",
                                "org.bluez.Error.Failed: br-connection-refused"}) {
        FakeBearer ops;
        ops.classic_succeeds = false;
        ops.classic_error = message;
        BearerSupervisor sup(ops, true);

        int64_t now = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            sup.tick(now);
            EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS) << message;
            now += sup.status().classic_backoff;
        }
    }
}

// A phone that walked out of the room, or has Bluetooth off, answers with a page
// timeout. Paging it every 5s forever is what wedges bluetoothd; the five-minute
// ceiling would leave the link down long after it is back on the desk.
TEST(BearerSupervisor, AbsentPhoneBacksOffToTheAbsentCeiling) {
    for (const char* message : {"org.bluez.Error.Failed: br-connection-page-timeout",
                                "org.bluez.Error.Failed: Host is down",
                                "org.bluez.Error.Failed: Connection timed out"}) {
        FakeBearer ops;
        ops.classic_succeeds = false;
        ops.classic_error = message;
        BearerSupervisor sup(ops, true);

        int64_t now = 0;
        sup.tick(now);
        const int first = sup.status().classic_backoff;
        for (int attempt = 0; attempt < 10; ++attempt) {
            now += sup.status().classic_backoff;
            sup.tick(now);
        }
        EXPECT_GT(sup.status().classic_backoff, first) << message;
        EXPECT_EQ(sup.status().classic_backoff, ABSENT_CLASSIC_BACKOFF_MAX_SECONDS) << message;
    }
}

// A refusal from the phone is a real answer, and repeating it every five seconds
// helps nobody.
TEST(BearerSupervisor, PersistentFailuresStillEscalate) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    const int first = sup.status().classic_backoff;
    sup.tick(first);
    EXPECT_GT(sup.status().classic_backoff, first);
}

// reset() is what a caller reaches for when the phone reappears. The existing
// coverage stops at a minimum backoff; the case that actually bites is a backoff
// that grew to minutes while the phone was out of range.
TEST(BearerSupervisor, ResetClearsAnEscalatedBackoff) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    now += sup.status().classic_backoff;
    sup.tick(now);
    ASSERT_GT(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);
    const int attempts = ops.classic_attempts;

    sup.reset();
    EXPECT_EQ(sup.status().classic_backoff, 0);
    sup.tick(++now);
    EXPECT_EQ(ops.classic_attempts, attempts + 1) << "reset() left the grown backoff in place";
}

// The Forbidden retry exists to drop a stale session left by a previous run.
// Removing the empty string that the failed CreateSession just wrote drops
// nothing at all.
TEST(ProfileSupervisor, ForbiddenRetryRemovesThePreviousSession) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    const std::string stale = sup.map_session();
    ASSERT_FALSE(stale.empty());

    // The session goes sour; the next attempt is refused.
    sup.drop_map();
    ops.removed.clear();
    ops.map_error = "org.bluez.obex.Error.Forbidden";
    sup.tick(PROFILE_STEADY_POLL_SECONDS * 2, true);

    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);
    EXPECT_EQ(ops.map_attempts, 3) << "the one extra retry did not happen";
}

// A MAP session can die while the Classic link stays up. Nothing else notices,
// because tick() short-circuits once map_open is set.
TEST(ProfileSupervisor, DropMapReopensWithoutTouchingPbap) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    ASSERT_TRUE(sup.status().pbap_open);
    const std::string first_map = sup.map_session();
    const std::string pbap = sup.pbap_session();

    sup.drop_map();
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open) << "contacts were dropped along with messages";
    ASSERT_EQ(ops.removed.size(), 1u);
    EXPECT_EQ(ops.removed.front(), first_map);

    // And reopens on the very next tick rather than waiting out the steady poll.
    sup.tick(1, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_NE(sup.map_session(), first_map);
    EXPECT_EQ(sup.pbap_session(), pbap) << "the surviving PBAP session was reopened needlessly";
}

// A held profile frees the iPhone's single MAP session for another computer, so
// it must be removed and stay closed until the hold lifts.
TEST(ProfileSupervisor, HeldMapIsRemovedAndNotReopened) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const std::string first_map = sup.map_session();
    const std::string pbap = sup.pbap_session();

    sup.set_held(true, false, false);
    ASSERT_EQ(ops.removed.size(), 1u);
    EXPECT_EQ(ops.removed.front(), first_map);
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.status().map_held);

    sup.tick(100, true);
    EXPECT_EQ(ops.map_attempts, 1) << "a held MAP session was reopened";
    EXPECT_EQ(sup.pbap_session(), pbap);
    EXPECT_NE(sup.status().reason.find("released"), std::string::npos);

    // Lifting the hold reopens on the next tick, without waiting out the poll.
    sup.set_held(false, false, false);
    sup.tick(101, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_EQ(ops.map_attempts, 2);
}

// Re-applying the same hold every tick must not churn sessions.
TEST(ProfileSupervisor, RepeatedHoldIsIdempotent) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    sup.set_held(true, true, true);
    sup.set_held(true, true, true);
    EXPECT_EQ(ops.removed.size(), 2u);
    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts + ops.pbap_attempts, 2);
    EXPECT_NE(sup.status().reason.find("idle"), std::string::npos);
}

// iOS routinely keeps an LE link while letting BR/EDR lapse. ANCS rides LE
// alone, so notification mirroring keeps working there — reporting LE as down
// because BR/EDR is would tear down a GATT session that is still delivering.
TEST(BearerSupervisor, LeStaysReportedWhenOnlyClassicIsDown) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().classic_connected);
    ASSERT_TRUE(sup.status().le_connected);

    // BR/EDR lapses; the LE link is untouched.
    ops.classic = false;
    ops.classic_succeeds = false;
    sup.tick(100);

    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected) << "an LE link that is still up must not be reported as down";
    EXPECT_GT(ops.classic_attempts, 1) << "BR/EDR must still be retried";
}

// An iPhone that refuses BR/EDR while unlocked with its permissions on is wedged
// on its own side. Only cycling Bluetooth on the phone clears it, so the status
// has to say that rather than claiming to still be connecting.
TEST(BearerSupervisor, RepeatedRefusalNamesTheRemedy) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.Failed: br-connection-refused";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    EXPECT_NE(sup.status().reason.find("Connecting"), std::string::npos)
        << "one refusal is not yet evidence of anything";

    while (ops.classic_attempts < CLASSIC_FAILURES_BEFORE_ADVICE) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }

    EXPECT_NE(sup.status().reason.find("Turn Bluetooth off and back on"), std::string::npos)
        << "the status never names the one thing that clears this";
    EXPECT_NE(sup.status().reason.find("Re-pairing is not the fix"), std::string::npos);

    // And it goes away the moment the phone accepts. The backoff has to elapse
    // first, or no attempt is made and the advice rightly still stands.
    ops.classic_succeeds = true;
    now += sup.status().classic_backoff;
    sup.tick(now);
    EXPECT_EQ(sup.status().reason.find("Turn Bluetooth off"), std::string::npos);
    sup.tick(++now);
    EXPECT_TRUE(sup.status().classic_connected);
}

// Walking back into range must not be held behind the backoff the absence grew.
TEST(BearerSupervisor, LeComingUpClearsTheClassicBackoff) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    // Out of range: BR/EDR fails until the backoff reaches its ceiling.
    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    ASSERT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MAX_SECONDS)
        << "expected the absence to grow the backoff to its ceiling";
    const int attempts_while_away = ops.classic_attempts;

    // Back in range: the phone sees the solicitation and opens LE inbound.
    ops.le = true;
    ops.classic_succeeds = true;
    sup.tick(now);

    EXPECT_EQ(ops.classic_attempts, attempts_while_away + 1)
        << "LE came up and BR/EDR still sat out the backoff grown while the phone was away";
    EXPECT_EQ(sup.status().classic_backoff, 0);
}

// The "phone keeps refusing" advice must not survive a phone that was merely away.
TEST(BearerSupervisor, LeComingUpClearsTheRefusalAdvice) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    int64_t now = 0;
    for (int i = 0; i < 40; ++i) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    ASSERT_NE(sup.status().reason.find("keeps refusing"), std::string::npos);

    ops.le = true;
    sup.tick(now);
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "blamed the phone for refusing after it had only been out of range";
}

// A live LE link is standing proof the phone is present, not a one-off event.
TEST(BearerSupervisor, ClassicBackoffStaysShortWhileLeIsUp) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";
    ops.le = true; // phone answered the solicitation; BR/EDR still will not page

    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
        EXPECT_LE(sup.status().classic_backoff, LE_UP_CLASSIC_BACKOFF_MAX_SECONDS)
            << "backed off for minutes against a phone that was answering on LE";
    }
}

// Telling the user to cycle Bluetooth would drop the LE link carrying notifications.
TEST(BearerSupervisor, DoesNotBlameThePhoneWhileItAnswersOnLe) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.le = true;

    int64_t now = 0;
    while (now < 1200) {
        sup.tick(now);
        now += BEARER_POLL_SECONDS;
    }
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "told the user to cycle Bluetooth while notifications were mirroring over LE";
}

// BlueZ reports a Classic link up only while some local profile is connected, so a
// host that offers none refuses Device1.Connect forever while the phone answers OBEX
// perfectly well. Sending that user to their phone wastes their time.
TEST(BearerSupervisor, NamesTheHostWhenObexIsUpAndClassicKeepsRefusing) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    ops.classic_succeeds = false;
    ops.classic_error = "br-connection-unknown";

    int64_t now = 0;
    sup.tick(now, true);
    EXPECT_EQ(sup.status().reason.find("offers the iPhone no profile"), std::string::npos)
        << "named the host on the first refusal, before a transient decline could clear";

    while (now < 1200) {
        now += BEARER_POLL_SECONDS;
        sup.tick(now, true);
    }
    EXPECT_NE(sup.status().reason.find("offers the iPhone no profile"), std::string::npos);
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos)
        << "blamed the phone while it was serving messages and contacts";
}

// A phone-initiated reconnect brings BR/EDR up without hands-free, and nothing
// in BlueZ retries it. One bearer cycle is the remedy, and it must stay one: the
// drop it causes runs back through reset(), which is where a naive flag clears
// and the cycle repeats forever.
TEST(BearerSupervisor, CyclesClassicOnceWhenHandsFreeIsAbsent) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0); // connects Classic
    sup.tick(1);
    EXPECT_EQ(ops.classic_disconnects, 0) << "cycled before the grace window elapsed";

    sup.tick(1 + HFP_ABSENT_SECONDS - 1);
    EXPECT_EQ(ops.classic_disconnects, 0);

    sup.tick(1 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 1);

    // The link the cycle dropped comes back, still with no hands-free.
    sup.reset();
    for (int64_t now = 1 + HFP_ABSENT_SECONDS; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 1) << "hands-free never arrived, so the phone must be asked only once";
}

// 20 s of waiting was most of the delay before call control came back.
TEST(BearerSupervisor, HandsFreeGraceIsEightSeconds) {
    EXPECT_EQ(HFP_ABSENT_SECONDS, 8);

    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(8);
    EXPECT_EQ(ops.classic_disconnects, 0);
    sup.tick(9);
    EXPECT_EQ(ops.classic_disconnects, 1);
}

// The window is a setting, so it can be tuned on a machine without rebuilding.
TEST(BearerSupervisor, HandsFreeGraceCanBeOverridden) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);
    sup.set_hfp_absent_seconds(20);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + 19);
    EXPECT_EQ(ops.classic_disconnects, 0);
    sup.tick(1 + 20);
    EXPECT_EQ(ops.classic_disconnects, 1);
}

TEST(BearerSupervisor, HandsFreeGraceOverrideIsParsedStrictly) {
    EXPECT_EQ(hfp_absent_seconds_from(nullptr, 8), 8);
    EXPECT_EQ(hfp_absent_seconds_from("", 8), 8);
    EXPECT_EQ(hfp_absent_seconds_from("20", 8), 20);
    EXPECT_EQ(hfp_absent_seconds_from("3", 8), 3);
    EXPECT_EQ(hfp_absent_seconds_from("2", 8), 8) << "too short to tell late from absent";
    EXPECT_EQ(hfp_absent_seconds_from("121", 8), 8);
    EXPECT_EQ(hfp_absent_seconds_from("12s", 8), 8);
    EXPECT_EQ(hfp_absent_seconds_from("-5", 8), 8);
}

// The deliberate drop used to be silent, which made it look like a fault.
TEST(BearerSupervisor, SaysSoWhenItDropsTheLinkOnPurpose) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    std::ostringstream captured;
    std::streambuf* previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(debug::log_mutex);
        previous = std::cerr.rdbuf(captured.rdbuf());
    }
    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + HFP_ABSENT_SECONDS);
    std::string log;
    {
        std::lock_guard<std::mutex> lock(debug::log_mutex);
        std::cerr.rdbuf(previous);
        log = captured.str();
    }

    ASSERT_EQ(ops.classic_disconnects, 1);
    EXPECT_NE(log.find("INFO"), std::string::npos) << log;
    EXPECT_NE(log.find("dropping the BR/EDR link on purpose"), std::string::npos) << log;
    EXPECT_NE(log.find("without hands-free"), std::string::npos) << log;
}

// Once hands-free is connected the cycle is armed again, so the next reconnect
// that comes up without it is recovered too.
TEST(BearerSupervisor, ArmsAnotherCycleAfterHandsFreeConnects) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 1);

    ops.telephony = true;
    sup.reset();
    sup.tick(100); // reconnects Classic
    sup.tick(101); // Classic up with hands-free on it: arms the next cycle

    ops.telephony = false;
    sup.tick(102);
    sup.tick(102 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 2);
}

// Cycling BR/EDR costs the OBEX sessions on top of it, so it must never happen
// for a feature the user has switched off.
TEST(BearerSupervisor, NeverCyclesClassicWhenCallsAreDisabled) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 0);
}

// When PipeWire owns hands-free, a telephony object exists on its bus and the
// link is healthy. Cycling BR/EDR would drop the OBEX sessions to recover
// something that is not missing.
TEST(BearerSupervisor, NeverCyclesClassicWhileHandsFreeIsPresent) {
    FakeBearer ops;
    ops.telephony = true;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 0);
}

namespace {

    // A phone that is bonded and reachable on neither bearer.
    BearerStatus gone_bearer() {
        BearerStatus s;
        s.device_present = true;
        s.device_paired = true;
        return s;
    }

} // namespace

TEST(AwayLock, LocksOnceTheGraceHasPassedAfterASupervisionTimeout) {
    const BearerStatus gone = gone_bearer();
    EXPECT_TRUE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 30, 0, 30));
}

TEST(AwayLock, WaitsOutTheGraceBeforeLocking) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 29, 0, 30));
}

// LE flaps on its own while BR/EDR carries on; that is not an absence.
TEST(AwayLock, NeverLocksWhileEitherBearerIsUp) {
    BearerStatus bearer = gone_bearer();
    bearer.classic_connected = true;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));

    bearer.classic_connected = false;
    bearer.le_connected = true;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));
}

// An open MAP or PBAP session reaches the phone whatever BlueZ reports.
TEST(AwayLock, NeverLocksWhileAnObexSessionIsOpen) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, true, REASON_TIMEOUT, 300, 0, 30));
}

// Only a supervision timeout means the phone left. Local covers the daemon's own
// disconnects and the adapter powering off, Remote the phone's Bluetooth being
// switched off, Suspend this machine going to sleep.
TEST(AwayLock, LocksOnNoOtherDisconnectReason) {
    const BearerStatus gone = gone_bearer();
    for (const char* reason : {"org.bluez.Reason.Local",
                               "org.bluez.Reason.Remote",
                               "org.bluez.Reason.Suspend",
                               "org.bluez.Reason.Authentication",
                               "org.bluez.Reason.Unknown",
                               ""})
        EXPECT_FALSE(should_lock_on_away(true, gone, false, reason, 300, 0, 30)) << reason;
}

// An unpaired or removed device is not a phone that walked away.
TEST(AwayLock, NeverLocksForADeviceThatIsNoLongerPaired) {
    BearerStatus bearer = gone_bearer();
    bearer.device_paired = false;
    EXPECT_FALSE(should_lock_on_away(true, bearer, false, REASON_TIMEOUT, 300, 0, 30));
}

TEST(AwayLock, NeverLocksWhenTheSettingIsOff) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(false, gone, false, REASON_TIMEOUT, 300, 0, 30));
}

// Nothing has gone down yet, so there is no absence to time.
TEST(AwayLock, NeverLocksBeforeAnythingHasGoneDown) {
    const BearerStatus gone = gone_bearer();
    EXPECT_FALSE(should_lock_on_away(true, gone, false, REASON_TIMEOUT, 300, -1, 30));
}

// No Bearer.LE1, so LE never reads up; the phone walked out past the
// grace window and came back over BR/EDR. ANCS must be tried again.
TEST(HoldAncs, ClassicReturningHoldsWhenLeIsInvisible) {
    BearerStatus bearer = gone_bearer();
    const int64_t past_grace = ANCS_BEARER_GRACE_SECONDS + 60;
    EXPECT_FALSE(should_hold_ancs(false, bearer, past_grace, 0)) << "phone still away";

    bearer.classic_connected = true;
    EXPECT_TRUE(should_hold_ancs(false, bearer, past_grace, 0));
}

// With a populated LE bearer its state is known, so classic alone does not hold.
TEST(HoldAncs, ClassicAloneDoesNotHoldWhenLeIsVisible) {
    BearerStatus bearer = gone_bearer();
    bearer.classic_connected = true;
    bearer.le_available = true;
    const int64_t past_grace = ANCS_BEARER_GRACE_SECONDS + 60;
    EXPECT_FALSE(should_hold_ancs(false, bearer, past_grace, 0));
    EXPECT_TRUE(should_hold_ancs(false, bearer, ANCS_BEARER_GRACE_SECONDS - 1, 0)) << "inside the grace window";
    EXPECT_TRUE(should_hold_ancs(true, bearer, past_grace, 0)) << "a live session holds";
}

// 16 November trial: the link obexd brought back carried no hands-free, and
// eight seconds later it was cut, taking messages and contacts with it. BlueZ
// is asked for the one profile first; the cut only follows if that does nothing.
TEST(BearerSupervisor, AsksForHandsFreeOverTheLiveLinkBeforeCycling) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    sup.tick(0); // connects Classic
    sup.tick(1);
    sup.tick(1 + HFP_REQUEST_SECONDS - 1);
    EXPECT_EQ(ops.hands_free_requests, 0) << "the phone opens hands-free itself within a few seconds of the link";

    sup.tick(1 + HFP_REQUEST_SECONDS);
    EXPECT_EQ(ops.hands_free_requests, 1);
    EXPECT_EQ(ops.classic_disconnects, 0);

    sup.tick(1 + HFP_REQUEST_SECONDS + 1);
    EXPECT_EQ(ops.hands_free_requests, 1) << "asked once per link";

    sup.tick(1 + HFP_ABSENT_SECONDS);
    EXPECT_EQ(ops.classic_disconnects, 1) << "the request did nothing, so the cycle still runs, and no later";
    EXPECT_EQ(sup.hfp_cycles(), 1);
}

TEST(BearerSupervisor, ARequestThatBringsHandsFreeSparesTheCycle) {
    FakeBearer ops;
    ops.telephony = false;
    ops.hands_free_request_works = true;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.hands_free_requests, 1);
    EXPECT_EQ(ops.classic_disconnects, 0);
    EXPECT_EQ(sup.hfp_cycles(), 0);
}

TEST(BearerSupervisor, HandsFreeRequestCanBeSwitchedOff) {
    EXPECT_TRUE(hfp_request_enabled(nullptr));
    EXPECT_TRUE(hfp_request_enabled("1"));
    EXPECT_FALSE(hfp_request_enabled("0"));

    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);
    sup.set_hfp_request_enabled(false);

    for (int64_t now = 0; now <= 1 + HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.hands_free_requests, 0);
    EXPECT_EQ(ops.classic_disconnects, 1);
}

// With no call control wanted there is nothing to ask for.
TEST(BearerSupervisor, NoHandsFreeRequestWhenCallsAreOff) {
    FakeBearer ops;
    ops.telephony = false;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now <= 4 * HFP_ABSENT_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.hands_free_requests, 0);
}

// A refusal collected while the phone was away must not be what the first
// attempt after its return reports: that attempt has to reach BlueZ.
TEST(BearerSupervisor, ForgetsAnOldRefusalWhenLeComesBack) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.refusals_forgotten, 0);

    ops.le = true;
    sup.tick(1);
    EXPECT_EQ(ops.refusals_forgotten, 1);
    sup.tick(2);
    EXPECT_EQ(ops.refusals_forgotten, 1) << "only on the transition";
}

// 16 November trial: with the phone's Bluetooth off, MAP and PBAP were tried
// every 14 s, each pair holding the supervising thread for close to 3 s.
TEST(ProfileSupervisor, SpacesAttemptsOutWhileNoLinkReachesThePhone) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true, true);
    ASSERT_TRUE(sup.status().map_open);
    ASSERT_EQ(ops.map_attempts, 1);

    // The link drops and nothing reaches the phone any more.
    ops.map_error = "Unable to find service record";
    ops.pbap_error = "Unable to find service record";
    sup.reset();

    std::vector<int64_t> attempts_at;
    int seen = ops.map_attempts;
    for (int64_t now = 100; now < 100 + 600; ++now) {
        sup.tick(now, true, false);
        if (ops.map_attempts != seen) {
            seen = ops.map_attempts;
            attempts_at.push_back(now - 100);
        }
    }
    const std::vector<int64_t> expected{15, 30, 60, 120, 240, 360, 480};
    EXPECT_EQ(attempts_at, expected) << "15 s, then doubling to a ceiling of two minutes; never right after the loss";
    EXPECT_EQ(sup.status().reason, obex_error_advice(ObexError::Unavailable, "map"));
}

TEST(ProfileSupervisor, OpensAtOnceWhenALinkIsBack) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);
    sup.tick(0, true, true);

    ops.map_error = "Unable to find service record";
    ops.pbap_error = "Unable to find service record";
    sup.reset();
    for (int64_t now = 100; now < 200; ++now)
        sup.tick(now, true, false);
    const int before = ops.map_attempts;

    // Deep inside a two-minute wait, the LE link comes back.
    ops.map_error.clear();
    ops.pbap_error.clear();
    sup.tick(200, true, true);
    EXPECT_EQ(ops.map_attempts, before + 1);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open);
}

// A phone in range with no link up yet, at the start of a run, is still tried
// at once: obexd opens its own transport.
TEST(ProfileSupervisor, FirstAttemptOfARunIsNotDelayed) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true, false);
    EXPECT_EQ(ops.map_attempts, 1);
    EXPECT_TRUE(sup.status().map_open);

    // And an open session counts as a link for the profile still missing.
    FakeProfiles partial;
    partial.pbap_error = "Forbidden";
    ProfileSupervisor other(partial);
    other.tick(0, true, false);
    const int pbap_attempts = partial.pbap_attempts;
    other.tick(PROFILE_STEADY_POLL_SECONDS, true, false);
    EXPECT_GT(partial.pbap_attempts, pbap_attempts) << "the usual cadence, not the absent one";
}

TEST(ProfileSupervisor, SpacingCanBeSwitchedOff) {
    EXPECT_TRUE(obex_absent_backoff_enabled(nullptr));
    EXPECT_TRUE(obex_absent_backoff_enabled("1"));
    EXPECT_FALSE(obex_absent_backoff_enabled("0"));
}

// --- Bond health -----------------------------------------------------------

namespace {

    BondFacts facts_of(bool paired, bool le_known, bool le_bonded, bool classic_known = false, bool classic_bonded = false) {
        BondFacts facts;
        facts.paired = paired;
        facts.le_known = le_known;
        facts.le_bonded = le_bonded;
        facts.classic_known = classic_known;
        facts.classic_bonded = classic_bonded;
        return facts;
    }

    // Drives the supervisor into the broken state the way the incident did:
    // one authentication failure every eight seconds.
    int64_t break_bond(FakeBearer& ops, BearerSupervisor& sup, int64_t now) {
        for (int i = 0; i < AUTH_FAILURES_BEFORE_BROKEN; ++i) {
            now += 8;
            ++ops.auth_failures;
            sup.tick(now);
        }
        return now;
    }

} // namespace

TEST(BondHealth, NamesAreStable) {
    EXPECT_STREQ(to_string(BondState::None), "none");
    EXPECT_STREQ(to_string(BondState::Ok), "ok");
    EXPECT_STREQ(to_string(BondState::Partial), "partial");
    EXPECT_STREQ(to_string(BondState::Broken), "broken");
    EXPECT_STREQ(BOND_DETAIL_BREDR_ONLY, "bredr_only");
    EXPECT_STREQ(BOND_DETAIL_LE_ONLY, "le_only");
    EXPECT_STREQ(BOND_DETAIL_AUTH_FAILURES, "auth_failures");
    EXPECT_STREQ(BOND_DETAIL_KEY_MISSING, "key_missing");
    EXPECT_STREQ(PAIR_STATUS_STALE_BOND, "stale_bond");
}

TEST(BondHealth, NothingPairedIsNone) {
    const BondHealth health = assess_bond(facts_of(false, true, false));
    EXPECT_EQ(health.state, BondState::None);
    EXPECT_FALSE(health.needs_repair);
    EXPECT_TRUE(bond_reusable(health));
}

TEST(BondHealth, BothHalvesIsOk) {
    const BondHealth health = assess_bond(facts_of(true, true, true, true, true));
    EXPECT_EQ(health.state, BondState::Ok);
    EXPECT_TRUE(health.detail.empty());
    EXPECT_FALSE(health.needs_repair);
}

// The bond of the incident: made outside the daemon, BR/EDR keys only.
TEST(BondHealth, BredrOnlyIsPartialAndNeedsRepair) {
    const BondHealth health = assess_bond(facts_of(true, true, false, true, true));
    EXPECT_EQ(health.state, BondState::Partial);
    EXPECT_EQ(health.detail, BOND_DETAIL_BREDR_ONLY);
    EXPECT_TRUE(health.needs_repair);
    EXPECT_FALSE(bond_reusable(health));
}

// Where the machine cannot carry LE, pairing again yields the same bond.
TEST(BondHealth, BredrOnlyIsNotRepairedWhereLeCannotWork) {
    const BondHealth health = assess_bond(facts_of(true, true, false), false);
    EXPECT_EQ(health.state, BondState::Partial);
    EXPECT_FALSE(health.needs_repair);
    EXPECT_TRUE(bond_reusable(health));
}

// Without a populated Bearer.LE1 there is nothing to judge by.
TEST(BondHealth, UnknownBearersReadAsOk) {
    const BondHealth health = assess_bond(facts_of(true, false, false));
    EXPECT_EQ(health.state, BondState::Ok);
    EXPECT_FALSE(health.needs_repair);
}

TEST(BondHealth, LiveAncsOutranksTheBearerProperties) {
    BondFacts facts = facts_of(true, true, false);
    facts.ancs_live = true;
    EXPECT_EQ(assess_bond(facts).state, BondState::Ok);
}

TEST(BondHealth, LeOnlyIsReportedButNotCondemned) {
    const BondHealth health = assess_bond(facts_of(true, true, true, true, false));
    EXPECT_EQ(health.state, BondState::Partial);
    EXPECT_EQ(health.detail, BOND_DETAIL_LE_ONLY);
    EXPECT_FALSE(health.needs_repair);
    // A marker-only Bearer.BREDR1 says nothing.
    EXPECT_EQ(assess_bond(facts_of(true, true, true, false, false)).state, BondState::Ok);
}

TEST(BondHealth, BrokenOutranksTheAssessment) {
    const BondHealth partial = assess_bond(facts_of(true, true, false));
    const BondHealth broken = combine_bond(partial, true, BOND_DETAIL_AUTH_FAILURES);
    EXPECT_EQ(broken.state, BondState::Broken);
    EXPECT_EQ(broken.detail, BOND_DETAIL_AUTH_FAILURES);
    EXPECT_TRUE(broken.needs_repair);
    // Nothing paired cannot be broken.
    EXPECT_EQ(combine_bond(BondHealth{}, true, BOND_DETAIL_AUTH_FAILURES).state, BondState::None);
}

TEST(BondHealth, JsonCarriesTheThreeKeys) {
    nlohmann::json payload;
    put_bond(payload, BondHealth{BondState::Broken, BOND_DETAIL_KEY_MISSING, true});
    EXPECT_EQ(payload["bond_state"], "broken");
    EXPECT_EQ(payload["bond_detail"], "key_missing");
    EXPECT_EQ(payload["needs_repair"], true);
    EXPECT_EQ(bond_from_json(payload), (BondHealth{BondState::Broken, BOND_DETAIL_KEY_MISSING, true}));
    EXPECT_EQ(bond_from_json(nlohmann::json(nullptr)), BondHealth{});
}

TEST(BondHealth, KeyMissingIsRecognised) {
    EXPECT_TRUE(is_key_missing("GDBus.Error:org.bluez.Error.Failed: br-connection-key-missing"));
    EXPECT_FALSE(is_key_missing("GDBus.Error:org.bluez.Error.Failed: br-connection-refused"));
}

TEST(AuthFailureTracker, BreaksAfterEnoughFailuresCloseTogether) {
    AuthFailureTracker tracker;
    for (int i = 0; i < AUTH_FAILURES_BEFORE_BROKEN - 1; ++i)
        EXPECT_FALSE(tracker.note_auth_failures(i * 8));
    EXPECT_FALSE(tracker.broken());
    EXPECT_TRUE(tracker.note_auth_failures(AUTH_FAILURES_BEFORE_BROKEN * 8)) << "the failure that breaks it says so";
    EXPECT_TRUE(tracker.broken());
    EXPECT_EQ(tracker.detail(), BOND_DETAIL_AUTH_FAILURES);
    EXPECT_FALSE(tracker.note_auth_failures(100)) << "said once";
}

TEST(AuthFailureTracker, FailuresFarApartDoNotBreak) {
    AuthFailureTracker tracker;
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(tracker.note_auth_failures(static_cast<int64_t>(i) * AUTH_FAILURE_WINDOW_SECONDS));
    EXPECT_FALSE(tracker.broken());
}

TEST(AuthFailureTracker, ABurstCountedInOneTickBreaks) {
    AuthFailureTracker tracker;
    EXPECT_TRUE(tracker.note_auth_failures(0, 40));
}

TEST(AuthFailureTracker, KeyMissingTwiceBreaks) {
    AuthFailureTracker tracker;
    EXPECT_FALSE(tracker.note_key_missing());
    EXPECT_TRUE(tracker.note_key_missing());
    EXPECT_EQ(tracker.detail(), BOND_DETAIL_KEY_MISSING);

    AuthFailureTracker other;
    EXPECT_FALSE(other.note_key_missing());
    other.note_classic_connected();
    EXPECT_FALSE(other.note_key_missing()) << "a link in between answers the first refusal";
}

TEST(AuthFailureTracker, OnlyAnLeLinkThatHoldsRecovers) {
    AuthFailureTracker tracker;
    tracker.note_auth_failures(0, AUTH_FAILURES_BEFORE_BROKEN);
    ASSERT_TRUE(tracker.broken());

    EXPECT_FALSE(tracker.observe_le(10, true));
    EXPECT_FALSE(tracker.observe_le(10 + BOND_RECOVERY_SECONDS - 1, true));
    // A flap restarts the count.
    EXPECT_FALSE(tracker.observe_le(10 + BOND_RECOVERY_SECONDS, false));
    EXPECT_FALSE(tracker.observe_le(100, true));
    EXPECT_TRUE(tracker.observe_le(100 + BOND_RECOVERY_SECONDS, true));
    EXPECT_FALSE(tracker.broken());
}

// The bond is read on the first tick, before anything has failed.
TEST(BearerSupervisor, PublishesAPartialBondFromTheFirstTick) {
    FakeBearer ops;
    ops.facts_known = true;
    ops.facts = facts_of(true, true, false, true, true);
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(sup.status().bond.state, BondState::Partial);
    EXPECT_EQ(sup.status().bond.detail, BOND_DETAIL_BREDR_ONLY);
    EXPECT_TRUE(sup.status().bond.needs_repair);
    EXPECT_FALSE(sup.status().standing_back) << "partial is a finding, not a reason to stop";
    EXPECT_NE(sup.status().reason.find("Forget This Device"), std::string::npos) << sup.status().reason;
}

TEST(BearerSupervisor, PartialBondIsNotRepairedWhereLeCannotWork) {
    FakeBearer ops;
    ops.facts_known = true;
    ops.facts = facts_of(true, true, false);
    BearerSupervisor sup(ops, false);
    sup.set_le_expected(false);

    sup.tick(0);
    EXPECT_EQ(sup.status().bond.state, BondState::Partial);
    EXPECT_FALSE(sup.status().bond.needs_repair);
    EXPECT_EQ(sup.status().reason.find("Forget This Device"), std::string::npos) << sup.status().reason;
}

TEST(BearerSupervisor, HealthyBondIsOkAndUnpairedIsNone) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    EXPECT_EQ(sup.status().bond.state, BondState::Ok);
    EXPECT_FALSE(sup.status().bond.needs_repair);

    ops.paired = false;
    sup.tick(5);
    EXPECT_EQ(sup.status().bond.state, BondState::None);
}

// The incident: an authentication failure every few seconds, for ever.
TEST(BearerSupervisor, AuthenticationLoopBreaksTheBondAndStopsTheAttempts) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);
    sup.tick(0);

    int64_t now = break_bond(ops, sup, 0);
    EXPECT_EQ(sup.status().bond.state, BondState::Broken);
    EXPECT_EQ(sup.status().bond.detail, BOND_DETAIL_AUTH_FAILURES);
    EXPECT_TRUE(sup.status().bond.needs_repair);
    EXPECT_TRUE(sup.status().standing_back);
    EXPECT_NE(sup.status().reason.find("Forget This Device"), std::string::npos) << sup.status().reason;

    // Nothing is asked of the phone for the next ten minutes, however many
    // more failures are counted.
    const int attempts = ops.classic_attempts;
    const int64_t broken_at = now;
    while (now < broken_at + BROKEN_BOND_RETRY_SECONDS - BEARER_POLL_SECONDS) {
        now += BEARER_POLL_SECONDS;
        ++ops.auth_failures;
        sup.tick(now);
        ASSERT_TRUE(sup.status().standing_back) << "at " << now;
    }
    EXPECT_EQ(ops.classic_attempts, attempts) << "kept connecting on a broken bond";
    EXPECT_EQ(ops.le_attempts, 0);
}

TEST(BearerSupervisor, BrokenBondStopsTheSolicitationAndTheProfiles) {
    FakeBearer ops;
    ops.classic = true;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    EXPECT_TRUE(should_solicit_ancs(true, sup.status(), 0, -1));
    EXPECT_TRUE(should_open_profiles(sup.status(), false));

    const int64_t now = break_bond(ops, sup, 0);
    EXPECT_FALSE(should_solicit_ancs(true, sup.status(), now, -1))
        << "the advert is what calls the phone back onto a link it cannot authenticate";
    EXPECT_FALSE(should_open_profiles(sup.status(), false));
    EXPECT_TRUE(should_open_profiles(sup.status(), true)) << "a session that is open is left alone";
}

// The cut for hands-free and the LE dial belong to the loop too.
TEST(BearerSupervisor, BrokenBondMakesNoHandsFreeCycleAndNoLeDial) {
    FakeBearer ops;
    ops.classic = true;
    ops.telephony = false;
    ops.bearer_pref = "bredr";
    BearerSupervisor sup(ops, true);
    sup.set_calls_enabled(true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);
    ASSERT_TRUE(sup.status().standing_back);

    for (int64_t now = 2; now < 200; now += BEARER_POLL_SECONDS)
        sup.tick(now);
    EXPECT_EQ(ops.classic_disconnects, 0);
    EXPECT_EQ(ops.hands_free_requests, 0);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(ops.bearer_writes.empty());
    EXPECT_TRUE(sup.status().classic_connected) << "the link is still reported as it is";
}

TEST(BearerSupervisor, BrokenBondIsTriedAgainInOneShortWindow) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);
    ASSERT_TRUE(sup.status().standing_back);
    const int attempts = ops.classic_attempts;

    // The window opens ten minutes on, with a clean slate.
    const int64_t opens = 1 + BROKEN_BOND_RETRY_SECONDS;
    sup.tick(opens);
    EXPECT_FALSE(sup.status().standing_back);
    EXPECT_EQ(ops.classic_attempts, attempts + 1);
    EXPECT_EQ(sup.status().bond.state, BondState::Broken) << "trying again is not a recovery";

    // And closes again on its own.
    sup.tick(opens + BROKEN_BOND_PROBE_SECONDS);
    EXPECT_TRUE(sup.status().standing_back);
    const int after = ops.classic_attempts;
    sup.tick(opens + BROKEN_BOND_PROBE_SECONDS + 100);
    EXPECT_EQ(ops.classic_attempts, after);

    // The next one is a full interval after that.
    sup.tick(opens + BROKEN_BOND_PROBE_SECONDS + BROKEN_BOND_RETRY_SECONDS);
    EXPECT_FALSE(sup.status().standing_back);
}

TEST(BearerSupervisor, AWindowThatMeetsAnotherRefusalClosesAtOnce) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);

    const int64_t opens = 1 + BROKEN_BOND_RETRY_SECONDS;
    sup.tick(opens);
    ASSERT_FALSE(sup.status().standing_back);
    ++ops.auth_failures;
    sup.tick(opens + 5);
    EXPECT_TRUE(sup.status().standing_back);
}

TEST(BearerSupervisor, AnLeLinkThatHoldsClearsABrokenBond) {
    FakeBearer ops;
    ops.classic = true;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);
    ASSERT_EQ(sup.status().bond.state, BondState::Broken);

    ops.le = true;
    sup.tick(10);
    EXPECT_EQ(sup.status().bond.state, BondState::Broken) << "a link that has just come up proves nothing";
    sup.tick(10 + BOND_RECOVERY_SECONDS);
    EXPECT_EQ(sup.status().bond.state, BondState::Ok);
    EXPECT_FALSE(sup.status().standing_back);
    EXPECT_FALSE(sup.status().bond.needs_repair);
}

// A phone that deleted its side answers every connect with key-missing.
TEST(BearerSupervisor, KeyMissingBreaksTheBond) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "GDBus.Error:org.bluez.Error.Failed: br-connection-key-missing";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    EXPECT_EQ(sup.status().bond.state, BondState::Ok) << "one refusal is not enough";
    for (int i = 0; i < 4 && sup.status().bond.state != BondState::Broken; ++i) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }
    EXPECT_EQ(sup.status().bond.state, BondState::Broken);
    EXPECT_EQ(sup.status().bond.detail, BOND_DETAIL_KEY_MISSING);
    EXPECT_TRUE(sup.status().standing_back);
    EXPECT_EQ(ops.classic_attempts, KEY_MISSING_BEFORE_BROKEN);

    const int attempts = ops.classic_attempts;
    sup.tick(now + 300);
    EXPECT_EQ(ops.classic_attempts, attempts);
}

// Failures counted before this supervisor was built belong to another bond.
TEST(BearerSupervisor, FailuresFromBeforeAreNotCounted) {
    FakeBearer ops;
    ops.auth_failures = 500;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    sup.tick(5);
    EXPECT_EQ(sup.status().bond.state, BondState::Ok);
}

// Removing the pairing ends the broken state: the next bond starts clean.
TEST(BearerSupervisor, RemovingThePairingClearsABrokenBond) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);
    ASSERT_EQ(sup.status().bond.state, BondState::Broken);

    ops.present = false;
    sup.tick(6);
    EXPECT_EQ(sup.status().bond.state, BondState::None);
    EXPECT_FALSE(sup.status().standing_back);

    ops.present = true;
    sup.tick(11);
    EXPECT_EQ(sup.status().bond.state, BondState::Ok);
    EXPECT_FALSE(sup.status().standing_back);
}

// A dropped link resets the backoff, never the verdict on the bond.
TEST(BearerSupervisor, ResetKeepsTheBrokenVerdict) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);
    sup.tick(0);
    ops.auth_failures = AUTH_FAILURES_BEFORE_BROKEN;
    sup.tick(1);
    sup.reset();
    sup.tick(6);
    EXPECT_EQ(sup.status().bond.state, BondState::Broken);
    EXPECT_TRUE(sup.status().standing_back);
}

// A closed bus connection is this computer's problem. Reported as "other", the
// user was left looking for a switch on the phone that was not there to find.
TEST(ObexErrors, ClosedBusIsNotBlamedOnThePhone) {
    // Verbatim from the daemon's journal, 16 November.
    EXPECT_EQ(classify_obex_error("The connection is closed"), ObexError::BusClosed);
    EXPECT_EQ(classify_obex_error("session bus unavailable: Could not connect: No such file or directory"),
              ObexError::BusClosed);
    EXPECT_STREQ(to_string(ObexError::BusClosed), "bus_closed");

    const std::string advice = obex_error_advice(ObexError::BusClosed, "map");
    EXPECT_NE(advice.find("session bus"), std::string::npos);
    EXPECT_EQ(advice.find("iPhone's Settings"), std::string::npos);
    EXPECT_EQ(advice.find("re-pair"), std::string::npos);
}

TEST(ProfileSupervisor, ForbiddenFastSecondsFromTheEnvironment) {
    EXPECT_EQ(forbidden_fast_seconds_from(nullptr, 300), 300);
    EXPECT_EQ(forbidden_fast_seconds_from("", 300), 300);
    EXPECT_EQ(forbidden_fast_seconds_from("0", 300), 0);
    EXPECT_EQ(forbidden_fast_seconds_from("60", 300), 60);
    EXPECT_EQ(forbidden_fast_seconds_from("3600", 300), 3600);
    EXPECT_EQ(forbidden_fast_seconds_from("3601", 300), 300);
    EXPECT_EQ(forbidden_fast_seconds_from("soon", 300), 300);
    EXPECT_EQ(forbidden_fast_seconds_from("-5", 300), 300);
}

// Someone who has just paired is looking at the switches: a refusal is tried
// again every two seconds, not at the next common poll.
TEST(ProfileSupervisor, RefusedProfilesAreRetriedEveryTwoSeconds) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(PROFILE_FORBIDDEN_FAST_SECONDS);

    sup.tick(0, true);
    const int map = ops.map_attempts;
    const int pbap = ops.pbap_attempts;

    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts, map);
    EXPECT_EQ(ops.pbap_attempts, pbap);

    sup.tick(PROFILE_FORBIDDEN_POLL_SECONDS, true);
    EXPECT_EQ(ops.map_attempts, map + 1) << "one attempt, not the first refusal's pair";
    EXPECT_EQ(ops.pbap_attempts, pbap + 1);

    sup.tick(PROFILE_FORBIDDEN_POLL_SECONDS + 1, true);
    EXPECT_EQ(ops.map_attempts, map + 1);

    // The switch is turned on: seen at the next close retry.
    ops.map_error.clear();
    sup.tick(2 * PROFILE_FORBIDDEN_POLL_SECONDS, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_EQ(sup.status().map_error, ObexError::None);
    EXPECT_FALSE(sup.status().pbap_open);
}

// Off unless asked for: the cadence the other tests pin down is the default.
TEST(ProfileSupervisor, CloseRetriesAreOffByDefault) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int map = ops.map_attempts;
    sup.tick(PROFILE_FORBIDDEN_POLL_SECONDS, true);
    EXPECT_EQ(ops.map_attempts, map);
}

// Without the BR/EDR link obexd would page the phone for each attempt. The
// close retries are for a refusal that comes back at once, over a link that is up.
TEST(ProfileSupervisor, CloseRetriesNeedTheClassicLink) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(PROFILE_FORBIDDEN_FAST_SECONDS);

    sup.tick(0, true, true, true);
    const int map = ops.map_attempts;

    sup.tick(2, true, true, false);
    EXPECT_EQ(ops.map_attempts, map) << "no BR/EDR link";
    sup.tick(3, true, false, true);
    EXPECT_EQ(ops.map_attempts, map) << "phone unreachable";

    sup.tick(4, true, true, true);
    EXPECT_EQ(ops.map_attempts, map + 1);
}

// Busy, a missing record, an absent phone: none of those is a switch being
// turned on, and each of them costs the phone or obexd more than a refusal.
TEST(ProfileSupervisor, CloseRetriesAreForRefusalsOnly) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(PROFILE_FORBIDDEN_FAST_SECONDS);

    sup.tick(0, true);
    const int map = ops.map_attempts;
    const int pbap = ops.pbap_attempts;

    sup.tick(2, true);
    sup.tick(4, true);
    EXPECT_EQ(ops.map_attempts, map + 2);
    EXPECT_EQ(ops.pbap_attempts, pbap) << "a busy profile stays on the common poll";
}

// An open profile is never tried again, however close the other's retries.
TEST(ProfileSupervisor, CloseRetriesLeaveAnOpenProfileAlone) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(PROFILE_FORBIDDEN_FAST_SECONDS);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    const int map = ops.map_attempts;
    const int pbap = ops.pbap_attempts;
    // The first refusal's one extra try drops what may be a stale session.
    const size_t removed = ops.removed.size();

    for (int t = 1; t <= 10; ++t)
        sup.tick(t, true);
    EXPECT_EQ(ops.map_attempts, map);
    EXPECT_EQ(ops.pbap_attempts, pbap + 5);
    EXPECT_EQ(ops.removed.size(), removed) << "a close retry must not drop a session";
}

// A switch left off for good must not be knocked on every two seconds forever.
TEST(ProfileSupervisor, CloseRetriesEndAndTheSteadyPollTakesOver) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(6);

    sup.tick(0, true);
    const int pbap = ops.pbap_attempts;

    sup.tick(2, true);
    sup.tick(4, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 2);

    for (int t = 6; t < PROFILE_STEADY_POLL_SECONDS; ++t)
        sup.tick(t, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 2) << "retried closely after the window ended";

    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 3);
}

// One switch turned on means the other is on screen: its window starts over.
TEST(ProfileSupervisor, AnOpenedProfileRestartsTheWindowOfTheOther) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(3);

    sup.tick(0, true);
    ops.map_error.clear();
    sup.tick(2, true);
    ASSERT_TRUE(sup.status().map_open);
    const int pbap = ops.pbap_attempts;

    // 4 s after the first refusal, past the 3 s window, but 2 s after MAP opened.
    sup.tick(4, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 1);
}

// A link loss must not start the window over: a phone that comes and goes all
// day with a switch off would be knocked on for five minutes each time.
TEST(ProfileSupervisor, ALinkLossDoesNotRestartTheWindow) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(6);

    sup.tick(0, true);
    sup.reset();
    ops.map_error = "Forbidden";
    sup.tick(20, true);
    const int map = ops.map_attempts;
    const int pbap = ops.pbap_attempts;

    sup.tick(22, true);
    sup.tick(24, true);
    EXPECT_EQ(ops.map_attempts, map);
    EXPECT_EQ(ops.pbap_attempts, pbap);
}

TEST(ProfileSupervisor, RetryNowTriesAtOnce) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ops.pbap_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int map = ops.map_attempts;
    sup.tick(1, true);
    ASSERT_EQ(ops.map_attempts, map);

    sup.retry_now(1);
    ops.map_error.clear();
    EXPECT_TRUE(sup.tick(1, true));
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_EQ(ops.pbap_attempts, map + 1);
}

// Asked for while the phone is away, the attempt is made all the same: the
// client asked because its user is acting, and one attempt costs little.
TEST(ProfileSupervisor, RetryNowIsNotHeldBackByAnAbsentPhone) {
    FakeProfiles ops;
    ops.map_error = "Host is down";
    ops.pbap_error = "Host is down";
    ProfileSupervisor sup(ops);

    sup.tick(0, true, false);
    const int map = ops.map_attempts;
    sup.tick(1, true, false);
    ASSERT_EQ(ops.map_attempts, map);

    sup.retry_now(1);
    sup.tick(1, true, false);
    EXPECT_EQ(ops.map_attempts, map + 1);
}

TEST(ProfileSupervisor, RetryNowRestartsTheCloseRetries) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    sup.set_forbidden_fast_seconds(3);

    sup.tick(0, true);
    const int pbap = ops.pbap_attempts;
    sup.tick(4, true);
    ASSERT_EQ(ops.pbap_attempts, pbap) << "window over, steady poll not due";

    sup.retry_now(4);
    sup.tick(4, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 1);
    sup.tick(6, true);
    EXPECT_EQ(ops.pbap_attempts, pbap + 2) << "followed closely again";
}

// The session that opened is announced before the other profile is tried:
// that attempt can wait on obexd for tens of seconds.
TEST(ProfileSupervisor, AnOpenedSessionIsAnnouncedBeforeTheOtherAttempt) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    std::vector<std::string> seen;
    sup.set_opened_handler([&] {
        seen.push_back(std::string(sup.status().map_open ? "map" : "") + (sup.status().pbap_open ? "+pbap" : "") +
                       " pbap_attempts=" + std::to_string(ops.pbap_attempts));
    });

    sup.tick(0, true);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "map pbap_attempts=0");
    EXPECT_EQ(seen[1], "map+pbap pbap_attempts=1");

    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(seen.size(), 2u) << "nothing opened, nothing announced";
}

TEST(ProfileSupervisor, ARefusedSessionIsNotAnnounced) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);
    int announced = 0;
    sup.set_opened_handler([&] { ++announced; });

    sup.tick(0, true);
    EXPECT_EQ(announced, 0);
}

// The sessions died with the bus. They are not removed through it, and the
// status says what happened instead of going on showing them open.
TEST(ProfileSupervisor, ALostBusClosesEverythingAndSaysSo) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);
    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    ASSERT_TRUE(sup.status().pbap_open);

    sup.bus_lost();
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_FALSE(sup.status().pbap_open);
    EXPECT_EQ(sup.status().map_error, ObexError::BusClosed);
    EXPECT_EQ(sup.status().pbap_error, ObexError::BusClosed);
    EXPECT_TRUE(sup.map_session().empty());
    EXPECT_TRUE(ops.removed.empty()) << "nothing can be removed over a closed bus";
    EXPECT_NE(sup.status().reason.find("session bus"), std::string::npos);

    // Every later attempt fails the same way and keeps the same cause.
    ops.map_error = "The connection is closed";
    ops.pbap_error = "The connection is closed";
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(sup.status().map_error, ObexError::BusClosed);
    EXPECT_NE(sup.status().reason.find("session bus"), std::string::npos);
}

// A profile released on purpose keeps saying so: that is the user's choice,
// and it is the other profile that reports the bus.
TEST(ProfileSupervisor, ALostBusLeavesAHeldProfileHeld) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);
    sup.tick(0, true);
    sup.set_held(true, false, false);

    sup.bus_lost();
    EXPECT_TRUE(sup.status().map_held);
    EXPECT_EQ(sup.status().map_error, ObexError::None);
    EXPECT_EQ(sup.status().pbap_error, ObexError::BusClosed);
    EXPECT_NE(sup.status().reason.find("session bus"), std::string::npos);
}
