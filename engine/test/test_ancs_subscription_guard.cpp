#include <gtest/gtest.h>
#include <tether/bluetooth/ancs/subscription_guard.hpp>

#include <deque>
#include <vector>

using namespace tether::bluetooth::ancs;

namespace {

    StopFacts turned_off(bool link_alive, bool notifying, int64_t now = 100, int64_t last_stop_at = -1) {
        StopFacts facts;
        facts.why = Release::TurnedOff;
        facts.link_alive = link_alive;
        facts.notifying = notifying;
        facts.now = now;
        facts.last_stop_at = last_stop_at;
        return facts;
    }

    // BlueZ 5.87's bookkeeping for one characteristic, as far as the CCC
    // descriptor goes (src/shared/gatt-client.c: register_notify,
    // complete_unregister_notify, enable_ccc_callback, disable_ccc_callback).
    // The phone answers nothing until answer_all(): the 16 November link.
    struct BluezChrc {
        struct Write {
            unsigned id;
            bool enable;
            int subscriber;
        };

        int notify_count = 0;
        unsigned ccc_write_id = 0;
        unsigned next_id = 1;
        std::deque<Write> on_the_link;
        std::vector<int> queued;
        // Subscribers whose enable write is still out.
        std::vector<int> enabling;
        bool notifying = false;
        bool aborted = false;

        void write(bool enable, int subscriber) {
            ccc_write_id = next_id++;
            on_the_link.push_back({ccc_write_id, enable, subscriber});
            if (enable)
                enabling.push_back(subscriber);
        }

        void start_notify(int subscriber) {
            ++notify_count;
            if (ccc_write_id) {
                queued.push_back(subscriber);
                return;
            }
            if (notify_count > 1) {
                notifying = true;
                return;
            }
            write(true, subscriber);
        }

        void stop_notify(int subscriber) {
            notifying = false;
            std::erase(queued, subscriber);
            if (std::erase(enabling, subscriber) > 0)
                return; // "cancel it and return": the count is left as it is
            if (--notify_count > 0)
                return;
            write(false, subscriber);
        }

        // Every write out is answered in turn: by the phone when `success`,
        // else by the ATT timeout or the link dropping.
        void answer_all(bool success = false) {
            while (!on_the_link.empty() && !aborted) {
                const Write done = on_the_link.front();
                on_the_link.pop_front();
                if (!ccc_write_id) {
                    aborted = true; // assert(notify_data->chrc->ccc_write_id)
                    return;
                }
                ccc_write_id = 0;
                if (done.enable) {
                    std::erase(enabling, done.subscriber);
                    notifying = success && notify_count > 0;
                    queued.clear();
                } else if (!queued.empty()) {
                    const int next = queued.front();
                    queued.erase(queued.begin());
                    write(true, next);
                }
            }
        }
    };

    // The engine's side of one characteristic, driven by the guard.
    struct Engine {
        BluezChrc& bluez;
        int64_t last_stop_at = -1;
        int subscriber = 1;
        bool subscribed = false;

        // What the code before the guard did: StopNotify whenever it believed
        // itself subscribed, StartNotify whenever it did not.
        void legacy_rebuild() {
            if (subscribed)
                bluez.stop_notify(subscriber);
            subscribed = false;
        }
        void legacy_subscribe() {
            // The first call gets no answer and times out; the second finds
            // the subscriber on record and is answered at once.
            if (bluez.queued.empty() && bluez.enabling.empty())
                bluez.start_notify(++subscriber);
            else
                subscribed = true;
        }

        void rebuild(bool link_alive, int64_t now) { release(Release::Rebuild, link_alive, now); }
        void turn_off(bool link_alive, int64_t now) { release(Release::TurnedOff, link_alive, now); }

        void release(Release why, bool link_alive, int64_t now) {
            StopFacts facts;
            facts.why = why;
            facts.link_alive = link_alive;
            facts.notifying = bluez.notifying;
            facts.now = now;
            facts.last_stop_at = last_stop_at;
            if (stop_notify_verdict(facts) == StopVerdict::Send) {
                bluez.stop_notify(subscriber);
                last_stop_at = now;
            }
            subscribed = false;
        }

