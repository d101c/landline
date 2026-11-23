#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

// How long an LE link takes to be of use, and how soon the supervising
// thread looks at one.
//
// Until the notification service has answered on a link, a link that dies
// is one the iPhone holds against this computer (le_strikes.hpp). The
// thread used to look once a second, and the service was asked its first
// question one pass after the subscription and heard one pass after the
// answer: on 17 November 2026 that came to 3.4 s. A link coming up, a link
// ending and a packet from the phone now each start a pass at once.
namespace tether::bluetooth {

    // Two passes started by events are at least this far apart: a burst of
    // packets must not turn the thread into a busy loop.
    inline constexpr int EVENT_PASS_GAP_MS = 100;

    // How long to wait before a pass an event asked for, given how long ago
    // the last one started. Zero: now.
    inline int event_pass_wait_ms(int64_t since_last_pass_ms, int gap_ms = EVENT_PASS_GAP_MS) {
        if (gap_ms <= 0 || since_last_pass_ms < 0 || since_last_pass_ms >= gap_ms)
            return 0;
        return static_cast<int>(gap_ms - since_last_pass_ms);
    }

    // LANDLINE_LE_PROMPT=0: the thread looks once a second, as it did.
    inline bool le_prompt_enabled(const char* env_override) {
        return !env_override || !(env_override[0] == '0' && env_override[1] == '\0');
    }

    // Dates the steps of one LE link, for the journal: the only measure
    // there is of the window in which a link can die unused.
    class LeFirstUse {
    public:
        struct Event {
            enum class Kind {
                // The subscription was confirmed by BlueZ.
                Subscribed,
                // The phone answered on the Data Source: the service is in use.
                Live,
            } kind;
            // Since the link came up.
            int64_t after_ms = 0;
            // Live: since the subscription, -1 when it was never seen apart.
            int64_t after_subscribed_ms = -1;
        };

        // Once per pass. `link_up_count` and `link_up_at_ms` are the monitor's
        // count and date of the LE bearer reading Connected; `now_ms` is on
        // the same clock. One step is reported per pass, Subscribed first.
        std::optional<Event> observe(uint64_t link_up_count,
                                     int64_t link_up_at_ms,
                                     bool le_connected,
                                     bool subscribed,
                                     bool live,
                                     int64_t now_ms) {
            if (link_up_count != link_seen_) {
                // Another link: nothing has been said about it yet. What is
                // read as subscribed or live in this very pass may still be
                // about the one before.
                link_seen_ = link_up_count;
                subscribed_at_ = -1;
                said_live_ = false;
                carried_over_ = subscribed || live;
                return std::nullopt;
            }
            if (!subscribed && !live)
                carried_over_ = false;
            if (!le_connected || link_up_at_ms < 0 || carried_over_ || link_up_count == 0)
                return std::nullopt;
            const int64_t age = std::max<int64_t>(0, now_ms - link_up_at_ms);
            if ((subscribed || live) && subscribed_at_ < 0) {
                subscribed_at_ = now_ms;
                if (!live)
                    return Event{Event::Kind::Subscribed, age};
            }
            if (live && !said_live_) {
                said_live_ = true;
                Event event{Event::Kind::Live, age};
                event.after_subscribed_ms = subscribed_at_ >= 0 && subscribed_at_ < now_ms ? now_ms - subscribed_at_ : -1;
                return event;
            }
            return std::nullopt;
        }

    private:
        uint64_t link_seen_ = 0;
        int64_t subscribed_at_ = -1;
        bool said_live_ = false;
        bool carried_over_ = false;
    };

} // namespace tether::bluetooth
