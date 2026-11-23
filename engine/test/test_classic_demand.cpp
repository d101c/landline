#include <gtest/gtest.h>
#include <tether/bluetooth/classic_demand.hpp>

#include <algorithm>
#include <vector>

using namespace tether::bluetooth;

namespace {

    // The iPhone and BlueZ as the machine sees them: the test moves the
    // links by hand, the way the supervisor's tick would report them.
    struct World {
        ClassicDemand demand;
        DemandFacts facts;
        std::vector<DemandAction> actions;

        World() { facts.le_connected = true; }

        DemandAction at(int64_t now) {
            facts.now = now;
            const DemandAction action = demand.step(facts);
            if (action != DemandAction::None)
                actions.push_back(action);
            return action;
        }

        // What carrying out an action does to the links, a second later.
        void obey(DemandAction action) {
            switch (action) {
            case DemandAction::OpenData:
                facts.classic_connected = true;
                facts.map_open = true;
                facts.pbap_open = true;
                break;
            case DemandAction::OpenVoice:
                facts.classic_connected = true;
                break;
            case DemandAction::RecycleForVoice:
            case DemandAction::Close:
                down();
                break;
            case DemandAction::None:
                break;
            }
        }

        void down() {
            facts.classic_connected = false;
            facts.map_open = false;
            facts.pbap_open = false;
            facts.hands_free = false;
        }

        // Steps one second at a time, obeying, until `until` (inclusive).
        void run(int64_t from, int64_t until) {
            for (int64_t now = from; now <= until; ++now)
                obey(at(now));
        }

        int count(DemandAction action) const {
            return static_cast<int>(std::count(actions.begin(), actions.end(), action));
        }

        // A data link opened for a sync at `now` and settled.
        void open_data(int64_t now) {
            ASSERT_TRUE(demand.take(Lease::Sync, now));
            obey(at(now));
            at(now + 1);
            ASSERT_EQ(demand.state(), ClassicState::Open);
        }

        // The link is up and nobody here asked.
        void phone_opens(int64_t now) {
            facts.classic_connected = true;
            at(now);
        }
    };

    bool has(const std::vector<Lease>& leases, Lease lease) {
        return std::find(leases.begin(), leases.end(), lease) != leases.end();
    }

} // namespace

TEST(ClassicDemand, NothingHappensWithoutALease) {
    World w;
    w.run(0, 600);
    EXPECT_TRUE(w.actions.empty());
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    EXPECT_STREQ(w.demand.opened_by(), "");
}

TEST(ClassicDemand, NothingOpensWithoutTheLeLink) {
    World w;
    w.facts.le_connected = false;
    EXPECT_TRUE(w.demand.take(Lease::Send, 10));
    EXPECT_TRUE(w.demand.take(Lease::Sync, 10));
    EXPECT_EQ(w.at(10), DemandAction::None);
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    // The automatic lease is given up; the user's waits for the phone.
    EXPECT_FALSE(w.demand.holds(Lease::Sync));
    EXPECT_TRUE(w.demand.holds(Lease::Send));
    w.facts.le_connected = true;
    EXPECT_EQ(w.at(15), DemandAction::OpenData);
}

TEST(ClassicDemand, ADataLeaseOpensThroughObexAlone) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Sync, 10));
    EXPECT_EQ(w.at(10), DemandAction::OpenData);
    EXPECT_EQ(w.demand.state(), ClassicState::Opening);
    EXPECT_TRUE(w.demand.want_data());
    EXPECT_STREQ(w.demand.opened_by(), "engine");
    w.obey(DemandAction::OpenData);
    EXPECT_EQ(w.at(12), DemandAction::None);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    EXPECT_EQ(w.count(DemandAction::OpenVoice), 0);
}

TEST(ClassicDemand, AVoiceLeaseOpensByARealConnection) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Call, 10));
    EXPECT_EQ(w.at(10), DemandAction::OpenVoice);
    // No OBEX session races the connection being made.
    EXPECT_FALSE(w.demand.want_data());
    w.obey(DemandAction::OpenVoice);
    w.at(11);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    // Under a call no session is opened at all (see CallsFirst).
    EXPECT_FALSE(w.demand.want_data());
    w.facts.hands_free = true;
    w.at(17);
    EXPECT_FALSE(w.demand.want_data());
}

TEST(ClassicDemand, OnALinkOpenedForTheSoundTheSessionsFollowHandsFree) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Audio, 10));
    EXPECT_EQ(w.at(10), DemandAction::OpenVoice);
    w.obey(DemandAction::OpenVoice);
    w.at(11);
    EXPECT_FALSE(w.demand.want_data());
    w.facts.hands_free = true;
    w.at(17);
    EXPECT_TRUE(w.demand.want_data());
}

TEST(ClassicDemand, AVoiceLeaseOnADataLinkAsksThenRecyclesOnce) {
    World w;
    w.open_data(10);
    ASSERT_TRUE(w.demand.take(Lease::Audio, 20));
    // First over the link as it is.
    EXPECT_EQ(w.at(20), DemandAction::OpenVoice);
    for (int64_t now = 21; now < 20 + CLASSIC_VOICE_WAIT_SECONDS; ++now)
        EXPECT_EQ(w.at(now), DemandAction::None) << now;
    // Hands-free did not come: the link is dropped and made again.
    EXPECT_EQ(w.at(20 + CLASSIC_VOICE_WAIT_SECONDS), DemandAction::RecycleForVoice);
    EXPECT_EQ(w.demand.state(), ClassicState::Opening);
    EXPECT_FALSE(w.demand.keep_data());
    w.obey(DemandAction::RecycleForVoice);
    EXPECT_EQ(w.at(29), DemandAction::OpenVoice);
    w.obey(DemandAction::OpenVoice);
    w.at(31);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    // Once: hands-free still absent, nothing more is cut.
    w.run(32, 80);
    EXPECT_EQ(w.count(DemandAction::RecycleForVoice), 1);
}

