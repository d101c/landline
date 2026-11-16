#include <gtest/gtest.h>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/le_first.hpp>

#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    LeExpectation expected_le() {
        LeExpectation le;
        le.ancs_enabled = true;
        le.le_available = true;
        le.phone_offers_ancs = true;
        return le;
    }

    HoldFacts all_down(int64_t now, int64_t since) {
        HoldFacts facts;
        facts.le = expected_le();
        facts.now = now;
        facts.all_down_since = since;
        return facts;
    }

    // BR/EDR up for `up_for` seconds, the advert on air as long, no LE link.
    RecycleFacts classic_only(int64_t now, int64_t up_for) {
        RecycleFacts facts;
        facts.le = expected_le();
        facts.classic_connected = true;
        facts.now = now;
        facts.classic_connected_since = now - up_for;
        facts.advert_on_air_since = now - up_for;
        return facts;
    }

    // The iPhone as it behaved on 16 November: it opens the LE link a second
    // after seeing the advert, but only while no other link to it is up.
    class Phone : public BearerOps {
    public:
        bool present = true;
        bool classic = false;
        bool le = false;
        bool answers_le = true;
        bool offers_ancs = true;
        bool busy = false;
        bool advert_on_air = true;
        bool classic_pending = false;
        std::string bearer_pref = "last-seen";
        std::vector<std::string> bearer_writes;

        int classic_attempts = 0;
        int le_dials = 0;
        int full_disconnects = 0;

        // One second of the phone's own behaviour.
        void second() {
            if (answers_le && advert_on_air && !classic && !le && bearer_pref != "bredr")
                le = true;
        }

        bool device_present() const override { return present; }
        bool device_paired() const override { return true; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return true; }
        bool le_connected() const override { return le; }
        std::string preferred_bearer() const override { return bearer_pref; }
        void set_preferred_bearer(const std::string& bearer) override {
            bearer_pref = bearer;
            bearer_writes.push_back(bearer);
        }
        ConnectResult connect_classic(std::string&) override {
            ++classic_attempts;
            classic = true;
            return ConnectResult::Requested;
        }
        bool classic_connect_outstanding() const override { return classic_pending; }
        ConnectResult connect_le(std::string& err) override {
            ++le_dials;
            err = "le-connection-abort-by-local";
            return ConnectResult::Failed;
        }
        bool le_connect_outstanding() const override { return false; }
        bool solicitation_on_air() const override { return advert_on_air && !le; }
        bool telephony_present() const override { return true; }
        bool disconnect_classic(std::string&) override {
            classic = false;
            return true;
        }
        bool disconnect_all(std::string&) override {
            ++full_disconnects;
            classic = false;
            le = false;
            return true;
        }
        bool phone_offers_ancs() const override { return offers_ancs; }
        bool phone_busy() const override { return busy; }
    };

    // Runs the supervisor and the phone side by side, one second at a time,
    // and reports the second at which the LE link was first seen up, or -1.
    int64_t run(BearerSupervisor& supervisor, Phone& phone, int64_t from, int64_t to) {
        int64_t le_at = -1;
        for (int64_t now = from; now < to; ++now) {
            supervisor.tick(now);
            if (le_at < 0 && supervisor.status().le_connected)
                le_at = now;
            phone.second();
        }
        return le_at;
    }

} // namespace

TEST(LeFirstConfig, DefaultsAndSwitches) {
    const LeFirstConfig defaults = le_first_config_from(nullptr, nullptr, nullptr);
    EXPECT_EQ(defaults.hold_seconds, LE_FIRST_HOLD_SECONDS);
    EXPECT_EQ(defaults.recycle_seconds, LE_RECYCLE_SECONDS);
    EXPECT_TRUE(defaults.unpin_after_attempt);
    EXPECT_EQ(defaults.dial_after_seconds, LE_DIAL_AFTER_SECONDS);

    EXPECT_EQ(le_first_config_from("0", "20", "30"), le_first_off());
    EXPECT_EQ(le_first_config_from("1", nullptr, nullptr), defaults);

    const LeFirstConfig tuned = le_first_config_from(nullptr, "0", "45");
    EXPECT_EQ(tuned.hold_seconds, 0);
    EXPECT_EQ(tuned.recycle_seconds, 45);
    EXPECT_TRUE(tuned.unpin_after_attempt);

    // Nonsense leaves the default in place.
    EXPECT_EQ(le_first_config_from(nullptr, "soon", "-3"), defaults);
    EXPECT_EQ(le_first_config_from(nullptr, "601", "12s"), defaults);
}

