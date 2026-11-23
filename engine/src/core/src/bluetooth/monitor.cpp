#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/bond_health.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    BluezMonitor* g_bluez = nullptr;

    namespace {
        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        // BlueZ emits PropertiesChanged continuously during discovery (RSSI churn
        // above all). Coalesce bursts into one refresh instead of re-reading the
        // whole object tree per signal.
        constexpr guint REFRESH_DEBOUNCE_MS = 250;
    } // namespace

    struct MonitorState {
        std::thread thread;
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        GDBusConnection* conn = nullptr;
        std::atomic<bool> running{false};

        int efd = -1;
        guint pending_refresh = 0;
        std::vector<guint> subscriptions;
        guint name_watch = 0;
        std::atomic<bool> bluez_present{true};
        // Set once the name has been seen owned, so that the first
        // appearance is not counted as a return.
        bool bluez_seen = false;
        bool bluez_left = false;
        std::atomic<uint64_t> bluez_generation{0};

        mutable std::mutex mutex;
        BluezObjects objects;
        Capability cap;
        std::map<std::string, std::string> disconnect_reasons;
        std::map<std::string, uint64_t> le_drops;
        std::map<std::string, uint64_t> le_link_drops;
        // The LE bearer read Connected: how many times, and when last.
        std::map<std::string, BluezMonitor::LeLinkUp> le_link_ups;
        std::atomic<int64_t> adapter_back_ms{-1};
        // Called on this thread when an LE link comes up or ends.
        std::function<void()> link_listener;
        // One cut link can be signalled on its bearer and on Device1 both, so
        // the two are counted apart and the larger is what is reported.
        std::map<std::string, uint64_t> auth_failures_bearer;
        std::map<std::string, uint64_t> auth_failures_device;
        BluezMonitor::LeStranger le_stranger;
        std::string preferred_adapter_id;
        std::string secure_adapter;
        std::optional<bool> secure_connections;
        std::chrono::steady_clock::time_point secure_checked_at{};

        void run();
        void refresh();
        void bluez_appeared();
        void bluez_vanished();
        void schedule_refresh();
        void refresh_now();
        void notify();
        void link_changed();
        std::optional<bool> read_secure_connections(const std::string& adapter_id);
    };

    namespace {

        gboolean on_refresh_timeout(gpointer user_data) {
            auto* impl = static_cast<MonitorState*>(user_data);
            impl->pending_refresh = 0;
            impl->refresh();
            return G_SOURCE_REMOVE;
        }

        constexpr const char* IFACE_ADAPTER = "org.bluez.Adapter1";

        // True when the signal gives an adapter back.
        bool log_adapter_change(const gchar* signal_name, GVariant* parameters) {
            const bool added = g_strcmp0(signal_name, "InterfacesAdded") == 0;
            if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE(added ? "(oa{sa{sv}})" : "(oas)")))
                return false;

            const gchar* path = nullptr;
            GVariant* interfaces = nullptr;
            g_variant_get(parameters, added ? "(&o@a{sa{sv}})" : "(&o@as)", &path, &interfaces);

            bool adapter = false;
            if (added) {
                if (GVariant* props = g_variant_lookup_value(interfaces, IFACE_ADAPTER, nullptr)) {
                    adapter = true;
                    g_variant_unref(props);
                }
            } else {
                const gchar** names = g_variant_get_strv(interfaces, nullptr);
                for (const gchar** name = names; name && *name && !adapter; ++name)
                    adapter = g_strcmp0(*name, IFACE_ADAPTER) == 0;
                g_free(names);
            }
            g_variant_unref(interfaces);

            if (adapter)
                debug::log(INFO, "bluetooth: adapter {} {}", path, added ? "added" : "removed");
            return adapter && added;
        }

        int64_t steady_now_ms() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // What a PropertiesChanged says of the LE bearer's Connected
        // property; nothing when it is about anything else.
        std::optional<bool> le_connected_change(GVariant* parameters) {
            if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)")))
                return std::nullopt;
            const gchar* iface = nullptr;
            GVariant* changed = nullptr;
            g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed, nullptr);
            std::optional<bool> connected;
            if (changed && g_strcmp0(iface, "org.bluez.Bearer.LE1") == 0) {
                if (GVariant* value = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN)) {
                    connected = g_variant_get_boolean(value) != FALSE;
                    g_variant_unref(value);
                }
            }
            if (changed)
                g_variant_unref(changed);
            return connected;
        }

        void on_bluez_signal(GDBusConnection*,
                             const gchar*,
                             const gchar* object_path,
                             const gchar*,
                             const gchar* signal_name,
                             GVariant* parameters,
                             gpointer user_data) {
            auto* impl = static_cast<MonitorState*>(user_data);
            if (g_strcmp0(signal_name, "PropertiesChanged") != 0) {
                if (log_adapter_change(signal_name, parameters))
                    impl->adapter_back_ms = steady_now_ms();
                impl->schedule_refresh();
                return;
            }
            // A link that comes up is acted on now, not a debounce and a
            // supervision pass later: until the notification service has
            // answered on it, a link that dies is one the iPhone counts
            // against this computer (le_strikes.hpp).
            const std::optional<bool> le_connected = le_connected_change(parameters);
            if (!le_connected) {
                impl->schedule_refresh();
                return;
            }
            if (*le_connected && object_path) {
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    auto& up = impl->le_link_ups[object_path];
                    ++up.count;
                    up.at_ms = steady_now_ms();
                }
                debug::log(INFO, "bluetooth: org.bluez.Bearer.LE1 connected to {}", object_path);
            }
            impl->refresh_now();
            impl->link_changed();
        }

        void on_disconnected(GDBusConnection*,
                             const gchar*,
                             const gchar* object_path,
                             const gchar* interface_name,
                             const gchar*,
                             GVariant* parameters,
                             gpointer user_data) {
            const gchar* reason = nullptr;
            const gchar* message = nullptr;
            g_variant_get(parameters, "(&s&s)", &reason, &message);
            if (!reason || !object_path)
                return;

            auto* impl = static_cast<MonitorState*>(user_data);
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->disconnect_reasons[object_path] = reason;
                if (g_strcmp0(interface_name, "org.bluez.Bearer.LE1") == 0 ||
                    g_strcmp0(interface_name, "org.bluez.Device1") == 0)
                    ++impl->le_drops[object_path];
                // An LE link on a device there is no pairing with. Counted on
                // the bearer's signal alone: Device1 repeats it.
                if (g_strcmp0(interface_name, "org.bluez.Bearer.LE1") == 0) {
                    ++impl->le_link_drops[object_path];
                    const Device* known = nullptr;
                    for (const auto& device : impl->objects.devices)
                        if (device.path == object_path)
                            known = &device;
                    if (!known || !known->paired) {
                        ++impl->le_stranger.count;
                        impl->le_stranger.path = object_path;
                        impl->le_stranger.address_type = known ? known->address_type : std::string();
                        impl->le_stranger.reason = reason;
                    }
                }
                if (g_strcmp0(reason, REASON_AUTHENTICATION) == 0) {
                    if (g_strcmp0(interface_name, "org.bluez.Device1") == 0)
                        ++impl->auth_failures_device[object_path];
                    else
                        ++impl->auth_failures_bearer[object_path];
                }
            }
            debug::log(INFO,
                       "bluetooth: {} disconnected from {}: {} ({})",
                       interface_name ? interface_name : "?",
                       object_path,
                       reason,
                       message ? message : "");
            // The end of an LE link is acted on at once (the advert may have
            // to come off air): the snapshot is read now, so that the pass
            // this starts does not see the link as still up.
            if (g_strcmp0(interface_name, "org.bluez.Bearer.LE1") == 0) {
                impl->refresh_now();
                impl->link_changed();
            } else {
                impl->schedule_refresh();
            }
        }

        void on_bluez_appeared(GDBusConnection*, const gchar*, const gchar*, gpointer user_data) {
            static_cast<MonitorState*>(user_data)->bluez_appeared();
        }

        void on_bluez_vanished(GDBusConnection*, const gchar*, gpointer user_data) {
            static_cast<MonitorState*>(user_data)->bluez_vanished();
        }

        // on watcher thread once it iterates its context
        gboolean quit_loop(gpointer user_data) {
            auto* state = static_cast<MonitorState*>(user_data);
            if (state->loop)
                g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    void MonitorState::notify() {
        if (efd < 0)
            return;
        uint64_t one = 1;
        if (write(efd, &one, sizeof(one)) < 0)
            debug::log(WARN, "bluetooth: failed to signal event fd");
    }

    // bluetoothd left the bus: it crashed, or was stopped or restarted. No
    // signal says so for the objects it exported, and a snapshot kept as it
    // was would go on describing links that are gone.
    void MonitorState::bluez_vanished() {
        const bool was_present = bluez_present.exchange(false);
        if (bluez_seen && was_present) {
            bluez_left = true;
            debug::log(WARN,
                       "bluetooth: bluetoothd left the bus (stopped, restarted or crashed); every link and "
                       "registration went with it. Waiting for it to come back.");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (objects == BluezObjects{})
                return;
            objects = BluezObjects{};
            cap = resolve_capability(objects, std::nullopt, preferred_adapter_id, l2cap_ertm_disabled());
        }
        notify();
    }

    void MonitorState::bluez_appeared() {
        bluez_present = true;
        if (bluez_seen && bluez_left) {
            bluez_left = false;
            ++bluez_generation;
            debug::log(INFO, "bluetooth: bluetoothd is back on the bus");
        }
        bluez_seen = true;
        // Its objects follow as InterfacesAdded, each of which refreshes;
        // this one covers a daemon that had them exported already.
        schedule_refresh();
        notify();
    }

    // Without the debounce: for the few changes that are acted on at once.
    void MonitorState::refresh_now() {
        if (pending_refresh != 0) {
            if (GSource* source = g_main_context_find_source_by_id(context, pending_refresh))
                g_source_destroy(source);
            pending_refresh = 0;
        }
        refresh();
    }

    void MonitorState::link_changed() {
        std::function<void()> listener;
        {
            std::lock_guard<std::mutex> lock(mutex);
            listener = link_listener;
        }
        if (listener)
            listener();
    }

    void MonitorState::schedule_refresh() {
        if (pending_refresh != 0)
            return;
        GSource* source = g_timeout_source_new(REFRESH_DEBOUNCE_MS);
        g_source_set_callback(source, on_refresh_timeout, this, nullptr);
        pending_refresh = g_source_attach(source, context);
        g_source_unref(source);
    }

    namespace {
        std::optional<bool> environment_flag(const char* name) {
            const char* raw = std::getenv(name);
            if (!raw)
                return std::nullopt;

            const std::string_view value(raw);
            if (value == "1" || value == "true" || value == "yes" || value == "on")
                return true;
            if (value == "0" || value == "false" || value == "no" || value == "off")
                return false;

            debug::log(WARN, "bluetooth: ignoring invalid {}={}", name, value);
            return std::nullopt;
        }
    } // namespace

    std::optional<bool> probe_secure_connections(const std::string& adapter_id, std::chrono::milliseconds timeout) {
        if (adapter_id.empty())
            return std::nullopt;

        const auto configured = [] { return environment_flag("LANDLINE_BLUEZ_SECURE_CONNECTIONS"); };
        std::array<gchar*, 5> argv = {
            const_cast<gchar*>("btmgmt"),
            const_cast<gchar*>("--index"),
            const_cast<gchar*>(adapter_id.c_str()),
            const_cast<gchar*>("info"),
            nullptr,
        };

        GPid child = 0;
        gint input_fd = -1;
        gint output_fd = -1;
        GError* error = nullptr;
        const auto flags = static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                                    G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_CLOEXEC_PIPES);
        // btmgmt epolls its stdin before it runs the command, rejects /dev/null with EPERM.
        if (!g_spawn_async_with_pipes(nullptr,
                                      argv.data(),
                                      nullptr,
                                      flags,
                                      nullptr,
                                      nullptr,
                                      &child,
                                      &input_fd,
                                      &output_fd,
                                      nullptr,
                                      &error)) {
            debug::log(WARN,
                       "bluetooth: cannot start btmgmt for {}: {}",
                       adapter_id,
                       error ? error->message : "unknown error");
            g_clear_error(&error);
            return configured();
        }
        if (input_fd >= 0)
            close(input_fd);

        const int old_flags = fcntl(output_fd, F_GETFL, 0);
        if (old_flags >= 0)
            fcntl(output_fd, F_SETFL, old_flags | O_NONBLOCK);

        std::string output;
        output.reserve(2048);
        int status = 0;
        bool exited = false;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::max(timeout, std::chrono::milliseconds::zero());

        auto drain_output = [&] {
            char buffer[1024];
            for (;;) {
                const ssize_t count = read(output_fd, buffer, sizeof(buffer));
                if (count > 0) {
                    if (output.size() < 64 * 1024)
                        output.append(buffer, std::min<size_t>(static_cast<size_t>(count), 64 * 1024 - output.size()));
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                break;
            }
        };

        while (std::chrono::steady_clock::now() < deadline) {
            drain_output();
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                exited = true;
                break;
            }
            if (waited < 0 && errno != EINTR)
                break;

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd watched{output_fd, POLLIN | POLLHUP, 0};
            poll(&watched, 1, static_cast<int>(std::clamp<int64_t>(remaining.count(), 1, 50)));
        }

        if (!exited) {
            kill(child, SIGTERM);
            const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < grace_deadline) {
                const pid_t waited = waitpid(child, &status, WNOHANG);
                if (waited == child) {
                    exited = true;
                    break;
                }
                if (waited < 0 && errno != EINTR)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!exited) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
            }

            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            debug::log(WARN, "bluetooth: btmgmt info for {} timed out after {}ms", adapter_id, elapsed.count());
            close(output_fd);
            g_spawn_close_pid(child);
            return configured();
        }

        drain_output();
        close(output_fd);
        g_spawn_close_pid(child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return configured();

        const size_t settings = output.find("current settings:");
        if (settings == std::string::npos)
            return configured();
        const size_t line_end = output.find('\n', settings);
        const std::string line = output.substr(settings, line_end - settings);
        return line.find("secure-conn") != std::string::npos;
    }

    std::optional<bool> MonitorState::read_secure_connections(const std::string& adapter_id) {
        constexpr auto RECHECK_AFTER = std::chrono::seconds(30);
        if (adapter_id.empty()) {
            secure_adapter.clear();
            secure_connections.reset();
            secure_checked_at = {};
            return std::nullopt;
        }

        const auto now = std::chrono::steady_clock::now();
        if (adapter_id != secure_adapter || secure_checked_at == std::chrono::steady_clock::time_point{} ||
            now - secure_checked_at >= RECHECK_AFTER) {
            secure_adapter = adapter_id;
            secure_connections = probe_secure_connections(adapter_id);
            secure_checked_at = std::chrono::steady_clock::now();
        }
        return secure_connections;
    }

    void MonitorState::refresh() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            // Said by the name watch already while bluetoothd is away.
            if (bluez_present)
                debug::log(WARN, "bluetooth: GetManagedObjects failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        BluezObjects parsed = parse_managed_objects(reply);
        g_variant_unref(reply);
        std::string preferred;
        {
            std::lock_guard<std::mutex> lock(mutex);
            preferred = preferred_adapter_id;
        }
        Capability resolved = resolve_capability(parsed, std::nullopt, preferred);
        resolved = resolve_capability(
            parsed, read_secure_connections(resolved.adapter_id), preferred, l2cap_ertm_disabled());

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (parsed == objects && resolved == cap)
                return;
            objects = std::move(parsed);
            cap = std::move(resolved);
        }
        notify();
    }

    void MonitorState::run() {
        g_main_context_push_thread_default(context);

        for (const char* signal : {"InterfacesAdded", "InterfacesRemoved"}) {
            subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                       BLUEZ_NAME,
                                                                       OBJECT_MANAGER,
                                                                       signal,
                                                                       "/",
                                                                       nullptr,
                                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                                       on_bluez_signal,
                                                                       this,
                                                                       nullptr));
        }
        subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                   BLUEZ_NAME,
                                                                   "org.freedesktop.DBus.Properties",
                                                                   "PropertiesChanged",
                                                                   nullptr,
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_bluez_signal,
                                                                   this,
                                                                   nullptr));
        subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                   BLUEZ_NAME,
                                                                   nullptr,
                                                                   "Disconnected",
                                                                   nullptr,
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_disconnected,
                                                                   this,
                                                                   nullptr));

        // After the subscriptions: the callbacks are queued on this thread's
        // context and run from the loop below.
        name_watch = g_bus_watch_name_on_connection(
            conn, BLUEZ_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, on_bluez_appeared, on_bluez_vanished, this, nullptr);

        refresh();

        g_main_loop_run(loop);
        running = false;

        if (name_watch != 0) {
            g_bus_unwatch_name(name_watch);
            name_watch = 0;
        }
        for (guint id : subscriptions)
            g_dbus_connection_signal_unsubscribe(conn, id);
        subscriptions.clear();

        if (pending_refresh != 0) {
            if (GSource* s = g_main_context_find_source_by_id(context, pending_refresh))
                g_source_destroy(s);
            pending_refresh = 0;
        }

        g_main_context_pop_thread_default(context);
    }

    namespace {
        // Whether the bus knows how to start org.bluez: it is installed, and
        // only away for now.
        bool bluez_can_be_started(GDBusConnection* conn) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          "org.freedesktop.DBus",
                                                          "/org/freedesktop/DBus",
                                                          "org.freedesktop.DBus",
                                                          "ListActivatableNames",
                                                          nullptr,
                                                          G_VARIANT_TYPE("(as)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          2000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return false;
            }
            bool found = false;
            GVariantIter* names = nullptr;
            const gchar* name = nullptr;
            g_variant_get(reply, "(as)", &names);
            while (g_variant_iter_loop(names, "&s", &name))
                found = found || g_strcmp0(name, BLUEZ_NAME) == 0;
            g_variant_iter_free(names);
            g_variant_unref(reply);
            return found;
        }
    } // namespace

    BluezMonitor::BluezMonitor() : impl_(std::make_unique<MonitorState>()) {}

    BluezMonitor::~BluezMonitor() { stop(); }

    bool BluezMonitor::start() {
        if (impl_->running)
            return true;

        GError* error = nullptr;
        impl_->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!impl_->conn) {
            debug::log(WARN, "bluetooth: cannot reach the system bus: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        // fail fast when bluetoothd is not present
        GVariant* probe = g_dbus_connection_call_sync(impl_->conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!probe) {
            const bool installed = bluez_can_be_started(impl_->conn);
            debug::log(WARN,
                       "bluetooth: BlueZ unavailable: {}{}",
                       error ? error->message : "unknown",
                       installed ? "; waiting for bluetoothd to come to the bus" : "");
            g_clear_error(&error);
            // A machine without BlueZ has no Bluetooth side at all. One where
            // bluetoothd is merely between two lives (a crash, a restart, a
            // boot still under way) gets it as soon as the name is owned.
            if (!installed) {
                g_clear_object(&impl_->conn);
                return false;
            }
            impl_->bluez_present = false;
            impl_->objects = BluezObjects{};
        } else {
            impl_->objects = parse_managed_objects(probe);
            g_variant_unref(probe);
        }
        // Secure Connections stays unknown until the watcher thread's first
        // refresh: probing it here would fork btmgmt on the startup path.
        impl_->cap = resolve_capability(impl_->objects, std::nullopt, {}, l2cap_ertm_disabled());

        impl_->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl_->efd < 0) {
            debug::log(ERR, "bluetooth: eventfd() failed");
            g_clear_object(&impl_->conn);
            return false;
        }

        impl_->context = g_main_context_new();
        // Created here, not on the watcher thread: a stop() that lands before the
        // thread reaches g_main_loop_new would otherwise quit nothing, and the
        // join() below it would block forever.
        impl_->loop = g_main_loop_new(impl_->context, FALSE);
        impl_->running = true;
        impl_->thread = std::thread([this] { impl_->run(); });
        return true;
    }

    void BluezMonitor::stop() {
        if (impl_->thread.joinable()) {
            g_main_context_invoke(impl_->context, quit_loop, impl_.get());
            impl_->thread.join();
        }
        if (impl_->loop) {
            g_main_loop_unref(impl_->loop);
            impl_->loop = nullptr;
        }
        if (impl_->context) {
            g_main_context_unref(impl_->context);
            impl_->context = nullptr;
        }
        g_clear_object(&impl_->conn);
        if (impl_->efd >= 0) {
            close(impl_->efd);
            impl_->efd = -1;
        }
    }

    bool BluezMonitor::running() const { return impl_->running; }

    bool BluezMonitor::bluez_present() const { return impl_->bluez_present; }

    uint64_t BluezMonitor::bluez_generation() const { return impl_->bluez_generation; }

    int BluezMonitor::event_fd() const { return impl_->efd; }

    void BluezMonitor::drain() {
        if (impl_->efd < 0)
            return;
        uint64_t counter = 0;
        while (read(impl_->efd, &counter, sizeof(counter)) > 0) {
            // Drain every pending wakeup; the snapshot is read separately.
        }
    }

    GDBusConnection* BluezMonitor::connection() const { return impl_->conn; }

    void BluezMonitor::invoke_sync(const std::function<void()>& fn) {
        if (!impl_->context) {
            fn();
            return;
        }

        struct Job {
            const std::function<void()>* fn;
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } job{&fn, {}, {}, false};

        g_main_context_invoke_full(
            impl_->context,
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                auto* j = static_cast<Job*>(data);
                (*j->fn)();
                {
                    std::lock_guard<std::mutex> lock(j->mutex);
                    j->finished = true;
                }
                j->done.notify_all();
                return G_SOURCE_REMOVE;
            },
            &job,
            nullptr);

        std::unique_lock<std::mutex> lock(job.mutex);
        job.done.wait(lock, [&job] { return job.finished; });
    }

    BluezObjects BluezMonitor::snapshot() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->objects;
    }

    Capability BluezMonitor::capability() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->cap;
    }

    std::string BluezMonitor::last_disconnect_reason(const std::string& address) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& device : impl_->objects.devices) {
            if (device.address != address)
                continue;
            auto it = impl_->disconnect_reasons.find(device.path);
            return it == impl_->disconnect_reasons.end() ? std::string{} : it->second;
        }
        return {};
    }

    uint64_t BluezMonitor::le_drop_count(const std::string& device_path) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->le_drops.find(device_path);
        return it == impl_->le_drops.end() ? 0 : it->second;
    }

    uint64_t BluezMonitor::le_link_drop_count(const std::string& device_path) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->le_link_drops.find(device_path);
        return it == impl_->le_link_drops.end() ? 0 : it->second;
    }

    BluezMonitor::LeLinkUp BluezMonitor::le_link_up(const std::string& device_path) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->le_link_ups.find(device_path);
        return it == impl_->le_link_ups.end() ? LeLinkUp{} : it->second;
    }

    int64_t BluezMonitor::adapter_back_at_ms() const { return impl_->adapter_back_ms.load(); }

    void BluezMonitor::set_link_listener(std::function<void()> listener) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->link_listener = std::move(listener);
    }

    BluezMonitor::LeStranger BluezMonitor::le_stranger() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->le_stranger;
    }

    uint64_t BluezMonitor::auth_failure_count(const std::string& address) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& device : impl_->objects.devices) {
            if (device.address != address)
                continue;
            const auto count = [&](const std::map<std::string, uint64_t>& counts) {
                auto it = counts.find(device.path);
                return it == counts.end() ? uint64_t{0} : it->second;
            };
            return std::max(count(impl_->auth_failures_bearer), count(impl_->auth_failures_device));
        }
        return 0;
    }

    void BluezMonitor::clear_disconnect_reason(const std::string& address) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& device : impl_->objects.devices) {
            if (device.address == address)
                impl_->disconnect_reasons.erase(device.path);
        }
    }

    void BluezMonitor::set_preferred_adapter(std::string id) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->preferred_adapter_id == id)
                return;
            impl_->preferred_adapter_id = std::move(id);
        }

        if (impl_->running)
            invoke_sync([this] { impl_->refresh(); });
    }

    std::string BluezMonitor::preferred_adapter_id() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->preferred_adapter_id;
    }

    bool gatt_link_alive(GDBusConnection* conn, const std::string& characteristic_path) {
        if (!conn || characteristic_path.empty())
            return true;
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            conn,
            BLUEZ_NAME,
            characteristic_path.c_str(),
            "org.bluez.GattCharacteristic1",
            "ReadValue",
            g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            nullptr,
            &error);
        if (reply) {
            g_variant_unref(reply);
            return true;
        }
        const bool down = error && std::string(error->message).find("Not connected") != std::string::npos;
        g_clear_error(&error);
        return !down;
    }

} // namespace tether::bluetooth
