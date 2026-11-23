#include <gtest/gtest.h>
#include <tether/bluetooth/advert.hpp>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/le_after_sleep.hpp>

#include <string>

using namespace tether::bluetooth;

namespace {

    LeExpectation expected_le() {
        LeExpectation le;
        le.ancs_enabled = true;
        le.le_available = true;
        le.phone_offers_ancs = true;
        return le;
    }

    // The iPhone of 17 November: BR/EDR comes and goes, the LE link never
    // comes, whatever is on air.
    class SilentPhone : public BearerOps {
    public:
        bool classic = false;
        bool advert_on_air = true;
        int classic_attempts = 0;
        int full_disconnects = 0;
        std::string bearer_pref = "le";

        bool device_present() const override { return true; }
        bool device_paired() const override { return true; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return true; }
        bool le_connected() const override { return false; }
        std::string preferred_bearer() const override { return bearer_pref; }
        void set_preferred_bearer(const std::string& bearer) override { bearer_pref = bearer; }
        ConnectResult connect_classic(std::string&) override {
            ++classic_attempts;
            return ConnectResult::Requested;
        }
        ConnectResult connect_le(std::string&) override { return ConnectResult::Requested; }
        bool le_connect_outstanding() const override { return false; }
        bool solicitation_on_air() const override { return advert_on_air; }
        bool telephony_present() const override { return true; }
        bool disconnect_classic(std::string&) override { return true; }
        bool disconnect_all(std::string&) override {
            ++full_disconnects;
            classic = false;
            return true;
        }
        bool phone_offers_ancs() const override { return true; }
    };

    LeStateFacts le_down(int64_t now, int64_t since) {
        LeStateFacts facts;
        facts.ancs_enabled = true;
        facts.device_paired = true;
        facts.le_available = true;
        facts.advert_registered = true;
        facts.now = now;
        facts.le_down_since = since;
        return facts;
    }

} // namespace

// --- The race of 17 November -------------------------------------------------

// 10:36:13.849 "asking BlueZ for the BR/EDR link", 10:36:13.853 "leaving
// BR/EDR alone": the PBAP session was still on the books when the link was
// seen down, so the tick took it for a link and connected.
TEST(HoldRace, NothingIsAskedOverClassicInTheTickThatSeesTheLinkDrop) {
    SilentPhone phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    phone.classic = true;
    supervisor.tick(100, true);
    supervisor.tick(101, true);
    ASSERT_TRUE(supervisor.status().classic_connected);

    // The suspend cuts the link; obexd has not cleaned its sessions up yet.
    phone.classic = false;
    supervisor.tick(102, true);
    EXPECT_EQ(phone.classic_attempts, 0);
    EXPECT_TRUE(supervisor.status().holding_for_le);

    // And for the whole hold, sessions gone or not.
    for (int64_t now = 103; now < 102 + LE_FIRST_HOLD_SECONDS; ++now) {
        supervisor.tick(now, false);
        EXPECT_EQ(phone.classic_attempts, 0) << "at " << now;
        EXPECT_TRUE(supervisor.status().holding_for_le) << "at " << now;
    }
    supervisor.tick(102 + LE_FIRST_HOLD_SECONDS, false);
    EXPECT_FALSE(supervisor.status().holding_for_le);
    EXPECT_EQ(phone.classic_attempts, 1);
}

TEST(HoldRace, TheSwitchGivesTheOldBehaviourBack) {
    SilentPhone phone;
    BearerSupervisor supervisor(phone, true);
    LeFirstConfig config;
    config.strict_hold = false;
    supervisor.set_le_first(config);

    phone.classic = true;
    supervisor.tick(100, true);
    phone.classic = false;
    supervisor.tick(101, true);
    EXPECT_EQ(phone.classic_attempts, 1);
    EXPECT_FALSE(supervisor.status().holding_for_le);
}

// A machine that offers the phone no profile: BlueZ never reads BR/EDR as up
// and the OBEX session is the only proof of a link. Nothing dropped, so the
// session still counts.
TEST(HoldRace, ASessionWithoutAClassicLinkStillCountsAsALink) {
    SilentPhone phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});
    supervisor.tick(100, true);
    supervisor.tick(101, true);
    EXPECT_FALSE(supervisor.status().holding_for_le);
    EXPECT_FALSE(session_outlived_link(false, false));
    EXPECT_TRUE(session_outlived_link(true, false));
    EXPECT_FALSE(session_outlived_link(true, true));
}