TEST(ClassicDemand, HandsFreeArrivingOverTheDataLinkSparesTheRecycle) {
    World w;
    w.open_data(10);
    ASSERT_TRUE(w.demand.take(Lease::Audio, 20));
    EXPECT_EQ(w.at(20), DemandAction::OpenVoice);
    w.facts.hands_free = true;
    w.run(21, 60);
    EXPECT_EQ(w.count(DemandAction::RecycleForVoice), 0);
}

TEST(ClassicDemand, ARingingCallRecyclesADataLinkAtOnce) {
    World w;
    w.open_data(10);
    ASSERT_TRUE(w.demand.take(Lease::Call, 20, 0, true));
    EXPECT_EQ(w.at(20), DemandAction::RecycleForVoice);
    w.obey(DemandAction::RecycleForVoice);
    EXPECT_EQ(w.at(21), DemandAction::OpenVoice);
}

TEST(ClassicDemand, ARingingCallOnAClosedLinkOpensByARealConnection) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Call, 20, 0, true));
    EXPECT_EQ(w.at(20), DemandAction::OpenVoice);
    w.obey(DemandAction::OpenVoice);
    w.run(21, 40);
    EXPECT_EQ(w.count(DemandAction::RecycleForVoice), 0);
}

TEST(ClassicDemand, EachLeaseKeepsTheLinkItsOwnTime) {
    const struct {
        Lease lease;
        int linger;
    } cases[] = {{Lease::Sync, 10},  {Lease::Arrival, 45}, {Lease::Send, 90},
                 {Lease::Reading, 20}, {Lease::Call, 15},    {Lease::Audio, 30}};
    for (const auto& c : cases) {
        World w;
        ASSERT_TRUE(w.demand.take(c.lease, 100));
        w.obey(w.at(100));
        w.at(101);
        ASSERT_EQ(w.demand.state(), ClassicState::Open) << to_string(c.lease);
        w.demand.release(c.lease, 110);
        EXPECT_EQ(w.at(110), DemandAction::None);
        EXPECT_EQ(w.demand.state(), ClassicState::Lingering) << to_string(c.lease);
        EXPECT_EQ(w.at(110 + c.linger - 1), DemandAction::None) << to_string(c.lease);
        EXPECT_EQ(w.at(110 + c.linger), DemandAction::Close) << to_string(c.lease);
        EXPECT_EQ(w.demand.state(), ClassicState::Closing);
        w.obey(DemandAction::Close);
        w.at(111 + c.linger);
        EXPECT_EQ(w.demand.state(), ClassicState::Idle);
        EXPECT_EQ(lease_linger_seconds(c.lease), c.linger);
    }
}

TEST(ClassicDemand, HourlyAndSetupLeaveNoLinger) {
    EXPECT_EQ(lease_linger_seconds(Lease::Hourly), 0);
    EXPECT_EQ(lease_linger_seconds(Lease::Setup), 0);
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Hourly, 100));
    w.obey(w.at(100));
    w.at(101);
    w.demand.release(Lease::Hourly, 105);
    EXPECT_EQ(w.at(105), DemandAction::Close);
}

TEST(ClassicDemand, ALeaseDuringTheLingerKeepsTheLink) {
    World w;
    w.open_data(100);
    w.demand.release(Lease::Sync, 105);
    w.at(105);
    ASSERT_EQ(w.demand.state(), ClassicState::Lingering);
    // The sessions stay: a reply sent now goes out at once.
    EXPECT_TRUE(w.demand.keep_data());
    ASSERT_TRUE(w.demand.take(Lease::Send, 108));
    EXPECT_EQ(w.at(108), DemandAction::None);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    EXPECT_TRUE(w.demand.want_data());
    w.demand.release(Lease::Send, 109);
    // The longest linger of what was returned wins.
    EXPECT_EQ(w.at(109 + 89), DemandAction::None);
    EXPECT_EQ(w.at(109 + 90), DemandAction::Close);
}

TEST(ClassicDemand, AutomaticOpeningsAreCappedAtTwelveAnHour) {
    World w;
    int64_t now = 0;
    for (int i = 0; i < CLASSIC_AUTO_OPENS_PER_HOUR; ++i, now += 60) {
        ASSERT_TRUE(w.demand.take(Lease::Arrival, now)) << i;
        w.obey(w.at(now));
        w.at(now + 1);
        w.demand.release(Lease::Arrival, now + 2);
        w.run(now + 2, now + 2 + 46);
        ASSERT_EQ(w.demand.state(), ClassicState::Idle) << i;
    }
    EXPECT_EQ(w.count(DemandAction::OpenData), CLASSIC_AUTO_OPENS_PER_HOUR);
    // The thirteenth is refused: the notification alone is shown.
    EXPECT_FALSE(w.demand.take(Lease::Arrival, now));
    EXPECT_TRUE(w.demand.capped());
    EXPECT_FALSE(w.demand.take(Lease::Hourly, now));
    EXPECT_EQ(w.at(now), DemandAction::None);
    // What the user asks for is never capped.
    EXPECT_TRUE(w.demand.take(Lease::Send, now + 1));
    EXPECT_EQ(w.at(now + 1), DemandAction::OpenData);
    w.obey(DemandAction::OpenData);
    w.at(now + 2);
    // And an automatic lease rides a link that is already there.
    EXPECT_TRUE(w.demand.take(Lease::Arrival, now + 3));
}

