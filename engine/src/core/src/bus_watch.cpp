#include "tether/bus_watch.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace tether {

    bool is_bus_closed_error(const GError* error) {
        return error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
    }

    struct BusWatchState {
        struct Entry {
            std::string name;
            BusWatch::Source source;
            std::chrono::milliseconds retry{10000};
            GDBusConnection* conn = nullptr;
            std::chrono::steady_clock::time_point next_try{};
            bool reported = false;
        };

        BusWatch::ClosedFn on_closed;
        std::chrono::milliseconds poll{1000};
        std::vector<Entry> entries;

        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        bool running = false;

        void check() {
            const auto now = std::chrono::steady_clock::now();
            for (auto& entry : entries) {
                if (entry.reported)
                    continue;
                if (!entry.conn) {
                    if (now < entry.next_try)
                        continue;
                    entry.next_try = now + entry.retry;
                    entry.conn = entry.source ? entry.source() : nullptr;
                    if (!entry.conn)
                        continue;
                    // GDBus would otherwise raise SIGTERM on a closed bus, but
                    // only from a main context that may never run, and the
                    // daemon answers SIGTERM by leaving with status 0.
                    g_dbus_connection_set_exit_on_close(entry.conn, FALSE);
                }
                if (g_dbus_connection_is_closed(entry.conn)) {
                    entry.reported = true;
                    if (on_closed)
                        on_closed(entry.name);
                }
            }
        }

        void run() {
            std::unique_lock<std::mutex> lock(mutex);
            while (running) {
                lock.unlock();
                check();
                lock.lock();
                wake.wait_for(lock, poll, [this] { return !running; });
            }
        }
    };

    BusWatch::BusWatch(ClosedFn on_closed, int poll_ms) : state_(std::make_unique<BusWatchState>()) {
        state_->on_closed = std::move(on_closed);
        state_->poll = std::chrono::milliseconds(poll_ms);
    }

    BusWatch::~BusWatch() {
        stop();
        for (auto& entry : state_->entries)
            g_clear_object(&entry.conn);
    }

    void BusWatch::watch(std::string name, Source source, int retry_ms) {
        BusWatchState::Entry entry;
        entry.name = std::move(name);
        entry.source = std::move(source);
        entry.retry = std::chrono::milliseconds(retry_ms);
        state_->entries.push_back(std::move(entry));
    }

    void BusWatch::watch_bus(std::string name, GBusType type) {
        watch(std::move(name), [type]() -> GDBusConnection* {
            GError* error = nullptr;
            GDBusConnection* conn = g_bus_get_sync(type, nullptr, &error);
            g_clear_error(&error);
            return conn;
        });
    }

    void BusWatch::start() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->running)
                return;
            state_->running = true;
        }
        state_->thread = std::thread([state = state_.get()] { state->run(); });
    }

    void BusWatch::stop() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->running = false;
        }
        state_->wake.notify_all();
        if (state_->thread.joinable())
            state_->thread.join();
    }

} // namespace tether
