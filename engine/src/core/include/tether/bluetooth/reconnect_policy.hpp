#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace tether::bluetooth {

    // How long the published status keeps showing messages and contacts open
    // after the BR/EDR link was dropped on purpose for hands-free. The sessions
    // are back within a few seconds; a subscriber should not see them fall for
    // a cut this side decided on.
    inline constexpr int HFP_CYCLE_MASK_SECONDS = 20;

    // A BR/EDR drop made here on purpose, and what was open when it was made.
    struct PlannedDrop {
        int64_t until = -1;
        bool map = false;
        bool pbap = false;
        std::string reason;
        // Whether a covered session has been seen closed since the drop.
        bool fell = false;

        bool active(int64_t now) const { return until >= 0 && now < until; }
        void clear() { *this = PlannedDrop{}; }

        // Ends the cover once what was open is open again, or the time is up:
        // a later loss is then a real one and is published as such.
        void observe(bool map_open, bool pbap_open, int64_t now) {
            if (until < 0)
                return;
            if (now >= until) {
                clear();
                return;
            }
            const bool restored = (!map || map_open) && (!pbap || pbap_open);
            if (!restored)
                fell = true;
            else if (fell)
                clear();
        }
    };

    // Keeps a session that was open before a planned drop reading open in a
    // bt_connection_changed payload. A profile released on purpose is left as
    // it is. Returns whether anything was covered.
    inline bool mask_planned_drop(nlohmann::json& payload, const PlannedDrop& drop, int64_t now) {
        if (!drop.active(now))
            return false;
        bool masked = false;
        const auto cover = [&](bool was_open, const char* open, const char* error, const char* held) {
            if (!was_open || payload.value(open, false) || payload.value(held, false))
                return;
            payload[open] = true;
            payload[error] = "none";
            masked = true;
        };
        cover(drop.map, "map_open", "map_error", "map_held");
        cover(drop.pbap, "pbap_open", "pbap_error", "pbap_held");
        if (masked)
            payload["profile_reason"] = drop.reason;
        return masked;
    }

    // TETHER_LONG_READ_ATTEMPTS=0: a long OBEX read holds the supervising
    // thread alone again, with no profile attempt and no notification work
    // until it ends.
    inline bool long_read_attempts_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // How long the address book, the recent calls and the mailbox wait for
    // notifications to come back. Each of those reads holds the one supervising
    // thread for seconds, and the ANCS session is rebuilt over several ticks of
    // that same thread.
    inline constexpr int ANCS_FIRST_SECONDS = 12;
    inline constexpr int ANCS_FIRST_SECONDS_MAX = 120;

    // TETHER_ANCS_FIRST_SECONDS: 0 switches the wait off, 1 to 120 sets it.
    inline int ancs_first_seconds_from(const char* text, int fallback) {
        if (!text || !*text)
            return fallback;
        int value = 0;
        for (const char* c = text; *c; ++c) {
            if (*c < '0' || *c > '9' || value > ANCS_FIRST_SECONDS_MAX)
                return fallback;
            value = value * 10 + (*c - '0');
        }
        return value <= ANCS_FIRST_SECONDS_MAX ? value : fallback;
    }

    // Decides, tick after tick, whether the long reads stand aside for the
    // notification session. A wait opens when a working session is lost, and
    // once more when the LE link then comes back. A phone whose notifications
    // never become ready gets a single wait, not one per LE flap: the LE
    // bearer flaps often, and the mailbox must not starve behind it.
    struct NotificationPriority {
        int64_t since = -1;
        bool armed = true;
        bool was_ready = false;
        bool was_le_up = false;

        bool hold(bool wanted, bool ready, bool le_up, int64_t now, int limit = ANCS_FIRST_SECONDS) {
            if (!wanted || limit <= 0) {
                since = -1;
                armed = true;
            } else if (ready) {
                since = -1;
                armed = true;
            } else if (was_ready) {
                since = now;
            } else if (le_up && !was_le_up && armed) {
                since = now;
                armed = false;
            }
            was_ready = wanted && ready;
            was_le_up = le_up;
            return since >= 0 && now - since < limit;
        }
    };

    // An address book read this recently is not read again for a session that
    // merely reopened, unless the phone reports another number of entries.
    inline constexpr int CONTACTS_FRESH_SECONDS = 600;
    // An older one is read again, but once the reconnection has settled.
    inline constexpr int CONTACTS_DEFER_SECONDS = 300;

    enum class ContactsPlan {
        PullNow,   // nothing usable in memory, or the phone's count moved
        PullLater, // in CONTACTS_DEFER_SECONDS
        Keep,      // what is in memory stands for this session
    };

    // What a newly opened PBAP session does about the address book. `pulled_age`
    // is how long ago this process last read it in full, negative when it never
    // did; the sizes are the phone's own entry counts, negative when unknown.
    inline ContactsPlan plan_contacts(bool have_book, int64_t pulled_age, int size_then, int size_now) {
        if (!have_book || pulled_age < 0)
            return ContactsPlan::PullNow;
        const bool known = size_then >= 0 && size_now >= 0;
        if (known && size_then != size_now)
            return ContactsPlan::PullNow;
        if (pulled_age < CONTACTS_FRESH_SECONDS)
            return known ? ContactsPlan::Keep : ContactsPlan::PullLater;
        return ContactsPlan::PullLater;
    }

    // The same decision on a link opened on demand, where a session opens
    // many times a day and is not kept for a read put off till later: the
    // entry count alone is asked, and the book is read again when it moved
    // or once a day.
    inline constexpr int CONTACTS_DAILY_SECONDS = 86400;

    inline ContactsPlan plan_contacts_on_demand(bool have_book, int64_t pulled_age, int size_then, int size_now) {
        if (!have_book || pulled_age < 0)
            return ContactsPlan::PullNow;
        if (size_then >= 0 && size_now >= 0 && size_then != size_now)
            return ContactsPlan::PullNow;
        return pulled_age >= CONTACTS_DAILY_SECONDS ? ContactsPlan::PullNow : ContactsPlan::Keep;
    }

    // TETHER_CONTACTS_ALWAYS_PULL=1 reads the address book on every new session.
    inline bool contacts_always_pulled(const char* env_override) {
        return env_override && std::string_view(env_override) == "1";
    }

} // namespace tether::bluetooth