TEST(HoldRace, StrictHoldIsReadFromTheEnvironment) {
    EXPECT_TRUE(le_first_config_from(nullptr, nullptr, nullptr).strict_hold);
    EXPECT_TRUE(le_first_config_from(nullptr, nullptr, nullptr, "1").strict_hold);
    EXPECT_FALSE(le_first_config_from(nullptr, nullptr, nullptr, "0").strict_hold);
    EXPECT_FALSE(le_first_off().strict_hold);
}

// --- The young LE link ---------------------------------------------------------

// 16 November, 22:25:05, 22:25:57, 22:26:13, 22:26:57: "the LE link came
// first", BR/EDR asked for in the same millisecond, LE lost to a timeout
// within the second.
TEST(LeSettle, ClassicIsNotAskedForWhileTheLeLinkIsYoung) {
    struct Phone : SilentPhone {
        bool le = false;
        bool le_connected() const override { return le; }
        bool solicitation_on_air() const override { return !le; }
    } phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    supervisor.tick(100);
    ASSERT_TRUE(supervisor.status().holding_for_le);
    phone.le = true;
    for (int64_t now = 102; now < 102 + LE_SETTLE_SECONDS; ++now) {
        supervisor.tick(now);
        EXPECT_TRUE(supervisor.status().le_settling) << "at " << now;
        EXPECT_FALSE(should_open_profiles(supervisor.status(), false)) << "at " << now;
        EXPECT_EQ(phone.classic_attempts, 0) << "at " << now;
    }
    supervisor.tick(102 + LE_SETTLE_SECONDS);
    EXPECT_FALSE(supervisor.status().le_settling);
    EXPECT_TRUE(should_open_profiles(supervisor.status(), false));
    EXPECT_EQ(phone.classic_attempts, 1);
}

TEST(LeSettle, Decision) {
    const LeFirstConfig config;
    SettleFacts facts;
    facts.le = expected_le();
    facts.le_connected = true;
    facts.now = 10;
    facts.le_up_since = 8;
    EXPECT_TRUE(settle_le_before_classic(config, facts));
    facts.now = 8 + LE_SETTLE_SECONDS;
    EXPECT_FALSE(settle_le_before_classic(config, facts));

    facts.now = 10;
    SettleFacts other = facts;
    other.classic_connected = true; // nothing left to protect it from
    EXPECT_FALSE(settle_le_before_classic(config, other));
    other = facts;
    other.obex_up = true;
    EXPECT_FALSE(settle_le_before_classic(config, other));
    other = facts;
    other.le_connected = false;
    EXPECT_FALSE(settle_le_before_classic(config, other));
    other = facts;
    other.le.phone_offers_ancs = false; // a new bond: nothing is delayed
    EXPECT_FALSE(settle_le_before_classic(config, other));

    LeFirstConfig off = config;
    off.settle_seconds = 0;
    EXPECT_FALSE(settle_le_before_classic(off, facts));
    EXPECT_EQ(le_first_config_from(nullptr, nullptr, nullptr, nullptr, "0").settle_seconds, 0);
    EXPECT_EQ(le_first_config_from(nullptr, nullptr, nullptr, nullptr, "12").settle_seconds, 12);
    EXPECT_EQ(le_first_config_from(nullptr, nullptr, nullptr).settle_seconds, LE_SETTLE_SECONDS);
    EXPECT_EQ(le_first_off().settle_seconds, 0);
}

// --- Sleep -------------------------------------------------------------------

TEST(SleepWatch, TellsASleepFromTheGapBetweenTheTwoClocks) {
    SleepWatch watch;
    // The first reading only sets the reference, whatever the gap already is.
    EXPECT_FALSE(watch.observe(1000, 500000));
    EXPECT_FALSE(watch.observe(2000, 501000));
    EXPECT_FALSE(watch.observe(3000, 502400));
    // 2 h 9 min asleep, as between 08:26:35 and 10:36:12.
    const auto slept = watch.observe(4000, 503400 + 7777000);
    ASSERT_TRUE(slept);
    EXPECT_GE(*slept, 7777000);
    // Told once.
    EXPECT_FALSE(watch.observe(5000, 504400 + 7777000));
    // A seven-second sleep (08:08:23 to 08:08:30) is one too.
    EXPECT_TRUE(watch.observe(6000, 505400 + 7777000 + 7000));
}

TEST(SleepWatch, SwitchedOffByTheEnvironment) {
    EXPECT_TRUE(resume_watch_enabled(nullptr));
    EXPECT_TRUE(resume_watch_enabled("1"));
    EXPECT_FALSE(resume_watch_enabled("0"));
}