TEST(LeFirstHold, HoldsWhileEverythingIsDownAndLeIsExpected) {
    const LeFirstConfig config;
    EXPECT_TRUE(hold_classic_for_le(config, all_down(100, 100)));
    EXPECT_TRUE(hold_classic_for_le(config, all_down(100 + LE_FIRST_HOLD_SECONDS - 1, 100)));
    EXPECT_FALSE(hold_classic_for_le(config, all_down(100 + LE_FIRST_HOLD_SECONDS, 100)));
}

TEST(LeFirstHold, EndsTheMomentAnyLinkIsUp) {
    const LeFirstConfig config;
    HoldFacts facts = all_down(101, 100);
    facts.le_connected = true;
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    facts = all_down(101, 100);
    facts.classic_connected = true;
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    facts = all_down(101, 100);
    facts.obex_up = true;
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    // A link is up: there is no "since everything was down".
    EXPECT_FALSE(hold_classic_for_le(config, all_down(101, -1)));
}

TEST(LeFirstHold, NeverWhereNoLeLinkIsToBeExpected) {
    const LeFirstConfig config;
    HoldFacts facts = all_down(101, 100);
    facts.le.ancs_enabled = false;
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    facts = all_down(101, 100);
    facts.le.le_available = false; // a BR/EDR-only bond
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    facts = all_down(101, 100);
    facts.le.phone_offers_ancs = false; // never offered: a fresh bond, or sharing off
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    facts = all_down(101, 100);
    facts.le.standing_back = true; // a broken bond
    EXPECT_FALSE(hold_classic_for_le(config, facts));

    EXPECT_FALSE(hold_classic_for_le(le_first_off(), all_down(101, 100)));
}

TEST(LeFirstRecycle, OnceBrEdrAndTheAdvertHaveBeenUpLongEnoughWithoutLe) {
    const LeFirstConfig config;
    EXPECT_EQ(recycle_for_le(config, classic_only(500, LE_RECYCLE_SECONDS - 1)), RecycleVerdict::TooEarly);
    EXPECT_EQ(recycle_for_le(config, classic_only(500, LE_RECYCLE_SECONDS)), RecycleVerdict::Recycle);
}

TEST(LeFirstRecycle, NeedsBothBrEdrAndTheAdvertForTheWholeTime) {
    const LeFirstConfig config;
    RecycleFacts facts = classic_only(500, 60);
    facts.advert_on_air_since = -1; // nothing on air: the phone was never asked
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::TooEarly);

    facts = classic_only(500, 60);
    facts.advert_on_air_since = 495;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::TooEarly);

    facts = classic_only(500, 60);
    facts.classic_connected_since = 495;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::TooEarly);

    facts = classic_only(500, 60);
    facts.classic_connected = false;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::ClassicDown);
}

TEST(LeFirstRecycle, NeverUnderACallOrPlayingAudioOrAConnectInFlight) {
    const LeFirstConfig config;
    RecycleFacts facts = classic_only(500, 60);
    facts.phone_busy = true;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::Busy);

    facts = classic_only(500, 60);
    facts.connect_outstanding = true;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::Busy);
}

TEST(LeFirstRecycle, OncePerOutageAndNotTwiceInTenMinutes) {
    const LeFirstConfig config;
    RecycleFacts facts = classic_only(5000, 60);
    facts.spent = true;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::Spent);

    facts = classic_only(5000, 60);
    facts.last_recycle_at = 5000 - LE_RECYCLE_MIN_INTERVAL_SECONDS + 1;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::TooSoonAfterLast);
    facts.last_recycle_at = 5000 - LE_RECYCLE_MIN_INTERVAL_SECONDS;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::Recycle);
}

