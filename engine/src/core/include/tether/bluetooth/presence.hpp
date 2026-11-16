#pragma once

#include "tether/bluetooth/bearer_supervisor.hpp"

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

// Whether the iPhone is here or gone, why it went, and what the away lock is
// doing about it. Pure: the supervisor feeds it once per tick and publishes
// what it returns. Nothing here ever unlocks a session.
namespace tether::bluetooth {

    // How long every path to the phone must stay down before it is called
    // away. Covers a link that flaps and comes straight back.
    inline constexpr int PRESENCE_CONFIRM_SECONDS = 3;
    inline constexpr int MAX_AWAY_LOCK_GRACE_SECONDS = 3600;

    inline int clamp_away_grace(int seconds) { return std::clamp(seconds, 1, MAX_AWAY_LOCK_GRACE_SECONDS); }

    // Why the phone is away, as published in bt_presence.reason:
    //   out_of_range        the link timed out: the phone walked away or ran flat
    //   switched_off        the phone ended the link: Bluetooth turned off there
    //   computer_suspended  this machine went to sleep
    //   bluetooth_off       this machine's controller is off or gone
    //   local               this side ended the link
    //   unknown             BlueZ gave no reason, or one not listed here
    inline const char* away_reason(const std::string& bluez_reason, bool adapter_powered) {
        if (!adapter_powered)
            return "bluetooth_off";
        if (bluez_reason == REASON_TIMEOUT)
            return "out_of_range";
        if (bluez_reason == "org.bluez.Reason.Remote")
            return "switched_off";
        if (bluez_reason == "org.bluez.Reason.Suspend")
            return "computer_suspended";
        if (bluez_reason == "org.bluez.Reason.Local")
            return "local";
        return "unknown";
    }

    // Applies a bt_set_lock_on_away command to the three settings it carries.
    // A field the command leaves out keeps its value, so a client that only
    // flips the switch does not reset the grace.
    template <typename ConfigT>
    void apply_lock_on_away(ConfigT& config, const nlohmann::json& command) {
        const auto boolean = [&](const char* key, bool current) {
            const auto it = command.find(key);
            return it != command.end() && it->is_boolean() ? it->template get<bool>() : current;
        };
        config.lock_on_away = boolean("enabled", config.lock_on_away);
        config.wake_on_return = boolean("wake_on_return", config.wake_on_return);
        if (const auto it = command.find("grace_seconds"); it != command.end() && it->is_number_integer())
            config.lock_away_seconds = clamp_away_grace(it->template get<int>());
    }

    struct PresenceInput {
        int64_t now = 0;
        BearerStatus bearer;
        bool obex_up = false;
        // A cut this side made on purpose and is about to undo.
        bool planned_drop = false;
        // Reason of the last org.bluez Disconnected signal, empty for none.
        std::string disconnect_reason;
        bool adapter_powered = true;
        bool lock_enabled = false;
        int grace_seconds = AWAY_LOCK_GRACE_SECONDS;
        bool wake_on_return = false;
    };

    struct PresenceStep {
        // The published state changed: send to_json().
        bool changed = false;
        // Lock the session now, then report the outcome with lock_result().
        bool lock_now = false;
        // The phone is back after this side locked the session, and the user
        // asked for the login screen to light up. Never an unlock.
        bool wake_now = false;
    };

    class PresenceTracker {
    public:
        PresenceStep step(const PresenceInput& in) {
            PresenceStep out;
            // Said once, in the event of the return; a later reading must not repeat it.
            wake_ = false;
            const bool reachable =
                in.bearer.classic_connected || in.bearer.le_connected || in.obex_up || in.planned_drop;

            if (reachable) {
                const bool was_locked = lock_ == "locked";
                gone_since_ = -1;
                if (state_ != "here") {
                    out.changed = true;
                    wake_ = state_ == "away" && was_locked && in.wake_on_return;
                    out.wake_now = wake_;
                    back_after_ = state_ == "away" ? in.now - away_from_ : 0;
                    state_ = "here";
                    reason_.clear();
                    lock_ = "none";
                }
                return out;
            }

            if (gone_since_ < 0) {
                gone_since_ = in.now;
                // A lock switched on while the phone is already gone must not
                // fire for an absence it was not watching.
                armed_ = in.lock_enabled;
            }
            // A phone never seen since the daemon started is not one that left.
            if (state_ == "unknown" || !in.bearer.device_paired)
                return out;

            const int64_t elapsed = in.now - gone_since_;
            const bool lockable = armed_ && in.lock_enabled;
            const bool lock_due = lockable && should_lock_on_away(true,
                                                                  in.bearer,
                                                                  in.obex_up,
                                                                  in.disconnect_reason,
                                                                  in.now,
                                                                  gone_since_,
                                                                  in.grace_seconds);
            if (state_ == "here" && elapsed < PRESENCE_CONFIRM_SECONDS && !lock_due)
                return out;

            const std::string reason = away_reason(in.disconnect_reason, in.adapter_powered);
            std::string lock = lock_;
            if (lock != "locked" && lock != "failed") {
                if (!lockable)
                    lock = "off";
                else if (reason != "out_of_range")
                    lock = "spared";
                else
                    lock = "pending";
            }
            if (lock_due && lock == "pending") {
                out.lock_now = true;
                lock = "locked";
            }

            if (state_ != "away" || reason != reason_ || lock != lock_) {
                if (state_ != "away")
                    away_from_ = gone_since_;
                state_ = "away";
                reason_ = reason;
                lock_ = lock;
                grace_ = in.grace_seconds;
                wake_ = false;
                out.changed = true;
            }
            away_for_ = elapsed;
            return out;
        }

        // The outcome of the lock asked for by the last step().
        void lock_result(bool ok) {
            if (!ok)
                lock_ = "failed";
        }

        // Another phone, or none: nothing is known any more. True when that
        // changes what was published.
        bool reset() {
            const bool changed = state_ != "unknown";
            *this = PresenceTracker{};
            return changed;
        }

        const std::string& state() const { return state_; }
        const std::string& lock() const { return lock_; }

        // The bt_presence payload.
        nlohmann::json to_json() const {
            nlohmann::json j;
            j["command"] = "bt_presence";
            j["state"] = state_;
            j["reason"] = reason_;
            j["away_seconds"] = state_ == "away" ? away_for_ : state_ == "here" ? back_after_ : 0;
            j["lock"] = lock_;
            j["lock_in_seconds"] = lock_ == "pending" ? std::max<int64_t>(0, grace_ - away_for_) : 0;
            j["wake"] = wake_;
            return j;
        }

    private:
        // "unknown" until the phone has been reached once, then "here" or "away".
        std::string state_ = "unknown";
        std::string reason_;
        // none (here), off (lock not enabled), spared (enabled, but the reason
        // is not a walk away), pending (grace running), locked, failed.
        std::string lock_ = "none";
        int64_t gone_since_ = -1;
        int64_t away_from_ = 0;
        int64_t away_for_ = 0;
        int64_t back_after_ = 0;
        int grace_ = AWAY_LOCK_GRACE_SECONDS;
        bool armed_ = false;
        bool wake_ = false;
    };

} // namespace tether::bluetooth