TEST(Resume, TheHoldAndTheRecycleStartOverAndTheAdvertIsWantedFast) {
    SilentPhone phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});

    // The engine starts with BR/EDR up and no LE: one recycle.
    phone.classic = true;
    int64_t now = 0;
    for (; phone.full_disconnects == 0 && now < 100; ++now)
        supervisor.tick(now);
    ASSERT_EQ(phone.full_disconnects, 1);
    EXPECT_EQ(supervisor.advert_rearmed_at(), now - 1);
    // The phone comes straight back over BR/EDR alone, and stays an hour.
    // No second recycle: it is spent until an LE link is seen.
    supervisor.tick(now++);
    phone.classic = true;
    for (int64_t until = now + 3600; now < until; ++now)
        supervisor.tick(now);
    ASSERT_EQ(phone.full_disconnects, 1);

    // The computer sleeps, which cuts the link, and wakes.
    phone.classic = false;
    const int attempts = phone.classic_attempts;
    supervisor.resumed(now);
    EXPECT_EQ(supervisor.advert_rearmed_at(), now);
    supervisor.tick(now++);
    EXPECT_TRUE(supervisor.status().holding_for_le);
    EXPECT_EQ(phone.classic_attempts, attempts);

    // The phone reopens BR/EDR on its own two seconds later, as it did at
    // 08:08:31 and 10:36:13. Still no LE: the links are dropped once more.
    supervisor.tick(now++);
    phone.classic = true;
    for (int64_t until = now + 2 * LE_RECYCLE_SECONDS; now < until; ++now)
        supervisor.tick(now);
    EXPECT_EQ(phone.full_disconnects, 2);
}

TEST(Resume, EveryLinkGoingDownWantsTheAdvertFast) {
    SilentPhone phone;
    BearerSupervisor supervisor(phone, true);
    supervisor.set_le_first(LeFirstConfig{});
    EXPECT_LT(supervisor.advert_rearmed_at(), 0);
    phone.classic = true;
    supervisor.tick(50);
    EXPECT_LT(supervisor.advert_rearmed_at(), 0);
    phone.classic = false;
    supervisor.tick(60);
    EXPECT_EQ(supervisor.advert_rearmed_at(), 60);
}

// The LE link had been down since the evening before: every later moment at
// which the phone might have answered was spent at 152.5 ms.
TEST(Resume, TheAdvertIsFastAgainWhateverTheAgeOfTheLeLoss) {
    const int64_t le_down_since = 0;
    const int64_t now = 12 * 3600;
    EXPECT_EQ(advert_interval_for(now, le_down_since), ADVERT_INTERVAL_STEADY);
    EXPECT_EQ(advert_interval_for(now, le_down_since, ADVERT_FAST_SECONDS, -1, now), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(now + ADVERT_REARM_FAST_SECONDS - 1, le_down_since, ADVERT_FAST_SECONDS, -1, now),
              ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(now + ADVERT_REARM_FAST_SECONDS, le_down_since, ADVERT_FAST_SECONDS, -1, now),
              ADVERT_INTERVAL_STEADY);
    // LANDLINE_ADVERT_FAST_SECONDS=0 means never fast, here too.
    EXPECT_EQ(advert_interval_for(now, le_down_since, 0, -1, now), ADVERT_INTERVAL_STEADY);
}

// --- An LE device nobody knows -----------------------------------------------

TEST(LeStranger, ReadsTheKindOfARandomAddress) {
    EXPECT_EQ(random_address_kind("4A:1B:2C:3D:4E:5F"), RandomAddressKind::Resolvable);
    EXPECT_EQ(random_address_kind("/org/bluez/hci0/dev_4A_1B_2C_3D_4E_5F"), RandomAddressKind::Resolvable);
    EXPECT_EQ(random_address_kind("7f:00:00:00:00:01"), RandomAddressKind::Resolvable);
    EXPECT_EQ(random_address_kind("D2:A4:C6:E8:0A:1C"), RandomAddressKind::Static);
    EXPECT_EQ(random_address_kind("3A:00:00:00:00:01"), RandomAddressKind::NonResolvable);
    EXPECT_EQ(random_address_kind("9A:00:00:00:00:01"), RandomAddressKind::Reserved);
    EXPECT_FALSE(random_address_kind(""));
    EXPECT_FALSE(random_address_kind("zz:00"));
}

