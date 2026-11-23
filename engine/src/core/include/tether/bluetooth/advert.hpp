#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <string_view>

namespace tether::bluetooth {

    // How long BlueZ keeps the solicitation on air before retiring it and calling
    // Release. iOS needs it running long enough to notice.
    inline constexpr int ANCS_ADVERT_TIMEOUT_SECONDS = 180;

    // LEAdvertisement1.MinInterval / MaxInterval, in whole milliseconds, which
    // is all BlueZ takes. Zero leaves the property out, and with it the choice
    // to the kernel (1.28 s by default).
    struct AdvertInterval {
        uint32_t min_ms = 0;
        uint32_t max_ms = 0;

        bool set() const { return min_ms > 0 && max_ms >= min_ms; }
        bool operator==(const AdvertInterval&) const = default;
    };

    // Apple's accessory guidelines: 20 ms for at least the first 30 s, then
    // one of a list of longer intervals of which 152.5 ms is the shortest. An
    // iPhone that is locked or asleep only listens in short windows, and an
    // advert every 1.28 s can miss several of them in a row.
    //
    // Fast for three minutes after the LE link went down: 30 s had run out
    // before a phone whose Bluetooth was off for under a minute came back.
    // After that 152.5 ms for as long as the phone is away, hours included:
    // about 2 ms of radio every 152.5 ms, a few milliwatts.
    inline constexpr int ADVERT_FAST_SECONDS = 180;
    inline constexpr int ADVERT_FAST_SECONDS_MAX = 3600;
    // Fast again for this long when BR/EDR reaches the phone while LE is still
    // down: the phone is known to be here, and LE is what is missing.
    inline constexpr int ADVERT_SEEN_FAST_SECONDS = 30;
    // BlueZ retires the advert after ANCS_ADVERT_TIMEOUT_SECONDS. It is put
    // back this long before that, so it never goes off air waiting for the
    // supervising thread to notice.
    inline constexpr int ADVERT_RENEW_MARGIN_SECONDS = 20;
    // Fast again for this long when every link has just gone down, when the
    // links were dropped on purpose, and when the computer wakes up: the
    // moments the phone is expected to answer the advert, whatever the time
    // since the LE link was lost. On 17 November 2026 the LE link had been
    // down for hours, so each of those moments was spent at 152.5 ms.
    inline constexpr int ADVERT_REARM_FAST_SECONDS = 60;
    inline constexpr AdvertInterval ADVERT_INTERVAL_FAST{20, 20};
    // BlueZ converts to 0.625 ms slots and truncates: 152 ms gives 243 slots
    // (151.875 ms) and 153 ms gives 244 (152.5 ms), so the pair brackets it.
    inline constexpr AdvertInterval ADVERT_INTERVAL_STEADY{152, 153};
    // The longest interval of Apple's list, 1022.5 ms (1636 slots; 1022 ms
    // truncates to 1635). For links that served and keep dropping
    // (le_strikes.hpp, Pebble's churn): a phone at the edge of range is then
    // offered a link about once a second and not fifty times. No longer after
    // links that died unused (#29): an iPhone indoors took about 80 s to
    // answer it on 18 November.
    inline constexpr AdvertInterval ADVERT_INTERVAL_SLOW{1022, 1023};

    // Fast for `fast_seconds` after the LE link went down, and for
    // ADVERT_SEEN_FAST_SECONDS after BR/EDR reached the phone without LE;
    // steady otherwise. Both instants are negative when they do not apply.
    inline AdvertInterval advert_interval_for(int64_t now,
                                              int64_t le_down_since,
                                              int fast_seconds = ADVERT_FAST_SECONDS,
                                              int64_t seen_without_le_since = -1,
                                              int64_t rearmed_at = -1) {
        const auto within = [now](int64_t since, int seconds) {
            return since >= 0 && now >= since && now - since < seconds;
        };
        const bool fast =
            within(le_down_since, fast_seconds) || within(seen_without_le_since, ADVERT_SEEN_FAST_SECONDS) ||
            (fast_seconds > 0 && within(rearmed_at, ADVERT_REARM_FAST_SECONDS));
        return fast ? ADVERT_INTERVAL_FAST : ADVERT_INTERVAL_STEADY;
    }

    // TETHER_ADVERT_FAST_SECONDS: how long the fast interval lasts after a
    // loss, 0 to 3600. 30 is the previous behaviour, 0 never advertises fast.
    inline int advert_fast_seconds_from(const char* text, int fallback) {
        if (!text || !*text)
            return fallback;
        int value = 0;
        for (const char* c = text; *c; ++c) {
            if (*c < '0' || *c > '9' || value > ADVERT_FAST_SECONDS_MAX)
                return fallback;
            value = value * 10 + (*c - '0');
        }
        return value <= ADVERT_FAST_SECONDS_MAX ? value : fallback;
    }

    // Whether an advert registered `age_seconds` ago should be registered
    // again now, ahead of BlueZ retiring it.
    inline bool advert_renewal_due(int64_t age_seconds) {
        return age_seconds >= ANCS_ADVERT_TIMEOUT_SECONDS - ADVERT_RENEW_MARGIN_SECONDS;
    }

    // TETHER_ADVERT_INTERVALS=0 goes back to an advert with no interval at all.
    inline bool advert_intervals_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // The object BlueZ introspects. The interval properties exist only when an
    // interval is asked for, so an advert without one is what it always was.
    std::string advert_introspection_xml(const AdvertInterval& interval);

    struct AdvertState;

    // Broadcasts an LE advertisement soliciting the ANCS service.
    class AncsAdvertisement {
    public:
        AncsAdvertisement(GDBusConnection* connection, std::string adapter_path, AdvertInterval interval = {});
        ~AncsAdvertisement();

        AncsAdvertisement(const AncsAdvertisement&) = delete;
        AncsAdvertisement& operator=(const AncsAdvertisement&) = delete;

        // Registration is two-phase, and the phases must run on different threads.
        bool export_object();
        bool register_with_bluez(std::string* error = nullptr);
        void unregister_with_bluez();
        void unexport_object();
        // The bluetoothd this was registered with is gone, and the
        // registration with it: there is nothing left to unregister.
        void forget_registration();

        bool active() const;
        // What this advert was registered with; BlueZ reads it once, at registration.
        AdvertInterval interval() const;
        // Seconds since BlueZ accepted the registration, 0 when it has not.
        int64_t age_seconds() const;

    private:
        std::unique_ptr<AdvertState> state_;
    };

} // namespace tether::bluetooth