        void subscribe(int64_t now) {
            if (!start_notify_allowed(now, last_stop_at))
                return;
            if (bluez.queued.empty() && bluez.enabling.empty() && bluez.notify_count == 0)
                bluez.start_notify(++subscriber);
            subscribed = classify_start(true, false, "", bluez.notifying) == StartOutcome::Live;
        }
    };

} // namespace

TEST(AncsStopNotify, ARebuildNeverStopsAnything) {
    StopFacts facts = turned_off(true, true);
    facts.why = Release::Rebuild;
    EXPECT_EQ(stop_notify_verdict(facts), StopVerdict::KeepForRebuild);
}

TEST(AncsStopNotify, TurningNotificationsOffOnALiveNotifyingLinkStops) {
    EXPECT_EQ(stop_notify_verdict(turned_off(true, true)), StopVerdict::Send);
}

TEST(AncsStopNotify, NothingIsSentToALinkThatIsDown) {
    EXPECT_EQ(stop_notify_verdict(turned_off(false, true)), StopVerdict::LinkDown);
}

// A subscriber that is not Notifying is either unknown to BlueZ or still
// waiting behind a write: stopping it is the third call of the crash.
TEST(AncsStopNotify, ASubscriptionStillBeingSetUpIsNotStopped) {
    EXPECT_EQ(stop_notify_verdict(turned_off(true, false)), StopVerdict::NotNotifying);
}

TEST(AncsStopNotify, NoSecondStopWhileTheFirstMayStillBeOnTheLink) {
    EXPECT_EQ(stop_notify_verdict(turned_off(true, true, 100, 100 - CCC_WRITE_SETTLE_SECONDS + 1)),
              StopVerdict::TooSoon);
    EXPECT_EQ(stop_notify_verdict(turned_off(true, true, 100, 100 - CCC_WRITE_SETTLE_SECONDS)), StopVerdict::Send);
}

TEST(AncsStopNotify, TheSwitchTurnsEveryStopOff) {
    EXPECT_TRUE(stop_notify_enabled(nullptr));
    EXPECT_TRUE(stop_notify_enabled("1"));
    EXPECT_FALSE(stop_notify_enabled("0"));
    StopFacts facts = turned_off(true, true);
    facts.enabled = false;
    EXPECT_EQ(stop_notify_verdict(facts), StopVerdict::Disabled);
}

TEST(AncsStartNotify, WaitsOutTheAttTimeoutAfterAStop) {
    EXPECT_TRUE(start_notify_allowed(100, -1));
    EXPECT_FALSE(start_notify_allowed(100, 100));
    EXPECT_FALSE(start_notify_allowed(100 + CCC_WRITE_SETTLE_SECONDS - 1, 100));
    EXPECT_TRUE(start_notify_allowed(100 + CCC_WRITE_SETTLE_SECONDS, 100));
    EXPECT_EQ(start_notify_wait(110, 100), CCC_WRITE_SETTLE_SECONDS - 10);
    EXPECT_EQ(start_notify_wait(100 + CCC_WRITE_SETTLE_SECONDS, 100), 0);
    // BlueZ fails an ATT request after 30 s; the wait must outlast it.
    EXPECT_GT(CCC_WRITE_SETTLE_SECONDS, 30);
}

TEST(AncsStartNotify, AnAcceptedCallIsOnlyLiveOnceNotifying) {
    EXPECT_EQ(classify_start(true, false, "", true), StartOutcome::Live);
    // BlueZ answers a second StartNotify at once for a subscriber it holds,
    // even one whose write was never answered: the code before took that
    // for a subscription and went on to stop it.
    EXPECT_EQ(classify_start(true, false, "", false), StartOutcome::Pending);
}

TEST(AncsStartNotify, NoAnswerAndInProgressArePendingNotFailures) {
    EXPECT_EQ(classify_start(false, true, "Timeout was reached", false), StartOutcome::Pending);
    EXPECT_EQ(classify_start(false, false, "GDBus.Error:org.bluez.Error.InProgress: In Progress", false),
              StartOutcome::Pending);
    EXPECT_EQ(classify_start(false, false, "GDBus.Error:org.bluez.Error.NotConnected: Not Connected", false),
              StartOutcome::Failed);
}