TEST(LeStranger, TheDeviceOfTenFiftyFourIsALead) {
    StrangerFacts facts;
    facts.address = "/org/bluez/hci0/dev_4A_1B_2C_3D_4E_5F";
    facts.le = expected_le();
    facts.advert_registered = true;
    EXPECT_TRUE(may_be_unresolved_phone(facts));
    facts.address_type = "random";
    EXPECT_TRUE(may_be_unresolved_phone(facts));
}

TEST(LeStranger, NotALeadWhenItCannotBeThePhone) {
    StrangerFacts base;
    base.address = "/org/bluez/hci0/dev_4A_1B_2C_3D_4E_5F";
    base.address_type = "random";
    base.le = expected_le();
    base.advert_registered = true;
    ASSERT_TRUE(may_be_unresolved_phone(base));

    StrangerFacts facts = base;
    facts.paired = true; // a keyboard, a mouse
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.phone_le_connected = true; // the phone is already here
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.advert_registered = false; // nothing called it
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.address_type = "public";
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.address = "/org/bluez/hci0/dev_D2_A4_C6_E8_0A_1C"; // static random: not an iPhone
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.le.phone_offers_ancs = false;
    EXPECT_FALSE(may_be_unresolved_phone(facts));
    facts = base;
    facts.le.ancs_enabled = false;
    EXPECT_FALSE(may_be_unresolved_phone(facts));

    EXPECT_TRUE(le_stranger_enabled(nullptr));
    EXPECT_FALSE(le_stranger_enabled("0"));
}

// --- The published state -----------------------------------------------------

TEST(LeState, ConnectedOffAndUnavailable) {
    LeStateFacts facts = le_down(100, -1);
    facts.le_connected = true;
    EXPECT_EQ(le_state_for(facts), (LeState{"connected", "", ""}));

    facts = le_down(100, 0);
    facts.ancs_enabled = false;
    EXPECT_EQ(le_state_for(facts), (LeState{"off", "disabled", ""}));

    facts = le_down(100, 0);
    facts.device_paired = false;
    EXPECT_EQ(le_state_for(facts), (LeState{"unavailable", "not_paired", ""}));

    facts = le_down(100, 0);
    facts.le_available = false;
    EXPECT_EQ(le_state_for(facts), (LeState{"unavailable", "no_le_bond", ""}));

    facts = le_down(100, 0);
    facts.bond_unusable = true;
    EXPECT_EQ(le_state_for(facts), (LeState{"unavailable", "bond", ""}));
}

TEST(LeState, WaitingFirstThenPaused) {
    LeStateFacts facts = le_down(100, 0);
    facts.classic_connected = true;
    EXPECT_EQ(le_state_for(facts).state, "waiting");
    EXPECT_EQ(le_state_for(facts).cause, "reconnecting");
    EXPECT_TRUE(le_state_for(facts).detail.empty());

    facts.now = LE_PAUSED_AFTER_SECONDS;
    const LeState paused = le_state_for(facts);
    EXPECT_EQ(paused.state, "paused");
    EXPECT_EQ(paused.cause, "phone_silent");
    EXPECT_NE(paused.detail.find("Notifications are paused"), std::string::npos);

    // A hold is an attempt in progress, however old the loss.
    facts.classic_connected = false;
    facts.holding_for_le = true;
    EXPECT_EQ(le_state_for(facts).state, "waiting");
}

// 17 November, 10:36 onwards: BR/EDR, messages and contacts back, no LE.
TEST(LeState, PausedAfterAResumeSaysSo) {
    LeStateFacts facts = le_down(50000, 0);
    facts.classic_connected = true;
    facts.resumed_at = 49000;
    facts.le_seen_since_resume = false;
    EXPECT_EQ(le_state_for(facts).state, "paused");
    EXPECT_EQ(le_state_for(facts).cause, "after_resume");

    // An LE link seen since: the wake-up is no longer the story.
    facts.le_seen_since_resume = true;
    EXPECT_EQ(le_state_for(facts).cause, "phone_silent");
}

TEST(LeState, PausedCausesInOrder) {
    LeStateFacts facts = le_down(1000, 0);
    EXPECT_EQ(le_state_for(facts).cause, "phone_away");

    facts.obex_up = true;
    EXPECT_EQ(le_state_for(facts).cause, "phone_silent");

    facts.advert_registered = false;
    EXPECT_EQ(le_state_for(facts).cause, "advert_off_air");

    // A stranger seen lately outranks the rest, then is forgotten.
    facts.stranger_at = 900;
    EXPECT_EQ(le_state_for(facts).cause, "unresolved_address");
    facts.now = 900 + STRANGER_MEMORY_SECONDS;
    EXPECT_EQ(le_state_for(facts).cause, "advert_off_air");
}