TEST(ClassicDemand, TheCapForgetsOpeningsOlderThanAnHour) {
    World w;
    int64_t now = 0;
    for (int i = 0; i < CLASSIC_AUTO_OPENS_PER_HOUR; ++i, now += 60) {
        ASSERT_TRUE(w.demand.take(Lease::Hourly, now));
        w.obey(w.at(now));
        w.at(now + 1);
        w.demand.release(Lease::Hourly, now + 2);
        w.run(now + 2, now + 4);
    }
    EXPECT_FALSE(w.demand.take(Lease::Sync, now));
    w.at(3600);
    EXPECT_TRUE(w.demand.take(Lease::Sync, 3600));
    EXPECT_EQ(w.at(3600), DemandAction::OpenData);
    EXPECT_FALSE(w.demand.capped());
}

TEST(ClassicDemand, ADataLinkIsClosedAfterFiveMinutesWhateverFollows) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Reading, 100, 3600));
    w.obey(w.at(100));
    w.at(101);
    w.run(102, 101 + CLASSIC_DATA_MAX_SECONDS - 1);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    EXPECT_EQ(w.at(101 + CLASSIC_DATA_MAX_SECONDS), DemandAction::Close);
    EXPECT_FALSE(w.demand.holds(Lease::Reading));
}

TEST(ClassicDemand, AVoiceLinkIsNotSubjectToTheFiveMinutes) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Audio, 100));
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.run(101, 100 + 3 * CLASSIC_DATA_MAX_SECONDS);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
}

TEST(ClassicDemand, AFailedOpeningBacksOffThirtySecondsTwoMinutesTenMinutesThenAnHour) {
    World w;
    const int waits[] = {30, 120, 600, 3600, 3600};
    int64_t now = 100;
    for (const int wait : waits) {
        ASSERT_TRUE(w.demand.take(Lease::Hourly, now)) << wait;
        EXPECT_EQ(w.at(now), DemandAction::OpenData);
        // The phone never answers.
        for (int64_t t = now + 1; t < now + CLASSIC_OPEN_TIMEOUT_SECONDS; ++t)
            EXPECT_EQ(w.at(t), DemandAction::None);
        const int64_t failed_at = now + CLASSIC_OPEN_TIMEOUT_SECONDS;
        EXPECT_EQ(w.at(failed_at), DemandAction::Close);
        EXPECT_EQ(w.demand.state(), ClassicState::Failed);
        EXPECT_FALSE(w.demand.holds(Lease::Hourly));
        EXPECT_EQ(w.demand.retry_at(), failed_at + wait);
        // No automatic lease during the wait.
        EXPECT_FALSE(w.demand.take(Lease::Arrival, failed_at + wait - 1));
        w.at(failed_at + wait - 1);
        EXPECT_EQ(w.demand.state(), ClassicState::Failed);
        w.at(failed_at + wait);
        EXPECT_EQ(w.demand.state(), ClassicState::Idle);
        now = failed_at + wait + 4000;
    }
}

TEST(ClassicDemand, ASuccessfulOpeningClearsTheBackoff) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Sync, 100));
    w.at(100);
    w.at(100 + CLASSIC_OPEN_TIMEOUT_SECONDS);
    ASSERT_EQ(w.demand.state(), ClassicState::Failed);
    w.at(200);
    ASSERT_TRUE(w.demand.take(Lease::Sync, 200));
    w.obey(w.at(200));
    w.at(201);
    ASSERT_EQ(w.demand.state(), ClassicState::Open);
    w.demand.release(Lease::Sync, 202);
    w.run(202, 220);
    ASSERT_TRUE(w.demand.take(Lease::Sync, 700));
    w.at(700);
    w.at(700 + CLASSIC_OPEN_TIMEOUT_SECONDS);
    EXPECT_EQ(w.demand.retry_at(), 700 + CLASSIC_OPEN_TIMEOUT_SECONDS + 30);
}

TEST(ClassicDemand, AGestureDuringTheWaitRetriesAtOnce) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Sync, 100));
    w.at(100);
    w.at(120);
    ASSERT_EQ(w.demand.state(), ClassicState::Failed);
    ASSERT_TRUE(w.demand.take(Lease::Send, 125));
    EXPECT_EQ(w.at(125), DemandAction::OpenData);
}

TEST(ClassicDemand, ProfilesRefusedFailTheOpeningWithoutWaitingTwentySeconds) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Sync, 100));
    w.at(100);
    w.facts.data_refused = true;
    EXPECT_EQ(w.at(103), DemandAction::Close);
    EXPECT_EQ(w.demand.state(), ClassicState::Failed);
}

TEST(ClassicDemand, ALinkLostUnderALeaseIsNotReopenedInALoop) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Reading, 100));
    w.obey(w.at(100));
    w.at(101);
    w.down();
    EXPECT_EQ(w.at(110), DemandAction::None);
    EXPECT_EQ(w.demand.state(), ClassicState::Failed);
    w.run(111, 139);
    EXPECT_EQ(w.count(DemandAction::OpenData), 1);
    // The lease is still held: tried again when the wait is over.
    EXPECT_EQ(w.at(140), DemandAction::OpenData);
}

TEST(ClassicDemand, BeforeSleepEveryLeaseButTheCallIsReturnedAndTheLinkClosed) {
    World w;
    w.open_data(100);
    ASSERT_TRUE(w.demand.take(Lease::Reading, 101));
    w.at(102);
    w.facts.suspended_soon = true;
    EXPECT_EQ(w.at(103), DemandAction::Close);
    EXPECT_TRUE(w.demand.leases().empty());
    w.obey(DemandAction::Close);
    w.at(104);
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    // And nothing opens while the computer is going down.
    w.demand.take(Lease::Sync, 105);
    EXPECT_EQ(w.at(105), DemandAction::None);
}

TEST(ClassicDemand, ACallInProgressKeepsTheLinkThroughTheSleepAnnouncement) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Call, 100));
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.facts.call_active = true;
    w.at(101);
    w.facts.suspended_soon = true;
    w.run(102, 110);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
}

