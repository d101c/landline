#pragma once

#include <cstdint>

namespace tether::bluetooth {

    // An iPhone that was near when the computer's Bluetooth controller was
    // reset (the kernel takes the adapter away and gives it back) may never
    // answer the advertisement again until its own Bluetooth is turned off
    // and on (#59, same family as #43). Seen: over an hour of silence with
    // the advertisement on air. Rather than wait without a word, the menu
    // says what to do.

    // The phone counts as near when its LE link was up this little before
    // the adapter came back.
    constexpr int64_t LE_AFTER_RESET_NEAR_MS = 10 * 60 * 1000;
    // Time given to the phone to come back by itself before the hint.
    constexpr int64_t LE_AFTER_RESET_WAIT_MS = 3 * 60 * 1000;
    // After this, the user has long moved on: the hint goes.
    constexpr int64_t LE_AFTER_RESET_FORGET_MS = 2 * 60 * 60 * 1000;

    class LeAfterReset {
    public:
        // Called on each pass, times in milliseconds on the steady clock:
        // when the adapter last came back (-1 for never) and whether the LE
        // link is up now. True while the hint is due.
        bool observe(int64_t now_ms, int64_t adapter_back_at_ms, bool le_up) {
            if (le_up) {
                le_seen_ms_ = now_ms;
                return false;
            }
            if (adapter_back_at_ms < 0 || le_seen_ms_ < 0)
                return false;
            // Back since the reset: a later drop is another story.
            if (le_seen_ms_ >= adapter_back_at_ms)
                return false;
            if (adapter_back_at_ms - le_seen_ms_ > LE_AFTER_RESET_NEAR_MS)
                return false;
            const int64_t since = now_ms - adapter_back_at_ms;
            return since >= LE_AFTER_RESET_WAIT_MS && since < LE_AFTER_RESET_FORGET_MS;
        }

    private:
        int64_t le_seen_ms_ = -1;
    };

    // What the engine does about it. Turning the adapter off and on is what
    // brought the iPhone back on 19 November (the controller had been reset
    // without its firmware being loaded again, and its advertisement was not
    // heard); the iPhone's own Bluetooth did nothing. Done once per reset,
    // and only when no other device is connected: headphones or a mouse are
    // not cut without asking. Otherwise, or when it did not help, the menu
    // says what to do.
    enum class AfterResetStep {
        None,
        Restart,        // turn the adapter off and on now
        AskComputer,    // "turn Bluetooth off and on on this computer"
        AskPhone,       // restarted already, still nothing: the iPhone's turn
    };

    inline AfterResetStep after_reset_step(bool due, bool others_connected, int64_t now_ms, int64_t restarted_at_ms) {
        if (!due)
            return AfterResetStep::None;
        if (restarted_at_ms < 0)
            return others_connected ? AfterResetStep::AskComputer : AfterResetStep::Restart;
        return now_ms - restarted_at_ms >= LE_AFTER_RESET_WAIT_MS ? AfterResetStep::AskPhone : AfterResetStep::None;
    }

} // namespace tether::bluetooth
