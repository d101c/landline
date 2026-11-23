#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Whether a notification session that once worked still does.
//
// A subscription can stay "Notifying" on both sides of the link and carry
// nothing: Pebble's firmware (PebbleOS, ancs.c, October 2026) says of iOS
// that "the CCCD subscription is likely wedged on iOS' side even though CP
// responses look fine", and checks two ways, both taken here:
//   - every 15 minutes, a request on the control point for a notification
//     that cannot exist (UID 0xFFFFFFFF). iOS answers it with the ANCS error
//     0xA2 (invalid parameter): the answer is the proof of life. No answer
//     within 5 s and the session is rebuilt;
//   - six hours without a single packet on the Notification Source and the
//     session is rebuilt as well.
//
// "Rebuilt" is the path that already exists (client.cpp, rebuild()):
// the subscription is forgotten on this side and asked again with
// StartNotify, under the rules of subscription_guard.hpp. Never a
// StopNotify then StartNotify in a row: that is what made bluetoothd 5.87
// abort (docs/moteur-reconnexion.md, section 10).
//
// Everything here is a decision on facts handed in.
namespace tether::bluetooth::ancs {

    inline constexpr int ANCS_PROBE_SECONDS = 15 * 60;
    inline constexpr int ANCS_PROBE_TIMEOUT_MS = 5000;
    // An answer that says neither yes nor no (the link going, BlueZ busy):
    // looked at again this much later.
    inline constexpr int ANCS_PROBE_RETRY_SECONDS = 60;
    inline constexpr int ANCS_QUIET_SECONDS = 6 * 3600;
    // The UID no notification has: iOS numbers them from zero upwards.
    inline constexpr uint32_t ANCS_PROBE_UID = 0xffffffff;

    enum class ProbeAnswer {
        // The phone answered, with an error or without.
        Alive,
        // Nothing in time.
        Silent,
        // Neither: not counted either way.
        Inconclusive,
    };

    // `ok`: the write was acknowledged. Otherwise `timed_out` (this side
    // gave up waiting) and the D-Bus error text. BlueZ reports the phone's
    // refusal as "Operation failed with ATT error: 0xa2".
    inline ProbeAnswer classify_probe(bool ok, bool timed_out, const std::string& error) {
        if (ok)
            return ProbeAnswer::Alive;
        if (error.find("ATT error") != std::string::npos || error.find("0xa2") != std::string::npos ||
            error.find("0xA2") != std::string::npos)
            return ProbeAnswer::Alive;
        if (timed_out || error.find("imed out") != std::string::npos || error.find("Timeout") != std::string::npos)
            return ProbeAnswer::Silent;
        return ProbeAnswer::Inconclusive;
    }

    struct LivenessConfig {
        bool enabled = true;
        int probe_seconds = ANCS_PROBE_SECONDS;
        int quiet_seconds = ANCS_QUIET_SECONDS;
    };

    // LANDLINE_ANCS_LIVENESS=0 turns both off; LANDLINE_ANCS_PROBE_SECONDS
    // (60 to 86400), LANDLINE_ANCS_QUIET_SECONDS (600 to 604800).
    inline LivenessConfig liveness_config_from(const char* enabled, const char* probe_seconds, const char* quiet_seconds) {
        const auto number = [](const char* text, int fallback, int min, int max) {
            if (!text || !*text)
                return fallback;
            int64_t value = 0;
            for (const char* c = text; *c; ++c) {
                if (*c < '0' || *c > '9' || value > max)
                    return fallback;
                value = value * 10 + (*c - '0');
            }
            return value >= min && value <= max ? static_cast<int>(value) : fallback;
        };
        LivenessConfig config;
        config.enabled = !enabled || std::string_view(enabled) != "0";
        config.probe_seconds = number(probe_seconds, ANCS_PROBE_SECONDS, 60, 86400);
        config.quiet_seconds = number(quiet_seconds, ANCS_QUIET_SECONDS, 600, 604800);
        return config;
    }

    class AncsLiveness {
    public:
        enum class Action {
            None,
            // Send the probe now, then probe_answer().
            Probe,
            // Rebuild the session: the probe went unanswered.
            ResubscribeSilent,
            // Rebuild the session: nothing on the Notification Source for
            // `quiet_seconds`.
            ResubscribeQuiet,
        };

        void configure(const LivenessConfig& config) { config_ = config; }
        const LivenessConfig& config() const { return config_; }

        // The session works (the phone has answered on it): the clocks
        // start. Called on each pass; only the first of a session counts.
        void ready(int64_t now) {
            if (live_)
                return;
            live_ = true;
            next_probe_ = now + config_.probe_seconds;
            last_packet_ = now;
        }

        // The session is gone or being rebuilt: nothing is probed.
        void lost() { live_ = false; }

        // The phone sent something on the Notification Source.
        void packet(int64_t now) {
            last_packet_ = now;
            // As good a proof of life as the probe's answer.
            if (live_)
                next_probe_ = now + config_.probe_seconds;
        }

        // Once per pass. `busy`: a request of the session is in flight or
        // queued; the probe waits for it.
        Action due(int64_t now, bool busy) {
            if (!config_.enabled || !live_)
                return Action::None;
            if (now - last_packet_ >= config_.quiet_seconds) {
                live_ = false;
                return Action::ResubscribeQuiet;
            }
            if (busy || now < next_probe_)
                return Action::None;
            next_probe_ = now + config_.probe_seconds;
            return Action::Probe;
        }

        Action probe_answer(int64_t now, ProbeAnswer answer) {
            switch (answer) {
            case ProbeAnswer::Alive:
                return Action::None;
            case ProbeAnswer::Silent:
                live_ = false;
                return Action::ResubscribeSilent;
            case ProbeAnswer::Inconclusive:
                next_probe_ = now + ANCS_PROBE_RETRY_SECONDS;
                return Action::None;
            }
            return Action::None;
        }

        bool live() const { return live_; }

    private:
        LivenessConfig config_;
        bool live_ = false;
        int64_t next_probe_ = 0;
        int64_t last_packet_ = 0;
    };

} // namespace tether::bluetooth::ancs