TEST(ClassicDemand, LeLostDuringADataLeaseClosesAsSoonAsTheLeaseIsReturned) {
    World w;
    w.open_data(100);
    w.facts.le_connected = false;
    // The operation in progress finishes.
    EXPECT_EQ(w.at(102), DemandAction::None);
    w.demand.release(Lease::Sync, 103);
    // No linger: BR/EDR up is what keeps the LE link from coming back.
    EXPECT_EQ(w.at(103), DemandAction::Close);
}

TEST(ClassicDemand, LeLostDuringACallKeepsTheLink) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Call, 100));
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.facts.call_active = true;
    w.facts.le_connected = false;
    w.run(101, 400);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
}

TEST(ClassicDemand, AClientLeaseLapsesWithoutRenewal) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Setup, 100));
    w.obey(w.at(100));
    w.at(101);
    w.at(100 + CLIENT_LEASE_SECONDS - 1);
    EXPECT_TRUE(w.demand.holds(Lease::Setup));
    // Renewed: five more minutes.
    ASSERT_TRUE(w.demand.take(Lease::Setup, 100 + CLIENT_LEASE_SECONDS - 1));
    w.at(100 + CLIENT_LEASE_SECONDS + 100);
    EXPECT_TRUE(w.demand.holds(Lease::Setup));
    EXPECT_EQ(w.at(100 + 2 * CLIENT_LEASE_SECONDS - 1), DemandAction::Close);
    EXPECT_FALSE(w.demand.holds(Lease::Setup));
}

TEST(ClassicDemand, ALinkTheIPhoneOpensIsAdoptedUnderThePhoneLease) {
    World w;
    w.phone_opens(100);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    EXPECT_STREQ(w.demand.opened_by(), "phone");
    EXPECT_TRUE(has(w.demand.leases(), Lease::Phone));
    EXPECT_TRUE(w.actions.empty());
    // Nothing is opened on it that nobody asked for.
    EXPECT_FALSE(w.demand.want_data());
    // A sync that is due uses it.
    ASSERT_TRUE(w.demand.take(Lease::Sync, 101));
    w.at(101);
    EXPECT_TRUE(w.demand.want_data());
    EXPECT_EQ(w.count(DemandAction::OpenData), 0);
}

TEST(ClassicDemand, AnIdlePhoneLinkIsClosedAfterTwoMinutes) {
    World w;
    w.phone_opens(100);
    for (int64_t now = 101; now < 100 + PHONE_LINK_IDLE_SECONDS; ++now)
        EXPECT_EQ(w.at(now), DemandAction::None) << now;
    EXPECT_EQ(w.at(100 + PHONE_LINK_IDLE_SECONDS), DemandAction::Close);
    w.obey(DemandAction::Close);
    w.at(101 + PHONE_LINK_IDLE_SECONDS);
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    EXPECT_STREQ(w.demand.opened_by(), "");
}

TEST(ClassicDemand, APhoneLinkIsNeverClosedUnderACallOrItsSound) {
    for (const bool call : {true, false}) {
        World w;
        w.phone_opens(100);
        w.facts.call_active = call;
        w.facts.phone_audio_streaming = !call;
        w.run(101, 1000);
        EXPECT_EQ(w.count(DemandAction::Close), 0);
        // The two minutes start when it falls silent.
        w.facts.call_active = false;
        w.facts.phone_audio_streaming = false;
        EXPECT_EQ(w.at(1001), DemandAction::None);
        EXPECT_EQ(w.at(1001 + PHONE_LINK_IDLE_SECONDS - 1), DemandAction::None);
        EXPECT_EQ(w.at(1001 + PHONE_LINK_IDLE_SECONDS), DemandAction::Close);
    }
}

TEST(ClassicDemand, ALinkSomeoneElseUsesIsNeverClosed) {
    World w;
    w.phone_opens(100);
    w.facts.held_by_other = true;
    w.run(101, 5000);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
}

// Tethering (NetworkManager's Bluetooth PAN) started on a link the engine
// was closing: the close BlueZ has not carried out yet is not asked again,
// which would cut the computer's Internet.
TEST(ClassicDemand, TetheringStartedDuringACloseCancelsTheClose) {
    World w;
    w.open_data(100);
    w.demand.release(Lease::Sync, 101);
    w.at(101);
    ASSERT_EQ(w.at(111), DemandAction::Close);
    // The link stays: NetworkManager took it.
    w.facts.held_by_other = true;
    w.run(112, 111 + 10 * CLASSIC_CLOSE_RETRY_SECONDS);
    EXPECT_EQ(w.count(DemandAction::Close), 1);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    // Tethering over: closed like a link the iPhone opened, once idle.
    w.facts.held_by_other = false;
    const int64_t over = 112 + 10 * CLASSIC_CLOSE_RETRY_SECONDS;
    EXPECT_EQ(w.at(over), DemandAction::None);
    EXPECT_EQ(w.at(over + PHONE_LINK_IDLE_SECONDS), DemandAction::Close);
}

// An opening that fails on a link tethering rides is given up on without
// taking the link down.
TEST(ClassicDemand, AFailedOpeningLeavesALinkTetheringUses) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Reading, 100, CLIENT_LEASE_SECONDS));
    ASSERT_EQ(w.at(100), DemandAction::OpenData);
    // Meanwhile NetworkManager brings the link up for tethering, and the
    // iPhone refuses the sessions.
    w.facts.classic_connected = true;
    w.facts.held_by_other = true;
    w.facts.data_refused = true;
    EXPECT_EQ(w.at(101), DemandAction::None);
    EXPECT_EQ(w.demand.state(), ClassicState::Failed);
    w.run(102, 2000);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
}

