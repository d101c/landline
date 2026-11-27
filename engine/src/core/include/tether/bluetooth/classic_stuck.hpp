#pragma once

#include <cstdint>

// #64: BlueZ answers "In Progress" to every BR/EDR connect, for ever.
//
// Seen on 24 November (15:53 → 16:06), BlueZ 5.87: the iPhone refused one
// hands-free connect, and two defects in bluetoothd made that permanent.
// profiles/audio/hfp-hf.c never completes its service when its connect fails
// (connect_cb, "failed:"), so Device1 keeps a connect pending; and
// Bearer.BREDR1.Connect keeps its own request when the device answers "busy"
// at once (src/bearer.c). From then on no classic link can be opened from
// this side: no call answered here, no sync, no sending.
//
// What clears it, found by hand, without root: Device1.Disconnect releases
// the pending connect (the LE link drops and is back within seconds), then
// Device1.Connect, once the LE link is back, opens BR/EDR by the other path
// and BlueZ answers and drops the bearer's stale request.
//
// A real connect also answers "In Progress", for the few seconds a page
// lasts, and never this long with no classic link to show for it. Pure.
namespace tether::bluetooth {

    // "In Progress" this many times in a row, over at least this long, with
    // no classic link: nothing is in progress. The engine asks every 3 s.
    inline constexpr int CLASSIC_STUCK_ANSWERS = 3;
    inline constexpr int CLASSIC_STUCK_SECONDS = 6;
    // The LE link is given this long to be seen going down before a link that
    // is up is taken for the one that came back.
    inline constexpr int CLASSIC_STUCK_SETTLE_SECONDS = 5;
    // No LE link this long after the disconnect: the second half is dropped.
    inline constexpr int CLASSIC_STUCK_GIVE_UP_SECONDS = 30;
    // Never more often than this: each one ends the LE link.
    inline constexpr int CLASSIC_STUCK_MIN_GAP_SECONDS = 300;

    class ClassicStuck {
    public:
        enum class Step {
            None,
            // Device1.Disconnect, now.
            Disconnect,
            // Device1.Connect, now; said again on each pass until asked().
            Connect,
            // The LE link never came back; nothing more is tried.
            GaveUp,
        };

        // `busy_answers`: "In Progress" answers to Bearer.BREDR1.Connect in a
        // row, 0 after any other answer. `paired`: nothing is done to a phone
        // whose pairing may be what is in progress.
        Step observe(int64_t now, int busy_answers, bool classic_up, bool le_up, bool paired) {
            if (waiting_) {
                if (!le_up)
                    seen_down_ = true;
                if (le_up && (seen_down_ || now - asked_at_ >= CLASSIC_STUCK_SETTLE_SECONDS))
                    return Step::Connect;
                if (now - asked_at_ >= CLASSIC_STUCK_GIVE_UP_SECONDS) {
                    waiting_ = false;
                    return Step::GaveUp;
                }
                return Step::None;
            }
            if (classic_up || busy_answers == 0 || !paired) {
                busy_since_ = -1;
                return Step::None;
            }
            if (busy_since_ < 0)
                busy_since_ = now;
            if (busy_answers < CLASSIC_STUCK_ANSWERS || now - busy_since_ < CLASSIC_STUCK_SECONDS)
                return Step::None;
            if (last_ >= 0 && now - last_ < CLASSIC_STUCK_MIN_GAP_SECONDS)
                return Step::None;
            waiting_ = true;
            seen_down_ = false;
            asked_at_ = now;
            last_ = now;
            busy_since_ = -1;
            ++recoveries_;
            return Step::Disconnect;
        }

        // The Device1.Connect went out.
        void asked() { waiting_ = false; }

        bool recovering() const { return waiting_; }
        int recoveries() const { return recoveries_; }

    private:
        int64_t busy_since_ = -1;
        bool waiting_ = false;
        bool seen_down_ = false;
        int64_t asked_at_ = -1;
        int64_t last_ = -1;
        int recoveries_ = 0;
    };

} // namespace tether::bluetooth
