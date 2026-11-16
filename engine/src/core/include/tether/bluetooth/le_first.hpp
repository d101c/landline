#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

// "LE first": how the LE link, which carries notifications, is given the
// chance to come up before or instead of being shut out by the BR/EDR one.
//
// Observed on 16 November 2026, three times: after the engine or bluetoothd
// restarted, BR/EDR, messages and contacts were back within seconds and the
// LE link stayed down for minutes with the solicitation on air. Whenever
// every link had dropped at once, the iPhone opened LE within a second and
// the rest followed. The iPhone opens the LE link; nothing asked from this
// side has ever been seen to open one that carries notifications.
//
// So, when the phone is expected to offer notifications:
//   - while no link is up, nothing is asked over BR/EDR for a few seconds:
//     the advert is alone on air, as in the case that works (the hold);
//   - BR/EDR up with the advert on air and no LE for a while: every link is
//     dropped once, on purpose, which puts the phone back in that case (the
//     recycle);
//   - the bond is never left preferring BR/EDR once a connect attempt is
//     over: BlueZ then turns the inbound LE link away;
//   - the outbound LE connect, which takes the advert off air while it
//     runs, waits until the advert has had its chance.
//
// Everything here is a decision on facts handed in.
namespace tether::bluetooth {

    // How long BR/EDR is left alone after every link went down, or after the
    // engine or bluetoothd started. An iPhone that answers the advert does so
    // in about a second.
    inline constexpr int LE_FIRST_HOLD_SECONDS = 10;
    // How long BR/EDR may be up, with the advert on air and no LE link,
    // before every link is dropped once.
    inline constexpr int LE_RECYCLE_SECONDS = 12;
    // No second recycle sooner than this, whatever happens.
    inline constexpr int LE_RECYCLE_MIN_INTERVAL_SECONDS = 600;
    // BR/EDR coming up this soon after a hold ran out says the phone was
    // there during the hold and did not open LE: a recycle would only repeat
    // what has just failed.
    inline constexpr int LE_HOLD_PROOF_SECONDS = 30;
    // How long the advert is on air without an LE link before the outbound
    // connect is tried. It used to be tried three seconds after BR/EDR came
    // up, and held the advert off air for up to 45 s.
    inline constexpr int LE_DIAL_AFTER_SECONDS = 90;

    inline constexpr int LE_FIRST_SECONDS_MAX = 600;

    struct LeFirstConfig {
        // Zero switches the measure off.
        int hold_seconds = LE_FIRST_HOLD_SECONDS;
        int recycle_seconds = LE_RECYCLE_SECONDS;
        bool unpin_after_attempt = true;
        int dial_after_seconds = LE_DIAL_AFTER_SECONDS;

        bool operator==(const LeFirstConfig&) const = default;
    };

    // The behaviour before any of this.
    inline LeFirstConfig le_first_off() { return LeFirstConfig{0, 0, false, 0}; }

    inline int le_first_seconds_from(const char* text, int fallback) {
        if (!text || !*text)
            return fallback;
        int value = 0;
        const char* end = text + std::strlen(text);
        const auto [parsed, ec] = std::from_chars(text, end, value);
        if (ec != std::errc() || parsed != end || value < 0 || value > LE_FIRST_SECONDS_MAX)
            return fallback;
        return value;
    }

    // TETHER_LE_FIRST=0 turns everything off at once. Otherwise
    // TETHER_LE_FIRST_HOLD_SECONDS and TETHER_LE_RECYCLE_SECONDS set the two
    // durations, 0 to 600, 0 switching that one measure off.
    inline LeFirstConfig le_first_config_from(const char* master, const char* hold, const char* recycle) {
        if (master && std::string_view(master) == "0")
            return le_first_off();
        LeFirstConfig config;
        config.hold_seconds = le_first_seconds_from(hold, LE_FIRST_HOLD_SECONDS);
        config.recycle_seconds = le_first_seconds_from(recycle, LE_RECYCLE_SECONDS);
        return config;
    }

    // What both decisions look at first: whether an LE link is to be expected
    // from this phone at all.
    struct LeExpectation {
        bool ancs_enabled = false;
        // The bond has an LE half.
        bool le_available = false;
        // The phone has offered its notification service on this bond.
        bool phone_offers_ancs = false;
        // The bond is broken and nothing is to be asked of the phone.
        bool standing_back = false;

