#pragma once

#include "tether/bluetooth/ancs/notifications.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tether::bluetooth {
    class BluezMonitor;
}

namespace tether::bluetooth::ancs {

    // How often to retry discovery, subscription and the readiness probe.
    inline constexpr int ANCS_RETRY_SECONDS = 5;

    // The same retries while a session is being rebuilt: for this long after
    // the link or the subscription was lost, discovery and subscription are
    // tried every second. The first attempt after a drop comes before the LE
    // link is back, and the next one used to be five seconds away.
    inline constexpr int ANCS_SETTLE_SECONDS = 20;
    inline constexpr int ANCS_SETTLE_RETRY_SECONDS = 1;

    // Past that, and for this long, they are tried every two seconds. A phone
    // whose "Share System Notifications" switch is off offers no notification
    // service; turning it on is seen by the next discovery, which only reads
    // what BlueZ already holds and costs nothing on the air. The readiness
    // probe is a write to the phone and stays at ANCS_RETRY_SECONDS.
    inline constexpr int ANCS_WATCH_SECONDS = 300;
    inline constexpr int ANCS_WATCH_RETRY_SECONDS = 2;

    // `settle_since` is when the rebuild began, negative when none is on record.
    inline int ancs_retry_seconds(int64_t now, int64_t settle_since, bool fast_settle = true) {
        if (!fast_settle || settle_since < 0 || now < settle_since)
            return ANCS_RETRY_SECONDS;
        if (now - settle_since < ANCS_SETTLE_SECONDS)
            return ANCS_SETTLE_RETRY_SECONDS;
        return now - settle_since < ANCS_WATCH_SECONDS ? ANCS_WATCH_RETRY_SECONDS : ANCS_RETRY_SECONDS;
    }

    // How often a live subscription is re-confirmed against BlueZ.
    inline constexpr int ANCS_VERIFY_SECONDS = 30;

    // Defined in client.cpp.
    struct AncsClientState;

    // GATT client for Apple's notification service.
    //
    // Everything here is driven from one thread by tick(). BlueZ delivers
    // characteristic values on the GLib thread, which only buffers the bytes;
    // the sequencer and all D-Bus writes stay on the caller's thread, so no ANCS
    // state is touched from two places at once.
    class AncsClient {
    public:
        using NotificationFn = std::function<void(const Notification&)>;
        using WithdrawFn = std::function<void(uint32_t uid)>;
        // Reported when readiness changes; `reason` is shown to the user.
        using StatusFn = std::function<void(bool ready, const std::string& reason)>;

        AncsClient(BluezMonitor& monitor, NotificationFn on_notification, WithdrawFn on_withdraw, StatusFn on_status);
        ~AncsClient();

        AncsClient(const AncsClient&) = delete;
        AncsClient& operator=(const AncsClient&) = delete;

        // The BlueZ object path of the connected iPhone, or empty when LE drops.
        void set_device(const std::string& device_path);

        // Whether to ask for notification contents or only which app sent them.
        void set_content_enabled(bool enabled);

        // Whether a session being rebuilt retries every second. On by default.
        void set_fast_settle(bool enabled);

        // Makes the next tick discover, subscribe and probe without waiting for
        // their dates, and starts the close retries over. Same thread as tick().
        void retry_now();

        // Ends a live subscription so the next tick resubscribes on whatever link
        // is up then. No-op when not subscribed.
        void drop_session(const std::string& reason);

        void tick(int64_t now);

        bool ready() const;
        const std::string& status_reason() const;

        // Queues a notification's positive or negative action. ANCS offers only these two.
        bool perform_action(uint32_t uid, ActionId action);

        // Newest first. Safe from any thread.
        std::vector<Notification> recent_notifications(size_t limit = 50) const;

    private:
        std::unique_ptr<AncsClientState> state_;
    };

} // namespace tether::bluetooth::ancs