TEST(ClassicDemand, TwoQuickReopeningsByTheIPhoneHandItTheLinkForHalfAnHour) {
    World w;
    int64_t now = 100;
    w.phone_opens(now);
    now += PHONE_LINK_IDLE_SECONDS;
    ASSERT_EQ(w.at(now), DemandAction::Close);
    w.obey(DemandAction::Close);
    w.at(++now);
    EXPECT_FALSE(w.demand.phone_keeps_link());

    // Back ten seconds after the close: counted once.
    now += 10;
    w.phone_opens(now);
    EXPECT_FALSE(w.demand.phone_keeps_link());
    now += PHONE_LINK_IDLE_SECONDS;
    ASSERT_EQ(w.at(now), DemandAction::Close);
    w.obey(DemandAction::Close);
    w.at(++now);

    // And again: it wants the link.
    now += 10;
    w.phone_opens(now);
    EXPECT_TRUE(w.demand.phone_keeps_link());
    const int64_t kept_from = now;
    w.run(now + 1, kept_from + PHONE_KEEPS_LINK_SECONDS - 1);
    EXPECT_EQ(w.count(DemandAction::Close), 2);
    EXPECT_TRUE(w.demand.phone_keeps_link(kept_from + PHONE_KEEPS_LINK_SECONDS - 1));
    // Half an hour later the guard lifts and a new attempt is made.
    EXPECT_FALSE(w.demand.phone_keeps_link(kept_from + PHONE_KEEPS_LINK_SECONDS));
    w.run(kept_from + PHONE_KEEPS_LINK_SECONDS, kept_from + PHONE_KEEPS_LINK_SECONDS + PHONE_LINK_IDLE_SECONDS + 1);
    EXPECT_EQ(w.count(DemandAction::Close), 3);
}

TEST(ClassicDemand, AReopeningLongAfterTheCloseIsNotCounted) {
    World w;
    int64_t now = 100;
    for (int i = 0; i < 3; ++i) {
        w.phone_opens(now);
        now += PHONE_LINK_IDLE_SECONDS;
        ASSERT_EQ(w.at(now), DemandAction::Close) << i;
        w.obey(DemandAction::Close);
        w.at(++now);
        now += PHONE_REOPEN_WINDOW_SECONDS;
    }
    EXPECT_FALSE(w.demand.phone_keeps_link());
}

TEST(ClassicDemand, TwoQuickReopeningsMoreThanFifteenMinutesApartAreNotALoop) {
    World w;
    int64_t now = 100;
    w.phone_opens(now);
    now += PHONE_LINK_IDLE_SECONDS;
    w.obey(w.at(now));
    w.at(++now);
    now += 5;
    w.phone_opens(now);
    now += PHONE_LINK_IDLE_SECONDS;
    w.obey(w.at(now));
    w.at(++now);
    // A third opening, quick again, but the first is out of the window.
    now += PHONE_REOPEN_PERIOD_SECONDS;
    w.phone_opens(now);
    now += PHONE_LINK_IDLE_SECONDS;
    ASSERT_EQ(w.at(now), DemandAction::Close);
    w.obey(DemandAction::Close);
    w.at(++now);
    now += 5;
    w.phone_opens(now);
    EXPECT_FALSE(w.demand.phone_keeps_link());
}

TEST(ClassicDemand, APhoneLinkIsClosedAtMostFourTimesAnHour) {
    World w;
    int64_t now = 100;
    for (int i = 0; i < PHONE_CLOSES_PER_HOUR; ++i) {
        w.phone_opens(now);
        now += PHONE_LINK_IDLE_SECONDS;
        ASSERT_EQ(w.at(now), DemandAction::Close) << i;
        w.obey(DemandAction::Close);
        w.at(++now);
        // Slow enough not to trip the loop guard.
        now += PHONE_REOPEN_WINDOW_SECONDS + 1;
    }
    ASSERT_FALSE(w.demand.phone_keeps_link());
    w.phone_opens(now);
    w.run(now + 1, now + 600);
    EXPECT_EQ(w.count(DemandAction::Close), PHONE_CLOSES_PER_HOUR);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    // The hour over, it is closed.
    w.run(now + 601, 100 + 3600 + PHONE_LINK_IDLE_SECONDS + 5);
    EXPECT_EQ(w.count(DemandAction::Close), PHONE_CLOSES_PER_HOUR + 1);
}

TEST(ClassicDemand, ALinkClosedHereAndReopenedForALeaseIsNotHeldAgainstTheIPhone) {
    World w;
    for (int64_t now = 100; now < 2000; now += 100) {
        ASSERT_TRUE(w.demand.take(Lease::Send, now));
        w.obey(w.at(now));
        w.at(now + 1);
        w.demand.release(Lease::Send, now + 2);
        w.demand.release(Lease::Send, now + 2);
        w.at(now + 2);
        w.obey(w.at(now + 92));
        w.at(now + 93);
        ASSERT_EQ(w.demand.state(), ClassicState::Idle);
    }
    EXPECT_FALSE(w.demand.phone_keeps_link());
}

TEST(ClassicDemand, ACloseBlueZDidNotCarryOutIsAskedAgain) {
    World w;
    w.open_data(100);
    w.demand.release(Lease::Sync, 101);
    w.at(101);
    ASSERT_EQ(w.at(111), DemandAction::Close);
    // The link is still there.
    EXPECT_EQ(w.at(112), DemandAction::None);
    EXPECT_EQ(w.at(111 + CLASSIC_CLOSE_RETRY_SECONDS), DemandAction::Close);
}