// The phone had the advert to itself a moment ago and did not come: dropping
// the links again would only repeat that.
TEST(LeFirstRecycle, NotRightAfterAHoldThatBroughtNothing) {
    const LeFirstConfig config;
    RecycleFacts facts = classic_only(500, 60);
    facts.hold_expired_at = facts.classic_connected_since - 2;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::HoldJustFailed);

    // BR/EDR only came back much later: the phone was away during the hold,
    // which therefore proved nothing.
    facts.hold_expired_at = facts.classic_connected_since - LE_HOLD_PROOF_SECONDS - 1;
    EXPECT_EQ(recycle_for_le(config, facts), RecycleVerdict::Recycle);
}

TEST(LeFirstRecycle, NeverWhereNoLeLinkIsToBeExpectedOrWhenOff) {
    RecycleFacts facts = classic_only(500, 60);
    facts.le.phone_offers_ancs = false;
    EXPECT_EQ(recycle_for_le(LeFirstConfig{}, facts), RecycleVerdict::NotExpected);

    facts = classic_only(500, 60);
    facts.le_connected = true;
    EXPECT_EQ(recycle_for_le(LeFirstConfig{}, facts), RecycleVerdict::LeUp);

    EXPECT_EQ(recycle_for_le(le_first_off(), classic_only(500, 60)), RecycleVerdict::Off);
}

TEST(LeFirstDial, WaitsForTheAdvertToHaveHadItsChance) {
    const LeFirstConfig config;
    EXPECT_FALSE(le_dial_due(config, false, 500, 0));
    EXPECT_FALSE(le_dial_due(config, true, 500, -1));
    EXPECT_FALSE(le_dial_due(config, true, 500, 500 - LE_DIAL_AFTER_SECONDS + 1));
    EXPECT_TRUE(le_dial_due(config, true, 500, 500 - LE_DIAL_AFTER_SECONDS));
    // Off: as soon as BR/EDR has settled, as before.
    EXPECT_TRUE(le_dial_due(le_first_off(), true, 500, -1));
    EXPECT_FALSE(le_dial_due(le_first_off(), false, 500, -1));
}

// Case (b) of 16 November: bluetoothd restarted, every link down, the phone
// next to the computer. Before: BR/EDR was asked for at once and LE never
// came. Now nothing is asked over BR/EDR until LE is up.
TEST(LeFirstSupervisor, AfterARestartNothingIsAskedOverBrEdrUntilLeIsUp) {
    Phone phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    supervisor.tick(0);
    EXPECT_TRUE(supervisor.status().holding_for_le);
    EXPECT_EQ(phone.classic_attempts, 0);
    EXPECT_FALSE(should_open_profiles(supervisor.status(), false));
    phone.second();

    const int64_t le_at = run(supervisor, phone, 1, 20);
    EXPECT_EQ(le_at, 1);
    EXPECT_TRUE(phone.classic);
    EXPECT_EQ(phone.classic_attempts, 1);
    EXPECT_EQ(phone.full_disconnects, 0);
    EXPECT_FALSE(supervisor.status().holding_for_le);
    EXPECT_TRUE(should_open_profiles(supervisor.status(), false));
}

TEST(LeFirstSupervisor, WithoutTheMeasuresBrEdrGoesFirstAndLeNeverComes) {
    Phone phone;
    BearerSupervisor supervisor(phone, true); // off, as before
    EXPECT_EQ(run(supervisor, phone, 0, 120), -1);
    EXPECT_TRUE(phone.classic);
    EXPECT_EQ(phone.full_disconnects, 0);
}

// Cases (a) and (c): the engine restarts and finds BR/EDR already up.
TEST(LeFirstSupervisor, BrEdrAlreadyUpIsDroppedOnceAndLeComesFirst) {
    Phone phone;
    phone.classic = true;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    const int64_t le_at = run(supervisor, phone, 0, 60);
    EXPECT_EQ(phone.full_disconnects, 1);
    EXPECT_EQ(supervisor.le_recycles(), 1);
    EXPECT_EQ(supervisor.planned_drops(), 1);
    ASSERT_GE(le_at, 0);
    EXPECT_LE(le_at, LE_RECYCLE_SECONDS + 3);
    EXPECT_TRUE(phone.classic);
    EXPECT_TRUE(phone.le);
}

// A phone that never opens LE costs one hold and no drop of a working link.
TEST(LeFirstSupervisor, APhoneThatNeverAnswersIsHeldForOnceAndNotRecycled) {
    Phone phone;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    EXPECT_EQ(run(supervisor, phone, 0, 300), -1);
    EXPECT_TRUE(phone.classic);
    EXPECT_EQ(phone.classic_attempts, 1);
    EXPECT_EQ(phone.full_disconnects, 0);
}

