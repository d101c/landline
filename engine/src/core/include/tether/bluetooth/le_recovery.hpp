#pragma once

#include <cstdint>

// #62: an iPhone that is near but does not come back over LE.
//
// Seen on 18, 19 and 23 November: the iPhone next to the computer, its
// Bluetooth on, and the LE link (notifications) never back until its
// Bluetooth was turned off and on. Advertising and waiting is all the lean
// mode did. What others do:
//   - an ANCS accessory is expected to reconnect itself: Apple's advice for
//     watches is to look for the iPhone and connect when it is there;
//   - car kits page the bonded phone on the classic link from time to time:
//     an iPhone whose Bluetooth is on answers, even locked.
//
// So, while the LE link is missing:
//   1. the classic link is paged as a presence probe, with a backoff, as long
//      as the iPhone was near not long ago;
//   2. each time the iPhone is there over the classic link (a probe answered,
//      or a link it opened), the engine dials the LE link itself, every
//      LE_RECOVERY_DIAL_SECONDS;
//   3. (in classic_demand.hpp) a classic link the iPhone opened is kept;
//   4. after LE_RECOVERY_STUCK_DIALS dials with the iPhone there and no LE
//      link, the state has a name: stuck, and the menu says what to do;
//   5. each stuck episode is written down by the caller, with what preceded
//      it.
//
// Pure: the caller feeds it once per pass and carries out what it returns.
namespace tether::bluetooth {

    // Waits between probes: 1, 3, 10 minutes, then every 15.
    inline constexpr int LE_RECOVERY_PROBE_WAITS[] = {60, 180, 600};
    inline constexpr int LE_RECOVERY_PROBE_EVERY = 900;
    // The iPhone counts as near when a link to it was up this little ago.
    inline constexpr int LE_RECOVERY_NEAR_SECONDS = 2 * 60 * 60;
    // Between two LE dials while the iPhone is there over the classic link.
    inline constexpr int LE_RECOVERY_DIAL_SECONDS = 120;
    // Dials with the iPhone there and still no LE link: stuck.
    inline constexpr int LE_RECOVERY_STUCK_DIALS = 3;

    struct LeRecoveryFacts {
        int64_t now = 0;
        bool le_connected = false;
        bool classic_connected = false;
        // A call, the iPhone's sound or tethering on the classic link: no
        // dial, no probe, nothing that competes with it.
        bool busy = false;
        // The adapter is on; nothing is tried otherwise.
        bool adapter_on = true;
    };

    struct LeRecoveryStep {
        bool probe = false;        // page the iPhone over the classic link
        bool dial = false;         // connect the LE link from here
        bool stuck_started = false;
        bool stuck_ended = false;
    };

    class LeRecovery {
    public:
        LeRecoveryStep step(const LeRecoveryFacts& f) {
            LeRecoveryStep out;
            // The engine starts with the session: whoever opened it is at
            // the computer, and the iPhone most likely with them.
            if (near_at_ < 0)
                near_at_ = f.now;
            if (f.le_connected) {
                near_at_ = f.now;
                out.stuck_ended = stuck_;
                lost_since_ = -1;
                probes_ = 0;
                next_probe_at_ = -1;
                dials_ = 0;
                last_dial_at_ = -1;
                stuck_ = false;
                return out;
            }
            if (lost_since_ < 0) {
                lost_since_ = f.now;
                next_probe_at_ = f.now + LE_RECOVERY_PROBE_WAITS[0];
            }
            if (!f.adapter_on || f.busy)
                return out;
            if (f.classic_connected) {
                near_at_ = f.now;
                if (last_dial_at_ < 0 || f.now - last_dial_at_ >= LE_RECOVERY_DIAL_SECONDS) {
                    out.dial = true;
                    last_dial_at_ = f.now;
                    ++dials_;
                    if (dials_ >= LE_RECOVERY_STUCK_DIALS && !stuck_) {
                        stuck_ = true;
                        out.stuck_started = true;
                    }
                }
                return out;
            }
            const bool near = near_at_ >= 0 && f.now - near_at_ <= LE_RECOVERY_NEAR_SECONDS;
            if (near && f.now >= next_probe_at_) {
                out.probe = true;
                ++probes_;
                next_probe_at_ = f.now + probe_wait(probes_);
            }
            return out;
        }

        // A probe that went unanswered is not the iPhone refusing LE: the
        // stuck count only grows with the iPhone there.
        bool stuck() const { return stuck_; }
        int64_t lost_since() const { return lost_since_; }
        int dials() const { return dials_; }
        int probes() const { return probes_; }

    private:
        static int probe_wait(int done) {
            constexpr int n = static_cast<int>(sizeof(LE_RECOVERY_PROBE_WAITS) / sizeof(LE_RECOVERY_PROBE_WAITS[0]));
            return done < n ? LE_RECOVERY_PROBE_WAITS[done] : LE_RECOVERY_PROBE_EVERY;
        }

        int64_t near_at_ = -1;
        int64_t lost_since_ = -1;
        int64_t next_probe_at_ = -1;
        int64_t last_dial_at_ = -1;
        int probes_ = 0;
        int dials_ = 0;
        bool stuck_ = false;
    };

} // namespace tether::bluetooth
