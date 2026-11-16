#pragma once

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <string_view>

namespace tether {

    // What the daemon exits with when a bus it lives on is closed under it
    // (EX_TEMPFAIL). Not zero, so a service manager set to restart on failure
    // starts a fresh process, which connects to the bus that is there now.
    inline constexpr int BUS_CLOSED_EXIT_STATUS = 75;

    // Between noticing a closed bus and leaving, so the status that names the
    // cause reaches the subscribed clients first.
    inline constexpr int BUS_CLOSED_EXIT_GRACE_MS = 2000;

    // TETHER_BUS_WATCH=0 leaves the daemon running on a closed bus, as before.
    inline bool bus_watch_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // Whether a failed D-Bus call failed because this process's own connection
    // is closed, which no retry on that connection can cure.
    bool is_bus_closed_error(const GError* error);

    struct BusWatchState;

    // Notices when a D-Bus connection of this process is closed.
    //
    // GDBus hands every caller the same connection per bus, and a closed one
    // stays closed and stays shared for as long as anything holds it: nothing
    // in the process reaches that bus again. Its "closed" signal is delivered
    // on the main context of whichever thread first asked for the bus, which
    // in this daemon may be one nobody runs, so the connections are polled.
    class BusWatch {
    public:
        // A new reference to the connection, or null while there is none yet.
        using Source = std::function<GDBusConnection*()>;
        // Called once per watched connection, from the watching thread.
        using ClosedFn = std::function<void(const std::string& name)>;

        explicit BusWatch(ClosedFn on_closed, int poll_ms = 1000);
        ~BusWatch();

        BusWatch(const BusWatch&) = delete;
        BusWatch& operator=(const BusWatch&) = delete;

        // Before start(). The source is asked again, every `retry_ms`, until
        // it yields a connection.
        void watch(std::string name, Source source, int retry_ms = 10000);

        // The process-wide connection to the session or the system bus.
        void watch_bus(std::string name, GBusType type);

        void start();
        void stop();

    private:
        std::unique_ptr<BusWatchState> state_;
    };

} // namespace tether