TEST(LeFirstSupervisor, BrEdrFoundUpAndAPhoneThatNeverAnswersIsRecycledOnlyOnce) {
    Phone phone;
    phone.classic = true;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    run(supervisor, phone, 0, 2 * LE_RECYCLE_MIN_INTERVAL_SECONDS);
    EXPECT_EQ(phone.full_disconnects, 1);
    EXPECT_TRUE(phone.classic);
}

TEST(LeFirstSupervisor, NoRecycleUnderACall) {
    Phone phone;
    phone.classic = true;
    phone.busy = true;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    run(supervisor, phone, 0, 120);
    EXPECT_EQ(phone.full_disconnects, 0);

    // The call ends: the recycle goes ahead.
    phone.busy = false;
    EXPECT_GE(run(supervisor, phone, 120, 160), 0);
    EXPECT_EQ(phone.full_disconnects, 1);
}

TEST(LeFirstSupervisor, AFreshBondThatNeverOfferedNotificationsIsLeftAlone) {
    Phone phone;
    phone.offers_ancs = false;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    supervisor.tick(0);
    EXPECT_FALSE(supervisor.status().holding_for_le);
    EXPECT_EQ(phone.classic_attempts, 1);
    run(supervisor, phone, 1, 120);
    EXPECT_EQ(phone.full_disconnects, 0);
}

// Upstream measured it as an A/B: a bond preferring BR/EDR turns the inbound
// LE link away. A preference left over from an earlier session is handed
// back during the hold, or the hold could not work.
TEST(LeFirstSupervisor, APreferenceLeftOnBrEdrIsHandedBackBeforeAnythingElse) {
    Phone phone;
    phone.bearer_pref = "bredr";
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    supervisor.tick(0);
    ASSERT_EQ(phone.bearer_writes.size(), 1u);
    EXPECT_EQ(phone.bearer_writes.front(), "le");
    phone.second();
    EXPECT_EQ(run(supervisor, phone, 1, 20), 1);
}

TEST(LeFirstSupervisor, ThePreferenceIsNotTouchedWhileAConnectIsInFlight) {
    Phone phone;
    phone.bearer_pref = "bredr";
    phone.classic_pending = true;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    for (int64_t now = 0; now < 5; ++now)
        supervisor.tick(now);
    EXPECT_TRUE(phone.bearer_writes.empty());

    phone.classic_pending = false;
    supervisor.tick(5);
    ASSERT_EQ(phone.bearer_writes.size(), 1u);
    EXPECT_EQ(phone.bearer_writes.front(), "le");
}

// The outbound LE connect takes the advert off air for as long as it runs.
// It used to go out three seconds after BR/EDR came up.
TEST(LeFirstSupervisor, TheOutboundDialNoLongerComesBeforeTheAdvertHasHadItsChance) {
    Phone phone;
    phone.classic = true;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    LeFirstConfig config;
    config.recycle_seconds = 0; // only the dial is looked at here
    supervisor.set_le_first(config);

    run(supervisor, phone, 0, LE_DIAL_AFTER_SECONDS);
    EXPECT_EQ(phone.le_dials, 0);
    run(supervisor, phone, LE_DIAL_AFTER_SECONDS, LE_DIAL_AFTER_SECONDS + 5);
    EXPECT_EQ(phone.le_dials, 1);
}

TEST(LeFirstSupervisor, ARestartOfBluetoothdStartsTheHoldAndTheRecycleOver) {
    Phone phone;
    phone.classic = true;
    phone.answers_le = false;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});
    run(supervisor, phone, 0, 60);
    ASSERT_EQ(phone.full_disconnects, 1);

    // bluetoothd dies and comes back; the phone now answers.
    phone.classic = false;
    phone.le = false;
    phone.answers_le = true;
    supervisor.bluez_restarted();
    supervisor.tick(60);
    EXPECT_TRUE(supervisor.status().holding_for_le);
    phone.second();
    EXPECT_EQ(run(supervisor, phone, 61, 80), 61);
    EXPECT_TRUE(phone.classic);
}
