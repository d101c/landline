#pragma once

#include <atomic>
#include <functional>
#include <gio/gio.h>
#include <mutex>
#include <string_view>

// logind's announcement of the computer's sleep. BlueZ reconnects, on wake,
// every device that had an audio profile connected when the computer went
// down, and that BR/EDR link then keeps the iPhone from reopening the LE
// one. Closing the classic link before the sleep leaves nothing for BlueZ to
// bring back; this is what tells the supervisor that the sleep is coming,
// and holds it off for the moment the close takes.
namespace tether {

    // LANDLINE_SLEEP_CLOSE=0: the announcement is not listened to.
    inline bool sleep_close_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // How long the sleep is held off at most, whatever the supervisor does.
    inline constexpr int SLEEP_HOLD_MAX_MS = 3000;

    class SleepAnnouncer {
    public:
        // true: the computer is about to sleep. false: it has woken up.
        using Handler = std::function<void(bool going_to_sleep)>;
        // Runs a function on the thread that dispatches `conn`'s signals.
        using Invoker = std::function<void(const std::function<void()>&)>;

        ~SleepAnnouncer() { stop(); }

        // `conn` is the system bus. The handler runs on the dispatching thread.
        void start(GDBusConnection* conn, Invoker invoke, Handler handler);
        void stop();

        // What had to be done before the sleep is done: logind may go on.
        // Any thread; harmless when no sleep is pending.
        void ready_to_sleep();

    private:
        static void on_signal(GDBusConnection*,
                              const gchar*,
                              const gchar*,
                              const gchar*,
                              const gchar*,
                              GVariant* parameters,
                              gpointer user_data);
        static gboolean on_hold_expired(gpointer user_data);

        // The delay inhibitor: logind waits for it to be closed, up to its
        // own InhibitDelayMaxSec, before suspending.
        void take_inhibitor();
        void drop_inhibitor();

        GDBusConnection* conn_ = nullptr;
        Invoker invoke_;
        Handler handler_;
        guint subscription_ = 0;
        // The timer that lets logind go after SLEEP_HOLD_MAX_MS. Dispatching thread only.
        guint hold_source_ = 0;
        std::mutex mutex_;
        int inhibitor_fd_ = -1;
    };

} // namespace tether