TEST(ClassicDemand, ALeaseTakenWhileClosingIsServedAfterTheClose) {
    World w;
    w.open_data(100);
    w.demand.release(Lease::Sync, 101);
    w.at(101);
    ASSERT_EQ(w.at(111), DemandAction::Close);
    ASSERT_TRUE(w.demand.take(Lease::Send, 111));
    w.obey(DemandAction::Close);
    EXPECT_EQ(w.at(112), DemandAction::None);
    EXPECT_EQ(w.at(113), DemandAction::OpenData);
}

TEST(ClassicDemand, ALinkTheFullModeLeftIsUsedThenClosedWithoutBlamingTheIPhone) {
    World w;
    w.facts.classic_connected = true;
    w.demand.adopt_engine_link(100);
    EXPECT_STREQ(w.demand.opened_by(), "engine");
    EXPECT_EQ(w.at(100), DemandAction::Close);
    w.obey(DemandAction::Close);
    w.at(101);
    w.phone_opens(300);
    EXPECT_FALSE(w.demand.phone_keeps_link());
}

// --- #38, #43: "Play iPhone audio on this computer" in the lean mode -------

TEST(ClassicDemand, TheAudioLeaseFollowsTheAskAndTheSoundNotTheChoiceAlone) {
    // Off: returned if held, nothing otherwise.
    EXPECT_EQ(plan_audio_lease(false, false, false, true).move, AudioLeaseMove::Release);
    EXPECT_EQ(plan_audio_lease(false, true, true, false).move, AudioLeaseMove::None);
    // On, nobody asking, nothing playing: the choice alone opens nothing.
    EXPECT_EQ(plan_audio_lease(true, false, false, false).move, AudioLeaseMove::None);
    EXPECT_EQ(plan_audio_lease(true, false, false, true).move, AudioLeaseMove::None);
    // Asked: offered for a while.
    const AudioLeasePlan asked = plan_audio_lease(true, true, false, false);
    EXPECT_EQ(asked.move, AudioLeaseMove::Take);
    EXPECT_EQ(asked.ttl_seconds, PHONE_AUDIO_OFFER_SECONDS);
    // Playing: renewed, the silence counted from the last pass that played.
    const AudioLeasePlan playing = plan_audio_lease(true, false, true, true);
    EXPECT_EQ(playing.move, AudioLeaseMove::Take);
    EXPECT_EQ(playing.ttl_seconds, PHONE_AUDIO_SILENCE_SECONDS);
}

namespace {
    // The supervisor's pass for the Audio lease (connection.cpp), on a World.
    void audio_pass(World& w, int64_t now, bool wanted, bool asked) {
        const AudioLeasePlan plan =
            plan_audio_lease(wanted, asked, w.facts.phone_audio_streaming, w.demand.holds(Lease::Audio));
        if (plan.move == AudioLeaseMove::Take)
            w.demand.take(Lease::Audio, now, plan.ttl_seconds);
        else if (plan.move == AudioLeaseMove::Release)
            w.demand.release(Lease::Audio, now);
    }

    void run_audio(World& w, int64_t from, int64_t until, bool wanted) {
        for (int64_t now = from; now <= until; ++now) {
            audio_pass(w, now, wanted, false);
            w.obey(w.at(now));
        }
    }
} // namespace

// 18 November, 17:01 to 18:26: the sound set "here" kept the classic link up
// for over an hour with nothing playing. Turned on now, the link is offered
// for two minutes, then let go: lease lapsed, lingering, closed.
TEST(ClassicDemand, TurnedOnTheLinkIsOfferedThenLetGoWhenNothingPlays) {
    World w;
    audio_pass(w, 100, true, true);
    EXPECT_EQ(w.at(100), DemandAction::OpenVoice);
    w.obey(DemandAction::OpenVoice);
    w.facts.hands_free = true;
    run_audio(w, 101, 100 + PHONE_AUDIO_OFFER_SECONDS - 1, true);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    run_audio(w, 100 + PHONE_AUDIO_OFFER_SECONDS, 100 + PHONE_AUDIO_OFFER_SECONDS + lease_linger_seconds(Lease::Audio) + 2,
              true);
    EXPECT_EQ(w.count(DemandAction::Close), 1);
    EXPECT_FALSE(w.demand.holds(Lease::Audio));
    // An hour later, still on, nothing asked, nothing playing: no link.
    run_audio(w, 300, 3900, true);
    EXPECT_EQ(w.count(DemandAction::OpenVoice), 1);
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
}

// The sound plays: the link stays as long, and a minute of silence after (a
// pause between two songs does not lose the link; a real stop does, quickly).
TEST(ClassicDemand, WhileTheSoundPlaysTheLinkStaysThenAMinuteOfSilence) {
    World w;
    audio_pass(w, 100, true, true);
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.facts.phone_audio_streaming = true;
    run_audio(w, 101, 4000, true);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    EXPECT_EQ(w.demand.state(), ClassicState::Open);
    w.facts.phone_audio_streaming = false;
    run_audio(w, 4001, 4000 + PHONE_AUDIO_SILENCE_SECONDS - 1, true);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    run_audio(w, 4000 + PHONE_AUDIO_SILENCE_SECONDS, 4000 + PHONE_AUDIO_SILENCE_SECONDS + 40, true);
    EXPECT_EQ(w.count(DemandAction::Close), 1);
}

// The iPhone opens the link itself and plays: the lease is taken for the
// silence after, then the link goes as any other.
TEST(ClassicDemand, SoundTheIPhoneSendsOnItsOwnLinkIsKeptThenLetGo) {
    World w;
    w.phone_opens(100);
    w.facts.phone_audio_streaming = true;
    run_audio(w, 101, 1000, true);
    EXPECT_TRUE(w.demand.holds(Lease::Audio));
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    w.facts.phone_audio_streaming = false;
    run_audio(w, 1001, 1000 + PHONE_AUDIO_SILENCE_SECONDS + PHONE_LINK_IDLE_SECONDS + 40, true);
    EXPECT_EQ(w.count(DemandAction::Close), 1);
}