        bool expected() const { return ancs_enabled && le_available && phone_offers_ancs && !standing_back; }
    };

    struct HoldFacts {
        LeExpectation le;
        bool le_connected = false;
        bool classic_connected = false;
        // An open MAP or PBAP session: BR/EDR reaches the phone already.
        bool obex_up = false;
        int64_t now = 0;
        // When no link at all was first seen, negative while one is up.
        int64_t all_down_since = -1;
    };

    // Whether BR/EDR is left alone for now: no connect, no MAP or PBAP session.
    inline bool hold_classic_for_le(const LeFirstConfig& config, const HoldFacts& facts) {
        if (config.hold_seconds <= 0 || !facts.le.expected())
            return false;
        if (facts.le_connected || facts.classic_connected || facts.obex_up)
            return false;
        if (facts.all_down_since < 0 || facts.now < facts.all_down_since)
            return false;
        return facts.now - facts.all_down_since < config.hold_seconds;
    }

    struct RecycleFacts {
        LeExpectation le;
        bool le_connected = false;
        bool classic_connected = false;
        // A connect to the phone, on either bearer, is still in flight.
        bool connect_outstanding = false;
        // A call is up or the phone's audio is playing here.
        bool phone_busy = false;
        int64_t now = 0;
        // Since when BR/EDR has been up, negative when it is not.
        int64_t classic_connected_since = -1;
        // Since when the advert has been on air with the LE link down,
        // negative when it is not.
        int64_t advert_on_air_since = -1;
        // When the last hold ran out without an LE link, negative for never.
        int64_t hold_expired_at = -1;
        // When the last recycle was done, negative for never.
        int64_t last_recycle_at = -1;
        // A recycle was done and no LE link has been seen since.
        bool spent = false;
    };

    enum class RecycleVerdict {
        Recycle,
        Off,
        NotExpected,
        LeUp,
        ClassicDown,
        Busy,
        // BR/EDR or the advert has not been up long enough.
        TooEarly,
        // The phone was given the advert alone a moment ago and did not come.
        HoldJustFailed,
        Spent,
        TooSoonAfterLast,
    };

    inline RecycleVerdict recycle_for_le(const LeFirstConfig& config, const RecycleFacts& facts) {
        if (config.recycle_seconds <= 0)
            return RecycleVerdict::Off;
        if (!facts.le.expected())
            return RecycleVerdict::NotExpected;
        if (facts.le_connected)
            return RecycleVerdict::LeUp;
        if (!facts.classic_connected || facts.classic_connected_since < 0)
            return RecycleVerdict::ClassicDown;
        if (facts.connect_outstanding || facts.phone_busy)
            return RecycleVerdict::Busy;
        if (facts.spent)
            return RecycleVerdict::Spent;
        if (facts.last_recycle_at >= 0 && facts.now - facts.last_recycle_at < LE_RECYCLE_MIN_INTERVAL_SECONDS)
            return RecycleVerdict::TooSoonAfterLast;
        if (facts.hold_expired_at >= 0 && facts.classic_connected_since >= facts.hold_expired_at &&
            facts.classic_connected_since - facts.hold_expired_at <= LE_HOLD_PROOF_SECONDS)
            return RecycleVerdict::HoldJustFailed;
        if (facts.advert_on_air_since < 0 || facts.now - facts.advert_on_air_since < config.recycle_seconds ||
            facts.now - facts.classic_connected_since < config.recycle_seconds)
            return RecycleVerdict::TooEarly;
        return RecycleVerdict::Recycle;
    }

    // Whether the one outbound LE connect may go out. `settled` is the old
    // rule on its own: BR/EDR up for BEARER_SETTLE_SECONDS.
    inline bool le_dial_due(const LeFirstConfig& config, bool settled, int64_t now, int64_t advert_on_air_since) {
        if (!settled)
            return false;
        if (config.dial_after_seconds <= 0)
            return true;
        return advert_on_air_since >= 0 && now - advert_on_air_since >= config.dial_after_seconds;
    }

} // namespace tether::bluetooth
