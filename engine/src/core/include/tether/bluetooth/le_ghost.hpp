#pragma once

#include <cstdint>
#include <optional>

// #63: BlueZ says the LE link is up while the controller holds none.
//
// Seen on 23 November (11:37 → 11:50): after a burst of LE links that each
// died within seconds, BlueZ kept Bearer.LE1.Connected while the kernel's
// list of links was empty. The engine trusts BlueZ, so nothing was missing
// as far as it knew: no advertisement, no probe, and the notifications stuck
// at "Subscribing…". A disconnect through BlueZ resynced it at once.
//
// So, while BlueZ says the link is up and the notification service has not
// answered for LE_GHOST_WAIT_SECONDS, the kernel is asked every
// LE_GHOST_CHECK_SECONDS; missing LE_GHOST_MISSES times in a row, the device
// is disconnected through BlueZ. A list that could not be read proves
// nothing and counts for nothing. Pure.
namespace tether::bluetooth {

    inline constexpr int LE_GHOST_WAIT_SECONDS = 30;
    inline constexpr int LE_GHOST_CHECK_SECONDS = 10;
    inline constexpr int LE_GHOST_MISSES = 2;
    // Never more often than this: each one ends a link, which the other rules
    // (strikes, #62) count as a link that died unused.
    inline constexpr int LE_GHOST_MIN_GAP_SECONDS = 300;

    class LeGhost {
    public:
        // Whether to read the kernel's list on this pass.
        bool check_due(int64_t now, bool bluez_le_up, bool notifications_ready) {
            if (!bluez_le_up || notifications_ready) {
                up_since_ = -1;
                last_check_ = -1;
                misses_ = 0;
                return false;
            }
            if (up_since_ < 0)
                up_since_ = now;
            if (now - up_since_ < LE_GHOST_WAIT_SECONDS)
                return false;
            if (last_resync_ >= 0 && now - last_resync_ < LE_GHOST_MIN_GAP_SECONDS)
                return false;
            return last_check_ < 0 || now - last_check_ >= LE_GHOST_CHECK_SECONDS;
        }

        // What the kernel said. True when BlueZ is to be told the link is gone.
        bool listed(int64_t now, std::optional<bool> in_kernel) {
            last_check_ = now;
            if (!in_kernel.has_value())
                return false;
            if (*in_kernel) {
                misses_ = 0;
                return false;
            }
            if (++misses_ < LE_GHOST_MISSES)
                return false;
            // Done; a fresh wait before the next verdict.
            misses_ = 0;
            up_since_ = now;
            last_check_ = -1;
            last_resync_ = now;
            ++resyncs_;
            return true;
        }

        int resyncs() const { return resyncs_; }

        // No device followed on this pass (the iPhone away for a while):
        // what was counted belongs to a link long gone.
        void forget() {
            up_since_ = -1;
            last_check_ = -1;
            misses_ = 0;
        }

    private:
        int64_t up_since_ = -1;
        int64_t last_check_ = -1;
        int misses_ = 0;
        int64_t last_resync_ = -1;
        int resyncs_ = 0;
    };

} // namespace tether::bluetooth