// Turned off: returned at once, the link lingers its 30 s and closes.
TEST(ClassicDemand, TurnedOffTheAudioLeaseIsReturned) {
    World w;
    audio_pass(w, 100, true, true);
    w.obey(w.at(100));
    w.facts.hands_free = true;
    run_audio(w, 101, 110, true);
    run_audio(w, 111, 111 + lease_linger_seconds(Lease::Audio) + 2, false);
    EXPECT_FALSE(w.demand.holds(Lease::Audio));
    EXPECT_EQ(w.count(DemandAction::Close), 1);
}

// 18 November, 18:18: the LE link drops (supervision timeout), the classic
// link stays up and carries the notifications, and the iPhone never answers
// the advertisement again while it is up. Now: 30 s without LE, the classic
// link is closed once; its lease waits for the LE link, then opens it again.
TEST(ClassicDemand, TheLeLinkGoneUnderAnOpenClassicLinkCyclesItOnce) {
    World w;
    // A lease that outlasts the drop (before #43 the Audio lease never ended).
    ASSERT_TRUE(w.demand.take(Lease::Audio, 100, 7200));
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.run(101, 200);
    ASSERT_EQ(w.demand.state(), ClassicState::Open);
    w.facts.le_connected = false;     // 18:18:03
    w.run(201, 201 + CLASSIC_LE_CYCLE_SECONDS - 1);
    EXPECT_EQ(w.count(DemandAction::Close), 0) << "a short LE drop is left alone";
    EXPECT_EQ(w.at(201 + CLASSIC_LE_CYCLE_SECONDS), DemandAction::Close);
    EXPECT_TRUE(w.demand.cycled_for_le(201 + CLASSIC_LE_CYCLE_SECONDS));
    EXPECT_TRUE(w.demand.holds(Lease::Audio)) << "the lease is kept";
    w.obey(DemandAction::Close);
    w.run(202 + CLASSIC_LE_CYCLE_SECONDS, 300);
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    EXPECT_EQ(w.count(DemandAction::OpenVoice), 1) << "nothing opened without LE";
    // The iPhone answers the advertisement: LE back, the link for the sound follows.
    w.facts.le_connected = true;
    EXPECT_EQ(w.at(301), DemandAction::OpenVoice);
}

TEST(ClassicDemand, TheLeCycleIsHeldUnderACallTheSoundOrTethering) {
    for (int what = 0; what < 3; ++what) {
        World w;
        w.open_data(100);
        w.demand.take(Lease::Send, 101);
        w.facts.le_connected = false;
        w.facts.call_active = what == 0;
        w.facts.phone_audio_streaming = what == 1;
        w.facts.held_by_other = what == 2;
        w.run(102, 102 + 3 * CLASSIC_LE_CYCLE_SECONDS);
        EXPECT_EQ(w.count(DemandAction::Close), 0) << what;
        // It stops: the cycle follows.
        w.facts.call_active = w.facts.phone_audio_streaming = w.facts.held_by_other = false;
        EXPECT_EQ(w.at(103 + 3 * CLASSIC_LE_CYCLE_SECONDS), DemandAction::Close) << what;
    }
}

TEST(ClassicDemand, TheLeCycleIsNotRepeatedWithinTenMinutes) {
    World w;
    w.phone_opens(100);
    w.demand.take(Lease::Audio, 100, 7200);
    w.facts.le_connected = false;
    const int64_t first = 101 + CLASSIC_LE_CYCLE_SECONDS;
    w.run(101, first);
    ASSERT_EQ(w.count(DemandAction::Close), 1);
    // The iPhone opens BR/EDR again at once, still no LE.
    w.run(first + 1, first + 10);
    w.phone_opens(first + 11);
    w.run(first + 12, first + CLASSIC_LE_CYCLE_INTERVAL_SECONDS - 1);
    EXPECT_EQ(w.count(DemandAction::Close), 1);
    EXPECT_EQ(w.at(first + CLASSIC_LE_CYCLE_INTERVAL_SECONDS), DemandAction::Close);
}

// A lease held while the iPhone is away (out of range: both links fall
// together) opens the link when it is back, no cycle involved.
TEST(ClassicDemand, ALeaseHeldWhileTheIPhoneIsAwayOpensTheLinkWhenItComesBack) {
    World w;
    ASSERT_TRUE(w.demand.take(Lease::Audio, 100, 7200));
    w.obey(w.at(100));
    w.facts.hands_free = true;
    w.run(101, 200);
    w.down();
    w.facts.le_connected = false;
    w.run(201, 3800);
    EXPECT_EQ(w.count(DemandAction::Close), 0);
    EXPECT_TRUE(w.demand.holds(Lease::Audio));
    w.facts.le_connected = true;
    EXPECT_EQ(w.at(3801), DemandAction::OpenVoice);
}

TEST(ClassicDemand, ResetForgetsTheStateAndKeepsTheLeases) {
    World w;
    w.open_data(100);
    w.demand.reset();
    EXPECT_EQ(w.demand.state(), ClassicState::Idle);
    // Whoever holds a lease will return it, whatever the mode did meanwhile.
    EXPECT_TRUE(w.demand.holds(Lease::Sync));
}

