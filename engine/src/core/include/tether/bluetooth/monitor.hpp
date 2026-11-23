#pragma once

#include "tether/bluetooth/objects.hpp"

#include <chrono>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>

namespace tether::bluetooth {

    struct MonitorState;

    // Watches BlueZ on the system bus and keeps a current snapshot of its object tree.
    class BluezMonitor {
    public:
        BluezMonitor();
        ~BluezMonitor();

        BluezMonitor(const BluezMonitor&) = delete;
        BluezMonitor& operator=(const BluezMonitor&) = delete;

        // Connects to the system bus and starts the watcher thread. BlueZ object
        // and controller capability reads happen on that thread so daemon startup
        // and the network event loop never wait for Bluetooth tooling.
        bool start();
        void stop();
        bool running() const;

        // Level-triggered eventfd, readable whenever the snapshot has changed.
        // Register with EpollEventLoop::addFd and call drain() from the callback.
        int event_fd() const;
        void drain();

        BluezObjects snapshot() const;
        Capability capability() const;

        // Reason from the last org.bluez Disconnected signal for the device
        std::string last_disconnect_reason(const std::string& address) const;

        // Clears the last disconnect reason for the device, so that the next disconnect will be reported.
        void clear_disconnect_reason(const std::string& address);

        // Disconnected signals seen on the device's LE bearer or the whole device.
        // Only its change matters. Each one ends any GATT session on the link.
        uint64_t le_drop_count(const std::string& device_path) const;

        // Disconnected signals of the device's LE bearer alone: one per LE
        // link that ended, including those that came and went between two
        // looks at the snapshot. Only its change matters.
        uint64_t le_link_drop_count(const std::string& device_path) const;

        // The device's LE bearer was told Connected: how many times, and
        // when last, in milliseconds on the steady clock (-1 for never).
        // Dated when the signal is read, before the snapshot is.
        struct LeLinkUp {
            uint64_t count = 0;
            int64_t at_ms = -1;
        };
        LeLinkUp le_link_up(const std::string& device_path) const;

        // When the adapter last came back after BlueZ took it away (the
        // kernel resetting the controller, for one), in milliseconds on the
        // steady clock; -1 when it has not happened since start (#59).
        int64_t adapter_back_at_ms() const;

        // Called on the watcher thread each time an LE link comes up, once
        // the snapshot shows it, and each time one ends. It must not block.
        // An empty function removes it.
        void set_link_listener(std::function<void()> listener);

        // Disconnected signals whose reason is org.bluez.Reason.Authentication,
        // for the device at `address`. Only its change matters.
        uint64_t auth_failure_count(const std::string& address) const;

        // The last LE link that ended on a device BlueZ holds no pairing
        // for, as its Disconnected signal described it. `count` is how many
        // there have been; only its change matters.
        struct LeStranger {
            uint64_t count = 0;
            std::string path;
            // Empty when BlueZ had already removed the object.
            std::string address_type;
            std::string reason;
        };
        LeStranger le_stranger() const;

        // Whether bluetoothd owns org.bluez right now. While it does not the
        // snapshot is empty: nothing it described exists any more.
        bool bluez_present() const;

        // How many times bluetoothd came back after having left the bus, by a
        // crash or a restart. Only its change matters: everything this process
        // had registered with the old one (the advertisement, the GATT
        // subscriptions) is unknown to the new one.
        uint64_t bluez_generation() const;

        // Which controller to use: "hciN" or an address, empty for the first powered one.
        void set_preferred_adapter(std::string id);
        std::string preferred_adapter_id() const;

        // Shared system-bus connection. GDBusConnection is thread-safe for call.
        GDBusConnection* connection() const;

        // Runs fn on the watcher thread and waits for it to finish.
        void invoke_sync(const std::function<void()>& fn);

    private:
        std::unique_ptr<MonitorState> impl_;
    };

    extern BluezMonitor* g_bluez;

    // BlueZ exposes Secure Connections only through the management interface.
    // Runs btmgmt first, then falls back to TETHER_BLUEZ_SECURE_CONNECTIONS when
    // a container cannot use host management tools. Public so the process
    // boundary can be tested directly.
    std::optional<bool> probe_secure_connections(const std::string& adapter_id,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(1));

    // Whether the ATT link under a GATT characteristic is up.
    bool gatt_link_alive(GDBusConnection* conn, const std::string& characteristic_path);

} // namespace tether::bluetooth