TEST(AncsStartNotify, AMissingObjectAsksForRediscovery) {
    EXPECT_EQ(classify_start(false,
                             false,
                             "GDBus.Error:org.freedesktop.DBus.Error.UnknownObject: Method \"StartNotify\" with "
                             "signature \"\" on interface \"org.bluez.GattCharacteristic1\" doesn't exist",
                             false),
              StartOutcome::Gone);
}

// The 16 November sequence, replayed against the bookkeeping of
// src/shared/gatt-client.c: a live subscription, a link that stops
// answering, and the engine rebuilding its session twice within the ATT
// timeout. Two disable writes end up outstanding and bluetoothd asserts.
TEST(AncsCrashSequence, TheOldRebuildAbortsBluetoothd) {
    BluezChrc bluez;
    Engine engine{bluez};
    bluez.start_notify(engine.subscriber);
    bluez.answer_all();
    bluez.notifying = true;
    engine.subscribed = true;
    ASSERT_FALSE(bluez.aborted);

    engine.legacy_rebuild();   // StopNotify: first disable write, unanswered
    engine.legacy_subscribe(); // StartNotify: queued, the call times out
    engine.legacy_subscribe(); // StartNotify again: answered at once
    ASSERT_TRUE(engine.subscribed);
    engine.legacy_rebuild(); // StopNotify: second disable write
    EXPECT_EQ(bluez.on_the_link.size(), 2u);

    bluez.answer_all(); // the ATT timeout fails both
    EXPECT_TRUE(bluez.aborted);
}

TEST(AncsCrashSequence, TheGuardedRebuildLeavesOneWriteAtMost) {
    BluezChrc bluez;
    Engine engine{bluez};
    bluez.start_notify(engine.subscriber);
    bluez.answer_all();
    bluez.notifying = true;
    engine.subscribed = true;

    // Any number of rebuilds and attempts on a link that answers nothing.
    for (int64_t now = 0; now < 60; ++now) {
        engine.rebuild(now % 2 == 0, now);
        engine.subscribe(now);
        EXPECT_LE(bluez.on_the_link.size(), 1u);
    }
    bluez.answer_all();
    EXPECT_FALSE(bluez.aborted);
}

// Notifications turned off and on again, over and over, on a link that
// answers nothing: one stop goes out, nothing is queued behind it.
TEST(AncsCrashSequence, TurningOffAndOnAgainNeverQueuesBehindAStop) {
    BluezChrc bluez;
    Engine engine{bluez};
    bluez.start_notify(engine.subscriber);
    bluez.answer_all();
    bluez.notifying = true;
    engine.subscribed = true;

    for (int64_t now = 0; now < 3 * CCC_WRITE_SETTLE_SECONDS; ++now) {
        engine.turn_off(true, now);
        engine.subscribe(now);
        EXPECT_LE(bluez.on_the_link.size(), 1u) << "at " << now;
        EXPECT_TRUE(bluez.queued.empty()) << "at " << now;
        // Thirty seconds on, the phone answers what is out.
        if (now % 30 == 29)
            bluez.answer_all(true);
    }
    bluez.answer_all();
    EXPECT_FALSE(bluez.aborted);
}

// A phone whose owner never turns the switch on: asked closely while someone
// may be doing it, then once a minute.
TEST(AncsUnanswered, CloseDuringTheWatchThenFarApart) {
    EXPECT_EQ(unanswered_seconds(100, 100, 5, 300), 5);
    EXPECT_EQ(unanswered_seconds(399, 100, 5, 300), 5);
    EXPECT_EQ(unanswered_seconds(400, 100, 5, 300), UNANSWERED_FAR_SECONDS);
    EXPECT_EQ(unanswered_seconds(100000, 100, 10, 300), UNANSWERED_FAR_SECONDS);
    // No session on record: nothing to count from.
    EXPECT_EQ(unanswered_seconds(100000, -1, 5, 300), 5);
    // Never closer than what was asked for.
    EXPECT_EQ(unanswered_seconds(100000, 100, 120, 300), 120);
}