TEST(ClassicDemand, NamesAreThoseOfTheSocket) {
    EXPECT_STREQ(to_string(Lease::Sync), "sync");
    EXPECT_STREQ(to_string(Lease::Arrival), "arrival");
    EXPECT_STREQ(to_string(Lease::Send), "send");
    EXPECT_STREQ(to_string(Lease::Reading), "reading");
    EXPECT_STREQ(to_string(Lease::Call), "call");
    EXPECT_STREQ(to_string(Lease::Audio), "audio");
    EXPECT_STREQ(to_string(Lease::Hourly), "hourly");
    EXPECT_STREQ(to_string(Lease::Setup), "setup");
    EXPECT_STREQ(to_string(Lease::Phone), "phone");
    EXPECT_STREQ(to_string(ClassicState::Idle), "idle");
    EXPECT_STREQ(to_string(ClassicState::Opening), "opening");
    EXPECT_STREQ(to_string(ClassicState::Open), "open");
    EXPECT_STREQ(to_string(ClassicState::Lingering), "lingering");
    EXPECT_STREQ(to_string(ClassicState::Closing), "closing");
    EXPECT_STREQ(to_string(ClassicState::Failed), "failed");
}

TEST(ClassicDemand, TheSyncOnLeReturnIsSpacedTenMinutesUnlessThereIsABacklog) {
    EXPECT_TRUE(sync_due_on_le_return(100, -1, false));
    EXPECT_FALSE(sync_due_on_le_return(100 + CLASSIC_SYNC_MIN_INTERVAL_SECONDS - 1, 100, false));
    EXPECT_TRUE(sync_due_on_le_return(100 + CLASSIC_SYNC_MIN_INTERVAL_SECONDS, 100, false));
    EXPECT_TRUE(sync_due_on_le_return(130, 100, true));
}

// 18 November: last sync at 03:09, asleep 03:15 to 10:14 on a clock that
// stood still. Seen across the sleep, the sync is due when the LE link returns.
TEST(ClassicDemand, ASyncDatedBeforeALongSleepIsDueAfterIt) {
    const int64_t last = 1000;
    const int64_t now = last + 360; // six minutes on the stopped clock
    const int64_t slept = 7 * 3600;
    EXPECT_FALSE(sync_due_on_le_return(now, last, false));
    EXPECT_TRUE(sync_due_on_le_return(now, dated_across_sleep(last, slept), false));
    // A short sleep keeps the throttle: two minutes asleep, still not due.
    EXPECT_FALSE(sync_due_on_le_return(now, dated_across_sleep(last, 120), false));
    EXPECT_EQ(dated_across_sleep(-1, slept), -1);
    // Older than the clock: long ago, still due, never confused with never.
    EXPECT_EQ(dated_across_sleep(100, slept), SYNC_LONG_AGO);
    EXPECT_TRUE(sync_due_on_le_return(now, SYNC_LONG_AGO, false));
    EXPECT_GE(now - SYNC_LONG_AGO, 3600);
    EXPECT_EQ(dated_across_sleep(5000, 600), 4400);
}

// #62: a link the iPhone opened while the LE link is missing is kept: closing
// it did not bring LE back on 23 November and lost calls and messages.
TEST(ClassicDemand, APhoneLinkIsKeptWhileTheLeLinkIsMissing) {
    World w;
    w.facts.le_connected = false;
    w.phone_opens(100);
    for (int64_t now = 101; now < 100 + PHONE_LINK_WITHOUT_LE_SECONDS; now += 5)
        EXPECT_EQ(w.at(now), DemandAction::None) << now;
    EXPECT_FALSE(w.demand.cycled_for_le(100 + PHONE_LINK_WITHOUT_LE_SECONDS - 1));
    EXPECT_EQ(w.at(100 + PHONE_LINK_WITHOUT_LE_SECONDS), DemandAction::Close);
}

TEST(ClassicDemand, ThePhoneLinkGoesTheUsualWayOnceLeIsBack) {
    World w;
    w.facts.le_connected = false;
    w.phone_opens(100);
    w.run(101, 400);
    w.facts.le_connected = true;
    // Idle since it opened: the two minutes are long gone.
    EXPECT_EQ(w.at(401), DemandAction::Close);
}

TEST(ClassicDemand, AProbeIsAnAutomaticDataLeaseWithNoLinger) {
    EXPECT_TRUE(lease_automatic(Lease::Probe));
    EXPECT_EQ(lease_kind(Lease::Probe), LeaseKind::Data);
    EXPECT_EQ(lease_linger_seconds(Lease::Probe), 0);
    EXPECT_STREQ(to_string(Lease::Probe), "probe");
}

// #62, from the review: the probe must page the iPhone without an LE link,
// and only the probe.
TEST(ClassicDemand, AProbeOpensWithoutAnLeLink) {
    World w;
    w.facts.le_connected = false;
    ASSERT_TRUE(w.demand.take(Lease::Probe, 100));
    EXPECT_EQ(w.at(100), DemandAction::OpenData);
}

TEST(ClassicDemand, OnlyTheProbeOpensWithoutAnLeLink) {
    World w;
    w.facts.le_connected = false;
    ASSERT_TRUE(w.demand.take(Lease::Hourly, 100));
    EXPECT_EQ(w.at(100), DemandAction::None);
    EXPECT_FALSE(w.demand.holds(Lease::Hourly));
}

TEST(ClassicDemand, AnUnansweredProbeLeavesNoBackoff) {
    World w;
    w.facts.le_connected = false;
    ASSERT_TRUE(w.demand.take(Lease::Probe, 100));
    EXPECT_EQ(w.at(100), DemandAction::OpenData);
    // Nobody answers: the opening gives up.
    DemandAction last = DemandAction::None;
    for (int64_t now = 101; now <= 100 + CLASSIC_OPEN_TIMEOUT_SECONDS + 1; ++now)
        last = w.at(now);
    (void)last;
    EXPECT_FALSE(w.demand.holds(Lease::Probe));
    // The iPhone back over LE: the sync is not held back by a backoff.
    w.facts.le_connected = true;
    const int64_t later = 100 + CLASSIC_OPEN_TIMEOUT_SECONDS + 5;
    EXPECT_TRUE(w.demand.take(Lease::Sync, later));
}
