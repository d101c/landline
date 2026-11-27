#include "tether/bluetooth/connection.hpp"
#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/classic_demand.hpp"
#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/dial_plan.hpp"
#include "tether/bluetooth/groups.hpp"
#include "tether/bluetooth/incoming_call.hpp"
#include "tether/bluetooth/journal.hpp"
#include "tether/bluetooth/le_after_sleep.hpp"
#include "tether/bluetooth/le_conn_update.hpp"
#include "tether/bluetooth/le_after_reset.hpp"
#include "tether/bluetooth/le_first_use.hpp"
#include "tether/bluetooth/classic_stuck.hpp"
#include "tether/bluetooth/le_ghost.hpp"
#include "tether/bluetooth/le_health.hpp"
#include "tether/bluetooth/le_recovery.hpp"
#include "tether/bluetooth/le_strikes.hpp"
#include "tether/bluetooth/link_mode.hpp"
#include "tether/bluetooth/map_session.hpp"
#include "tether/bluetooth/message_sync_policy.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/pairing.hpp"
#include "tether/bluetooth/pbap_session.hpp"
#include "tether/bluetooth/pending_connect.hpp"
#include "tether/bluetooth/phone_state.hpp"
#include "tether/bluetooth/presence.hpp"
#include "tether/bluetooth/obex_transfer.hpp"
#include "tether/bluetooth/reconnect_policy.hpp"
#include "tether/bluetooth/removed_threads.hpp"
#include "tether/bluetooth/supervision_tick.hpp"
#include "tether/log.hpp"
#include "tether/net.hpp"
#include "tether/paths.hpp"
#include "tether/secret_store.hpp"
#include "tether/session_lock.hpp"
#include "tether/sleep_watch.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <mutex>
#include <string_view>
#include <thread>

namespace tether::bluetooth {

    ConnectionManager* g_bt_connections = nullptr;

    namespace {
        MessageStore g_messages;
        std::mutex g_messages_mutex;
        MessageJournal g_journal;
        std::mutex g_map_mutex;
        std::shared_ptr<MapSession> g_map_session;

        std::mutex g_group_mutex;
        GroupCorrelator g_correlator;
        GroupRoster g_rosters;
        std::map<std::string, GroupInfo> g_group_info;
        std::atomic<bool> g_group_replies_enabled{false};

        // MAP folders are navigated from the session root one level at a time.
        // The session parks on the message root and lists its children, so both
        // halves of a conversation can be pulled without walking between them.
        constexpr const char* MAP_MSG_ROOT = "telecom/msg";
        constexpr const char* MAP_OUTBOX = "telecom/msg/outbox";
        constexpr const char* MAP_FOLDER_INBOX = "inbox";
        constexpr const char* MAP_FOLDER_SENT = "sent";
    } // namespace

    namespace {
        bool roster_contradicted(const std::string& thread_key, const std::string& sender_key) {
            const std::vector<std::string>* roster = g_rosters.find(thread_key);
            if (!roster || sender_key.empty())
                return false;
            std::vector<Recipient> members;
            for (const auto& address : *roster) {
                Recipient recipient;
                std::string ignored;
                if (recipient_from_thread_key(address, recipient, ignored))
                    members.push_back(recipient);
            }
            return sender_invalidates_route(sender_key, members);
        }

        // Loads the saved list, keeping anything removed while it could not be
        // read. Caller holds g_messages_mutex.
        void restore_removed_threads() {
            RemovedThreads loaded = load_removed_threads();
            const bool unsaved = removed_threads().size() > 0;
            for (const auto& [key, cutoff] : removed_threads().cutoffs())
                loaded.remove(key, cutoff);
            removed_threads() = std::move(loaded);
            if (unsaved)
                save_removed_threads(removed_threads());
        }
    } // namespace

    MessageStore& message_store() { return g_messages; }
    std::mutex& message_store_mutex() { return g_messages_mutex; }

    namespace {
        // Returns the Send lease when the caller is done, however it leaves.
        struct SendLease {
            bool held = false;
            ~SendLease() {
                if (held && g_bt_connections)
                    g_bt_connections->release_lease(Lease::Send);
            }
        };

        std::shared_ptr<MapSession> current_map_session() {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            return g_map_session;
        }
    } // namespace

    bool send_message(const std::string& thread_key,
                      const std::string& body,
                      Message& sent_out,
                      std::string& err_out,
                      const SendProgressFn& progress) {
        if (body.empty()) {
            err_out = _("Nothing to send.");
            return false;
        }

        std::vector<Recipient> recipients;
        if (thread_key.rfind("group:", 0) == 0) {
            GroupInfo info;
            GroupRoster rosters;
            {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                auto found = g_group_info.find(thread_key);
                if (found != g_group_info.end())
                    info = found->second;
                rosters = g_rosters;
            }
            ReplyEligibility eligibility;
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                eligibility = resolve_group_recipients(
                    info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, err_out);
            }
            if (eligibility != ReplyEligibility::Allowed)
                return false;
        } else {
            Recipient recipient;
            if (!recipient_from_thread_key(thread_key, recipient, err_out))
                return false;
            recipients.push_back(recipient);
        }

        std::shared_ptr<MapSession> session = current_map_session();
        // The lean link mode: the classic link is closed at rest. The Send
        // lease opens it, or keeps it if it is there, and the session is
        // waited for.
        SendLease lease;
        if (g_bt_connections && g_bt_connections->on_demand()) {
            lease.held = g_bt_connections->take_lease(Lease::Send);
            if (!session) {
                if (progress)
                    progress("connecting");
                session = g_bt_connections->await_messages(SEND_LINK_TIMEOUT_SECONDS, err_out);
                if (!session)
                    return false;
            }
        }
        if (!session) {
            err_out = _("Messages are not connected.");
            return false;
        }
        if (progress)
            progress("sending");

        const std::string bmessage = build_bmessage(recipients, body);
        if (bmessage.empty()) {
            err_out = _("Could not build a message for this conversation.");
            return false;
        }

        if (!session->push_message(MAP_OUTBOX, bmessage, err_out))
            return false;
        // The phone's own copy lands in its sent folder a moment later.
        if (g_bt_connections)
            g_bt_connections->request_message_sync("sent");

        Message sent;
        sent.thread_key = thread_key;
        sent.peer_address = recipients.size() == 1 ? recipients.front().address : std::string{};
        sent.body = body;
        sent.timestamp = static_cast<int64_t>(std::time(nullptr));
        sent.outgoing = true;
        sent.read = true;
        sent.folder = "outbox";

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            sent.handle =
                LOCAL_HANDLE_PREFIX + std::to_string(std::time(nullptr)) + "-" + std::to_string(g_messages.size());
            g_messages.add(sent);
            g_journal.append(sent);
        }
        sent_out = sent;
        return true;
    }

    void observe_message_notification(const std::string& sender,
                                      const std::string& subtitle,
                                      const std::string& body,
                                      int64_t now) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_correlator.observe({sender, subtitle, body, now});
        g_correlator.expire(now);
    }

    ReplyEligibility group_reply_status(const std::string& thread_key, std::string& reason) {
        GroupInfo info;
        GroupRoster rosters;
        {
            std::lock_guard<std::mutex> lock(g_group_mutex);
            auto found = g_group_info.find(thread_key);
            if (found != g_group_info.end())
                info = found->second;
            rosters = g_rosters;
        }
        std::vector<Recipient> recipients;
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        return resolve_group_recipients(
            info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, reason);
    }

    void discard_retained_messages() {
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        g_journal.close();
        g_messages = MessageStore();
        contact_store() = ContactStore();
    }

    void move_retained_store(Retention previous, Retention next) {
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        g_journal.close();
        migrate_contacts(previous, next);
        migrate_call_history(previous, next);
        if (!migrate_removed_threads(previous, next) && removed_threads().size() > 0)
            save_removed_threads(removed_threads());
    }

    void set_group_replies_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_group_replies_enabled = enabled;
    }

    void reload_group_rosters() {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_rosters = load_rosters();
        // re-reading the file is the user's answer to an invalidation.
        for (auto& [key, info] : g_group_info)
            info.route_invalidated = false;
    }

    bool mark_message_read(const std::string& handle, bool read, std::string& err_out, bool* synced_out) {
        if (synced_out)
            *synced_out = false;

        // The stored path is what obexd currently answers on. A message replayed
        // from the journal has none, and one that has scrolled out of the listing
        // window never gets another. The desktop's own read state must still move
        // for those, or the unread badge can never reach zero.
        std::string object_path;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            if (const Message* message = g_messages.find(handle)) {
                known = true;
                object_path = message->object_path;
            }
        }
        if (!known) {
            err_out = _("That message is no longer in the conversation history.");
            return false;
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }

        if (session && !object_path.empty()) {
            std::string sync_err;
            if (session->set_read(object_path, read, sync_err)) {
                if (synced_out)
                    *synced_out = true;
            } else {
                err_out = sync_err;
            }
        } else {
            err_out = _("The iPhone is not serving this message right now; marked read on this computer only.");
        }

        std::lock_guard<std::mutex> lock(g_messages_mutex);
        if (g_messages.set_read(handle, read)) {
            // Re-append so the state survives a restart. Retention keeps the last
            // line for a handle, so the newer read state wins on the next load.
            if (const Message* updated = g_messages.find(handle))
                g_journal.append(*updated);
        }
        return true;
    }

    void mark_messages_read_async(std::vector<std::string> handles, bool read) {
        if (handles.empty())
            return;

        std::thread([handles = std::move(handles), read]() {
            int changed = 0;
            int synced = 0;
            std::string last_err;
            for (const auto& handle : handles) {
                std::string err;
                bool handle_synced = false;
                if (mark_message_read(handle, read, err, &handle_synced))
                    ++changed;
                if (handle_synced)
                    ++synced;
                if (!err.empty())
                    last_err = err;
            }
            nlohmann::json event;
            event["command"] = "bt_message_read";
            event["handles"] = handles;
            event["read"] = read;
            event["success"] = changed > 0;
            event["synced"] = synced;
            if (!last_err.empty())
                event["message"] = last_err;
            broadcast_local_event(event.dump());
        }).detach();
    }

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_BEARER_LE = "org.bluez.Bearer.LE1";
        constexpr const char* IFACE_BEARER_BREDR = "org.bluez.Bearer.BREDR1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";
        // The iPhone's side of hands-free. Device1.ConnectProfile takes the
        // remote UUID; the local one (111e) answers "no more profiles".
        constexpr const char* UUID_HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb";
        constexpr int PROFILE_REQUEST_TIMEOUT_MS = 15000;

        // Turns the adapter off and, a second later, on again (three tries), off the
        // caller's thread (#59). BlueZ lets the session's user do it.
        void restart_adapter(GDBusConnection* conn, const std::string& path) {
            if (!conn || path.empty())
                return;
            g_object_ref(conn);
            std::thread([conn, path] {
                auto set_powered = [&](bool on) -> bool {
                    GError* error = nullptr;
                    GVariant* reply = g_dbus_connection_call_sync(
                        conn, BLUEZ_NAME, path.c_str(), IFACE_PROPS, "Set",
                        g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(on)), nullptr,
                        G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &error);
                    if (reply)
                        g_variant_unref(reply);
                    else
                        debug::log(WARN, "bluetooth: could not turn {} {}: {}", path, on ? "on" : "off",
                                   error ? error->message : "unknown error");
                    g_clear_error(&error);
                    return reply != nullptr;
                };
                if (set_powered(false)) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    // Never left off: tried again, and said loudly if it stays so.
                    bool on = false;
                    for (int attempt = 0; attempt < 3 && !on; ++attempt) {
                        if (attempt > 0)
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                        on = set_powered(true);
                    }
                    if (!on)
                        debug::log(ERR, "bluetooth: {} was turned off and could not be turned on again", path);
                }
                g_object_unref(conn);
            }).detach();
        }

        constexpr const char* OBEX_NAME = "org.bluez.obex";
        constexpr const char* OBEX_ROOT = "/org/bluez/obex";
        constexpr const char* OBEX_CLIENT = "org.bluez.obex.Client1";

        constexpr int CONNECT_TIMEOUT_MS = 30000;
        // BlueZ keeps working on a connect long after a caller gives up, and every
        // later attempt then collides with it. The LE connect is issued
        // asynchronously and left to run to completion rather than abandoned, so
        // this is generous on purpose.
        constexpr int LE_CONNECT_TIMEOUT_MS = 45000;

        // LANDLINE_MESSAGE_POLL_SECONDS: how often the inbox is listed with
        // nothing asking for it, 5 to 3600. Unset, message_sync_policy.hpp
        // decides: an hour behind working notifications, 15 s without them.
        int message_poll_override() {
            static const int seconds = message_poll_override_from(std::getenv("LANDLINE_MESSAGE_POLL_SECONDS"));
            return seconds;
        }
        // obexd can drop a MAP session while the Classic link stays up, and the
        // profile supervisor short-circuits once map_open is set, so nothing else
        // notices. Consecutive listing failures are the cheapest liveness signal.
        constexpr int MAP_FAILURES_BEFORE_REOPEN = 3;

        // The OBEX session object is gone, or its transport died under it.
        auto map_session_dead = [](const std::string& err) {
            return err.find("UnknownObject") != std::string::npos ||
                   err.find("Transport got disconnected") != std::string::npos ||
                   err.find("Transport is not connected") != std::string::npos;
        };
        constexpr int MESSAGE_LIST_MAX = 200;
        // Bodies are fetched one OBEX Get at a time on the connection thread,
        // which also has ANCS and the bearer supervisor. Needs cap to keep the
        // first listing of a full mailbox from stalling all of it
        constexpr size_t MESSAGE_BODY_FETCH_MAX = 20;
        // What bt_retry_profiles holds the link for in the lean link mode.
        constexpr int RETRY_LEASE_SECONDS = 30;
        // How long after the start "this controller cannot carry LE" is not
        // yet taken for an answer.
        constexpr int LINK_MODE_SETTLE_SECONDS = 10;
        constexpr int SUPERVISOR_TICK_SECONDS = 1;
        // The pause between two ticks while a call is in hand.
        constexpr int CALL_TICK_MS = 250;
        // A step that took this long has held the supervising thread: the
        // links are looked at again before anything else is done.
        constexpr int LONG_STEP_MS = 250;

        // Whether the controller can carry ANCS right now. Re-read rather than
        // latched: see the comment in ConnectionManager::start().
        bool ancs_available() { return g_bluez && g_bluez->capability().mode == DeliveryMode::Full; }

        std::string normalize_address(std::string address) {
            for (char& c : address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return address;
        }

        // Percent from the LE Battery Service, else HFP's 0-5 level, else unknown.
        BatteryReading
            phone_battery(const BluezMonitor* monitor, const std::string& address, const nlohmann::json& calls) {
            if (monitor) {
                const std::string wanted = normalize_address(address);
                for (const auto& device : monitor->snapshot().devices)
                    if (device.battery >= 0 && normalize_address(device.address) == wanted)
                        return {device.battery, "le"};
            }
            if (calls.is_object() && calls.value("available", false) && calls.value("indicators", true) &&
                calls.value("battery", 0) > 0)
                return {calls.value("battery", 0) * 20, "hfp"};
            return {};
        }

        // Bearer operations backed by BlueZ on the system bus.
        class BluezBearerOps : public BearerOps {
        public:
            BluezBearerOps(BluezMonitor& monitor, std::string address)
                : monitor_(monitor), address_(normalize_address(std::move(address))) {}

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            bool device_present() const override { return lookup().has_value(); }

            bool device_paired() const override {
                auto device = lookup();
                return device && device->paired;
            }

            bool classic_connected() const override {
                auto device = lookup();
                const bool up = device && device->classic_link_up();
                if (up)
                    classic_->discard_error();
                return up;
            }

            bool le_bearer_available() const override {
                auto device = lookup();
                return device && device->has_le_bearer;
            }

            bool le_connected() const override {
                auto device = lookup();
                return device && link_up(*device);
            }

            std::string preferred_bearer() const override {
                auto device = lookup();
                return device ? device->preferred_bearer : std::string();
            }

            void set_preferred_bearer(const std::string& bearer) override {
                auto device = lookup();
                if (!device)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(
                    monitor_.connection(),
                    BLUEZ_NAME,
                    device->path.c_str(),
                    IFACE_PROPS,
                    "Set",
                    g_variant_new("(ssv)", IFACE_DEVICE, "PreferredBearer", g_variant_new_string(bearer.c_str())),
                    nullptr,
                    G_DBUS_CALL_FLAGS_NONE,
                    5000,
                    nullptr,
                    &error);
                if (reply)
                    g_variant_unref(reply);
                // Absent on older BlueZ; not having it is not an error.
                g_clear_error(&error);
            }

            // Issued asynchronously, like the LE connect: a synchronous call kept
            // the supervising thread, and with it notifications, calls and the
            // published status, waiting on BlueZ for up to CONNECT_TIMEOUT_MS.
            // TETHER_SYNC_CLASSIC_CONNECT=1 brings the blocking call back.
            ConnectResult connect_classic(std::string& err) override {
                if (sync_classic_connect())
                    return connect_classic_sync(err);

                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                const ConnectResult begun = classic_->begin(err);
                if (begun != ConnectResult::Requested)
                    return begun;

                // bluez only exposes the per-bearer Connect while the device is connected on some bearer
                auto* request = new ClassicCall{classic_, le_, device->path, device->has_classic_bearer && device->connected};
                request->busy = classic_busy_;
                request->pin_bredr = !link_up(*device);
                // Never over an LE dial in flight: the write aborts it.
                if (!request->via_bearer && request->pin_bredr && !le_connect_outstanding())
                    set_preferred_bearer("bredr");
                debug::log(INFO,
                           "bluetooth: asking BlueZ for the BR/EDR link ({})",
                           request->via_bearer ? "Bearer.BREDR1.Connect" : "Device1.Connect");
                monitor_.invoke_sync([this, request] { issue_classic(monitor_.connection(), request); });
                return ConnectResult::Requested;
            }

            // The lean link mode: the LE link is up, so the per-bearer call
            // exists, and it is the only one made. No Device1.Connect behind
            // it, no PreferredBearer.
            ConnectResult connect_classic_on_demand(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                if (!device->has_classic_bearer) {
                    err = "no BR/EDR bearer";
                    return ConnectResult::Failed;
                }
                const ConnectResult begun = classic_->begin(err);
                if (begun != ConnectResult::Requested)
                    return begun;
                auto* request = new ClassicCall{classic_, le_, device->path, true};
                request->bearer_only = true;
                request->busy = classic_busy_;
                debug::log(INFO, "bluetooth: asking BlueZ for the BR/EDR link on demand (Bearer.BREDR1.Connect)");
                monitor_.invoke_sync([this, request] { issue_classic(monitor_.connection(), request); });
                return ConnectResult::Requested;
            }

            // An A2DP stream from the iPhone playing here, not merely
            // connected: BlueZ's transport is pending (the iPhone started it)
            // or active (PipeWire took it). A connected stream sits idle (#43).
            bool audio_playing() const {
                auto device = lookup();
                if (!device)
                    return false;
                const auto snapshot = monitor_.snapshot();
                const MediaTransport* transport = snapshot.find_sink_transport(device->path);
                return transport && (transport->state == "pending" || transport->state == "active");
            }

            // Whether the phone's network profile is connected: NetworkManager
            // is using the link for tethering, and it is not ours to close.
            bool tethering_active() const {
                auto device = lookup();
                if (!device)
                    return false;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(monitor_.connection(),
                                                              BLUEZ_NAME,
                                                              device->path.c_str(),
                                                              IFACE_PROPS,
                                                              "Get",
                                                              g_variant_new("(ss)", "org.bluez.Network1", "Connected"),
                                                              G_VARIANT_TYPE("(v)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              1000,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    g_clear_error(&error);
                    return false;
                }
                GVariant* boxed = nullptr;
                g_variant_get(reply, "(v)", &boxed);
                const bool connected = boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_BOOLEAN) &&
                                       g_variant_get_boolean(boxed) != FALSE;
                if (boxed)
                    g_variant_unref(boxed);
                g_variant_unref(reply);
                return connected;
            }

            void forget_classic_refusal() override { classic_->discard_error(); }

            // #64: "In Progress" answers in a row to the per-bearer connect.
            int classic_busy_answers() const { return classic_busy_->load(); }
            void forget_classic_busy() { *classic_busy_ = 0; }

            // #64, second half: Device1.Connect, the path that makes BlueZ
            // answer and drop the request its BR/EDR bearer kept. False when
            // a connect of ours is still in flight; asked again next pass.
            bool reconnect_through_device() {
                auto device = lookup();
                if (!device)
                    return false;
                classic_->discard_error();
                std::string ignored;
                if (classic_->begin(ignored) != ConnectResult::Requested)
                    return false;
                auto* request = new ClassicCall{classic_, le_, device->path, false};
                monitor_.invoke_sync([this, request] { issue_classic(monitor_.connection(), request); });
                return true;
            }

            // One profile, over the link as it stands. Asynchronous: the answer
            // is only worth a line in the journal, the telephony object showing
            // up is what counts.
            bool request_hands_free() override {
                auto device = lookup();
                if (!device)
                    return false;
                const std::string path = device->path;
                monitor_.invoke_sync([this, path] {
                    g_dbus_connection_call(monitor_.connection(),
                                           BLUEZ_NAME,
                                           path.c_str(),
                                           IFACE_DEVICE,
                                           "ConnectProfile",
                                           g_variant_new("(s)", UUID_HFP_AG),
                                           nullptr,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           PROFILE_REQUEST_TIMEOUT_MS,
                                           nullptr,
                                           on_hands_free_requested,
                                           nullptr);
                });
                return true;
            }

            ConnectResult connect_classic_sync(std::string& err) {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                // bluez only exposes the per-bearer Connect while the device is connected on some bearer
                std::string bearer_err;
                if (device->has_classic_bearer && device->connected) {
                    if (call(device->path, IFACE_BEARER_BREDR, "Connect", bearer_err))
                        return ConnectResult::Requested;
                    if (bearer_err.find("InProgress") != std::string::npos ||
                        bearer_err.find("AlreadyConnected") != std::string::npos) {
                        err = bearer_err;
                        return ConnectResult::Failed;
                    }
                }

                // Never over an LE dial in flight: the write aborts it.
                if (!link_up(*device) && !le_connect_outstanding())
                    set_preferred_bearer("bredr");
                if (call(device->path, IFACE_DEVICE, "Connect", err))
                    return ConnectResult::Requested;
                // per-bearer refusal is the more specific one
                if (!bearer_err.empty())
                    err = bearer_err;
                return ConnectResult::Failed;
            }

            bool le_connect_outstanding() const override {
                std::lock_guard<std::mutex> lock(le_->mutex);
                return le_->pending;
            }

            bool classic_connect_outstanding() const override { return classic_->pending(); }

            void set_telephony_probe(std::function<bool()> probe) { telephony_probe_ = std::move(probe); }
            // Whether a call is in progress, as the caller knows it.
            void set_call_probe(std::function<bool()> probe) { call_probe_ = std::move(probe); }

            bool phone_offers_ancs() const override {
                auto device = lookup();
                return device && device->supports_ancs();
            }

            bool phone_busy() const override {
                if (call_probe_ && call_probe_())
                    return true;
                auto device = lookup();
                // An open A2DP stream: the phone's sound is coming here.
                return device && monitor_.snapshot().find_sink_transport(device->path) != nullptr;
            }

            // Device1.Disconnect: both halves, and asynchronously. The links
            // going down are what counts; the answer is only worth a line.
            bool disconnect_all(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return false;
                }
                const std::string path = device->path;
                monitor_.invoke_sync([this, path] {
                    g_dbus_connection_call(monitor_.connection(),
                                           BLUEZ_NAME,
                                           path.c_str(),
                                           IFACE_DEVICE,
                                           "Disconnect",
                                           nullptr,
                                           nullptr,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           PROFILE_REQUEST_TIMEOUT_MS,
                                           nullptr,
                                           on_all_disconnected,
                                           nullptr);
                });
                return true;
            }

            bool telephony_present() const override {
                if (telephony_probe_)
                    return telephony_probe_();
                auto device = lookup();
                return device && monitor_.snapshot().find_telephony(device->path) != nullptr;
            }

            bool disconnect_classic(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return false;
                }
                // Only the per-bearer call: Device1.Disconnect() would take the
                // LE half and the ANCS subscription riding on it.
                if (!device->has_classic_bearer) {
                    err = "no BR/EDR bearer";
                    return false;
                }
                return call(device->path, IFACE_BEARER_BREDR, "Disconnect", err);
            }

            BondFacts bond_facts() const override {
                auto device = lookup();
                return device ? tether::bluetooth::bond_facts(*device) : BondFacts{};
            }

            uint64_t auth_failure_count() const override {
                auto device = lookup();
                return device ? monitor_.auth_failure_count(device->address) : 0;
            }

            bool solicitation_on_air() const override {
                if (!ancs_solicitation_active())
                    return false;
                const auto objects = monitor_.snapshot();
                auto device = lookup();
                const Adapter* adapter = device ? objects.find_adapter(device->adapter_path) : nullptr;
                return adapter && adapter->advertising_active_instances > 0;
            }

            // Issued asynchronously and never abandoned: a synchronous call that
            // times out leaves BlueZ still connecting, and the next attempt then
            // collides with it forever.
            ConnectResult connect_le(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return ConnectResult::Failed;
                }
                if (!device->has_le_bearer) {
                    err = "no LE bearer";
                    return ConnectResult::Failed;
                }

                {
                    std::lock_guard<std::mutex> lock(le_->mutex);
                    if (le_->pending)
                        return ConnectResult::Busy;
                    if (!le_->error.empty()) {
                        err = le_->error;
                        le_->error.clear();
                        return ConnectResult::Failed;
                    }
                    le_->pending = true;
                }

                // Some BlueZ builds publish Bearer.LE1 without Connect; the first
                // reply says which, and every later attempt uses the one that
                // exists.
                if (le_->bearer_connect_missing)
                    set_preferred_bearer("le");
                const char* iface = le_->bearer_connect_missing ? IFACE_DEVICE : IFACE_BEARER_LE;
                auto* holder = new std::shared_ptr<LeConnect>(le_);
                const std::string path = device->path;
                monitor_.invoke_sync([this, path, iface, holder] {
                    g_dbus_connection_call(monitor_.connection(),
                                           BLUEZ_NAME,
                                           path.c_str(),
                                           iface,
                                           "Connect",
                                           nullptr,
                                           nullptr,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           LE_CONNECT_TIMEOUT_MS,
                                           nullptr,
                                           on_le_connected,
                                           holder);
                });
                return ConnectResult::Requested;
            }

        private:
            // Outlives the ops object: a reply can land after the supervised
            // device is torn down and replaced.
            struct LeConnect {
                mutable std::mutex mutex;
                bool pending = false;
                std::string error;
                bool bearer_connect_missing = false;
            };
            std::shared_ptr<LeConnect> le_ = std::make_shared<LeConnect>();

            // Outlives the ops object for the same reason.
            std::shared_ptr<PendingConnect> classic_ = std::make_shared<PendingConnect>();

            // #64: how many times in a row BlueZ answered "In Progress" to
            // Bearer.BREDR1.Connect. Written where the answers land.
            std::shared_ptr<std::atomic<int>> classic_busy_ = std::make_shared<std::atomic<int>>(0);

            // One BR/EDR connect in flight, owned by its D-Bus callback.
            struct ClassicCall {
                std::shared_ptr<PendingConnect> state;
                std::shared_ptr<LeConnect> le;
                std::string path;
                // Bearer.BREDR1.Connect first; Device1.Connect when that is refused.
                bool via_bearer = false;
                bool pin_bredr = false;
                std::string bearer_error;
                // The lean link mode: never Device1.Connect behind a refusal.
                bool bearer_only = false;
                // #64: "In Progress" answers to the per-bearer call, in a row.
                std::shared_ptr<std::atomic<int>> busy;
            };

            static bool sync_classic_connect() {
                static const bool enabled = [] {
                    const char* value = std::getenv("LANDLINE_SYNC_CLASSIC_CONNECT");
                    return value && std::string_view(value) == "1";
                }();
                return enabled;
            }

            // Runs on the monitor's GLib thread, which is where the reply lands.
            static void issue_classic(GDBusConnection* connection, ClassicCall* request) {
                g_dbus_connection_call(connection,
                                       BLUEZ_NAME,
                                       request->path.c_str(),
                                       request->via_bearer ? IFACE_BEARER_BREDR : IFACE_DEVICE,
                                       "Connect",
                                       nullptr,
                                       nullptr,
                                       G_DBUS_CALL_FLAGS_NONE,
                                       CONNECT_TIMEOUT_MS,
                                       nullptr,
                                       on_classic_connected,
                                       request);
            }

            static void on_classic_connected(GObject* source, GAsyncResult* result, gpointer user_data) {
                std::unique_ptr<ClassicCall> request(static_cast<ClassicCall*>(user_data));
                GDBusConnection* connection = G_DBUS_CONNECTION(source);
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(connection, result, &error);
                const bool ok = reply != nullptr;
                const bool timed_out = !ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
                const std::string message = ok ? std::string() : error ? error->message : "unknown error";
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);

                // #64: an unanswered call says nothing either way.
                if (request->via_bearer && request->busy && !timed_out) {
                    if (!ok && message.find("InProgress") != std::string::npos)
                        ++*request->busy;
                    else
                        *request->busy = 0;
                }
                const ClassicReply verdict =
                    classify_classic_reply(request->via_bearer, ok, timed_out, message, request->bearer_error);
                if (verdict.fall_back && request->bearer_only) {
                    debug::log(INFO, "bluetooth: BlueZ refused the BR/EDR connect on demand: {}", message);
                    request->state->finish(message);
                    return;
                }
                if (verdict.fall_back) {
                    request->via_bearer = false;
                    request->bearer_error = message;
                    bool le_dialling = false;
                    {
                        std::lock_guard<std::mutex> lock(request->le->mutex);
                        le_dialling = request->le->pending;
                    }
                    // Queued ahead of the connect on the same connection, and
                    // never over an LE dial in flight: the write aborts it.
                    if (request->pin_bredr && !le_dialling)
                        g_dbus_connection_call(
                            connection,
                            BLUEZ_NAME,
                            request->path.c_str(),
                            IFACE_PROPS,
                            "Set",
                            g_variant_new("(ssv)", IFACE_DEVICE, "PreferredBearer", g_variant_new_string("bredr")),
                            nullptr,
                            G_DBUS_CALL_FLAGS_NONE,
                            5000,
                            nullptr,
                            nullptr,
                            nullptr);
                    issue_classic(connection, request.release());
                    return;
                }

                if (timed_out)
                    debug::log(INFO,
                               "bluetooth: BR/EDR connect unanswered after {}s; BlueZ is still working on it",
                               CONNECT_TIMEOUT_MS / 1000);
                else
                    debug::log(INFO, "bluetooth: BlueZ answered the BR/EDR connect: {}", ok ? "accepted" : message);
                request->state->finish(verdict.error);
            }

            static void on_all_disconnected(GObject* source, GAsyncResult* result, gpointer) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                if (reply) {
                    g_variant_unref(reply);
                    debug::log(INFO, "bluetooth: BlueZ dropped the links to the iPhone, as asked");
                } else {
                    debug::log(INFO,
                               "bluetooth: BlueZ did not drop the links to the iPhone ({})",
                               error ? error->message : "unknown error");
                    g_clear_error(&error);
                }
            }

            static void on_hands_free_requested(GObject* source, GAsyncResult* result, gpointer) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                if (reply) {
                    g_variant_unref(reply);
                    debug::log(INFO, "bluetooth: BlueZ accepted the hands-free request");
                } else {
                    debug::log(INFO,
                               "bluetooth: BlueZ declined the hands-free request ({})",
                               error ? error->message : "unknown error");
                    g_clear_error(&error);
                }
            }

            static void on_le_connected(GObject* source, GAsyncResult* result, gpointer user_data) {
                std::unique_ptr<std::shared_ptr<LeConnect>> holder(static_cast<std::shared_ptr<LeConnect>*>(user_data));
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                std::string message;
                if (reply)
                    g_variant_unref(reply);
                else {
                    message = error ? error->message : "unknown error";
                    g_clear_error(&error);
                }

                std::lock_guard<std::mutex> lock((*holder)->mutex);
                (*holder)->pending = false;
                // Not a failure of the connect, just the wrong interface. Retry on
                // the next tick against Device1 and report nothing.
                if (message.find("UnknownMethod") != std::string::npos)
                    (*holder)->bearer_connect_missing = true;
                else
                    (*holder)->error = message;
            }

            std::optional<Device> lookup() const {
                for (const auto& device : monitor_.snapshot().devices) {
                    if (normalize_address(device.address) == address_)
                        return device;
                }
                return std::nullopt;
            }

            // A BR/EDR reconnect sets ServicesResolved again while the stale ANCS
            // Notifying flag survives, so the inferred LE link is confirmed with a read.
            bool link_up(const Device& device) const {
                if (device.le_connected)
                    return true;
                return device.le_link_up() && gatt_link_alive(monitor_.connection(), device.gap_name_path);
            }

            bool call(const std::string& path,
                      const char* iface,
                      const char* method,
                      std::string& err,
                      int timeout_ms = CONNECT_TIMEOUT_MS) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(monitor_.connection(),
                                                              BLUEZ_NAME,
                                                              path.c_str(),
                                                              iface,
                                                              method,
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              timeout_ms,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return false;
                }
                g_variant_unref(reply);
                return true;
            }

            BluezMonitor& monitor_;
            std::string address_;
            std::function<bool()> telephony_probe_;
            std::function<bool()> call_probe_;
        };

        // OBEX sessions live on the session bus, not the system bus.
        class ObexProfileOps : public ProfileOps {
        public:
            explicit ObexProfileOps(std::string address) : address_(normalize_address(std::move(address))) {}

            ~ObexProfileOps() override { g_clear_object(&conn_); }

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            // The MAP session driver reuses this connection rather than opening a
            // second one to the same bus.
            GDBusConnection* bus() const { return conn_; }

            // The connection was there and is closed. GDBus hands out the same
            // closed one again, so this does not recover within the process.
            bool bus_closed() const { return conn_ && g_dbus_connection_is_closed(conn_); }

            std::string create_session(const std::string& target, std::string& err) override {
                if (!ensure_connection(err))
                    return {};

                GVariantBuilder args;
                g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&args, "{sv}", "Target", g_variant_new_string(target.c_str()));

                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "CreateSession",
                                                              g_variant_new("(sa{sv})", address_.c_str(), &args),
                                                              G_VARIANT_TYPE("(o)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              // Kept at 30 s: a real OBEX connect to the
                                                              // phone (page, SDP, L2CAP, OBEX Connect) takes
                                                              // seconds, and a dead obexd is now caught by
                                                              // the ping first (ProfileSupervisor::daemon_ready).
                                                              CONNECT_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return {};
                }

                const gchar* path = nullptr;
                g_variant_get(reply, "(&o)", &path);
                std::string session = path ? path : "";
                g_variant_unref(reply);
                return session;
            }

            // Peer.Ping is answered by obexd's bus library as soon as its main
            // loop runs. NO_AUTO_START: an absent obexd is reported, not started
            // behind systemd's back with the caller waiting on it.
            ObexDaemonState probe_daemon() override {
                std::string err;
                if (!ensure_connection(err))
                    return ObexDaemonState::Unknown;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              "org.freedesktop.DBus.Peer",
                                                              "Ping",
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                              OBEX_PING_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (reply) {
                    g_variant_unref(reply);
                    return ObexDaemonState::Alive;
                }
                const ObexDaemonState state = classify_obex_ping(error ? error->message : "unknown error");
                if (state == ObexDaemonState::Unknown)
                    debug::log(DEBUG, "bluetooth: obexd ping failed: {}", error ? error->message : "unknown error");
                g_clear_error(&error);
                return state;
            }

            bool start_daemon(std::string& err) override { return unit_job("StartUnit", err); }
            bool restart_daemon(std::string& err) override { return unit_job("RestartUnit", err); }

            void remove_session(const std::string& path) override {
                if (path.empty() || !conn_)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "RemoveSession",
                                                              g_variant_new("(o)", path.c_str()),
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              5000,
                                                              nullptr,
                                                              &error);
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
            }

        private:
            // The user's systemd queues the job and answers with its path at
            // once: nothing here waits for obexd itself.
            bool unit_job(const char* method, std::string& err) {
                if (!ensure_connection(err))
                    return false;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              "org.freedesktop.systemd1",
                                                              "/org/freedesktop/systemd1",
                                                              "org.freedesktop.systemd1.Manager",
                                                              method,
                                                              g_variant_new("(ss)", OBEX_UNIT, "replace"),
                                                              G_VARIANT_TYPE("(o)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              OBEX_PING_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return false;
                }
                g_variant_unref(reply);
                return true;
            }

            bool ensure_connection(std::string& err) {
                if (conn_)
                    return true;
                GError* error = nullptr;
                conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
                if (!conn_) {
                    err = std::string("session bus unavailable: ") + (error ? error->message : "unknown");
                    g_clear_error(&error);
                    return false;
                }
                return true;
            }

            GDBusConnection* conn_ = nullptr;
            std::string address_;
        };

    } // namespace

    struct ConnectionState {
        BluezMonitor* monitor = nullptr;
        ConnectionManager::StatusFn on_change;
        ConnectionManager::MessageFn on_message;

        std::unique_ptr<BluezBearerOps> bearer_ops;
        std::unique_ptr<ObexProfileOps> profile_ops;
        std::unique_ptr<BearerSupervisor> bearers;
        std::unique_ptr<ProfileSupervisor> profiles;

        std::thread thread;
        std::atomic<bool> running{false};
        std::mutex mutex;
        std::condition_variable wake;

        // Serializes start/stop/set_device
        std::mutex lifecycle;

        std::string address;
        // What the user asked for, versus what the controller can currently do.
        bool ancs_preference = true;
        bool ancs_enabled = true;
        bool link_was_ready = false;
        bool device_was_present = false;
        // Whether losing the phone to a supervision timeout locks the session
        std::atomic<bool> lock_on_away{false};
        std::atomic<int> lock_away_seconds{AWAY_LOCK_GRACE_SECONDS};
        // Profiles released by hand, for another computer to use. Not persisted.
        std::atomic<bool> hold_map{false};
        std::atomic<bool> hold_pbap{false};
        std::atomic<bool> hold_ancs{false};
        // Whether a locked or idle session releases MAP and PBAP.
        std::atomic<bool> yield_when_inactive{false};
        // Whether the phone's return after an away lock asks for the login screen to light up.
        std::atomic<bool> wake_on_return{false};
        // Here or away, why, and what the away lock did. Supervisor thread only.
        PresenceTracker presence;
        // Each low-battery threshold once per crossing. Supervisor thread only.
        BatteryWatch battery_watch;
        // Whether the phone is on charge, from the way its level moves.
        ChargeWatch charge_watch;
        // The phone the two above are about.
        std::string watched_address;
        // Events other than the status: bt_presence, bt_battery_low.
        ConnectionManager::EventFn on_event;
        // When the LE bearer was last seen down, so a blip can be told from a
        // session that is really gone. -1 while it is up.
        int64_t le_down_since = -1;
        // When the LE link was first seen up while the phone offered no ANCS
        // service. -1 whenever that is not the case.
        int64_t ancs_absent_since = -1;
        uint64_t le_drops_seen = 0;
        uint64_t le_link_drops_seen = 0;
        int map_failures = 0;
        // The last listing on the open MAP session, -1 for none, and whether
        // that session was ever listed.
        int64_t last_message_poll = -1;
        bool map_session_new = true;
        // The listing a notification asked for. Supervisor thread only.
        MessageSyncRequest message_request;
        // Asked from another thread, dated by the next tick.
        std::mutex sync_ask_mutex;
        std::atomic<bool> sync_asked{false};
        std::string sync_ask_reason;
        bool sync_ask_expects_new = false;
        // Which notifications are about the mailbox or the call history.
        std::mutex triggers_mutex;
        AncsSyncTriggers sync_triggers;
        // Wall clock of the last listing that succeeded, 0 for none, and the
        // supervisor's clock of the same, -1 for none.
        std::atomic<int64_t> last_sync_wall{0};
        int64_t last_sync_at = -1;

        // --- The link mode, and the classic link on demand (link_mode.hpp,
        // classic_demand.hpp). ---
        // What bt_set_link_mode last set, as a LinkMode.
        std::atomic<int> link_mode_wanted{static_cast<int>(LinkMode::Lean)};
        // What is applied, and whether it has been handed to the supervisors.
        EffectiveLinkMode effective{LinkMode::Full, ""};
        bool link_mode_applied = false;
        // The same as `effective.lean()`, for other threads.
        std::atomic<bool> lean{false};
        // Leases are taken from any thread; the machine is stepped by the supervisor.
        mutable std::mutex demand_mutex;
        ClassicDemand demand;
        ClassicState demand_state_logged = ClassicState::Idle;
        std::atomic<bool> lease_changed{false};
        // What outlives a closed link: verdicts, notifications granted.
        LinkMemory memory;
        // logind has announced the computer's sleep.
        std::atomic<bool> suspended_soon{false};
        SleepAnnouncer sleep_announcer;
        // LE up and notifications ready, as last seen: its rising edge is a sync.
        bool sync_edge = false;
        bool phone_link_seen = false;
        int64_t last_sync_lease_at = -1;
        int64_t last_hourly_lease_at = -1;
        // A Messages notification wants the link, dated for the debounce.
        bool arrival_wanted = false;
        int64_t arrival_wanted_at = -1;
        bool backlog_wanted = false;
        // How the LE link is holding (le_health.hpp): the classic link is not
        // opened unasked on a young LE link, nor soon after an opening or a
        // close took the LE link down, and a burst of LE drops takes the
        // advert off air for a moment. Supervisor thread only.
        LeHealth le_health;
        // LE links that ended before the notification service had answered
        // on them (le_strikes.hpp): each keeps the advert off air for a
        // while. Supervisor thread only.
        LeStrikes le_strikes;
        // An LE link came up or ended, or the phone sent a notification
        // packet: the next pass starts now (le_first_use.hpp). Set from
        // BlueZ's thread under `mutex`.
        std::atomic<bool> link_event{false};
        bool le_prompt = true;
        // The LE links seen coming up, and the steps of the last one.
        uint64_t le_ups_seen = 0;
        LeFirstUse first_use;
        // An iPhone that does not come back after the controller was reset
        // (le_after_reset.hpp, #59): the menu says what to do.
        LeAfterReset le_after_reset;
        // 0 nothing to say, 1 ask for this computer's Bluetooth to be turned
        // off and on, 2 the iPhone's.
        std::atomic<int> le_after_reset_hint{0};
        int64_t le_reset_seen_ms = -1;
        int64_t le_restarted_at_ms = -1;
        // #62: an iPhone near that does not come back over LE (le_recovery.hpp).
        LeRecovery le_recovery;
        // #63: BlueZ's LE link that the controller no longer holds.
        LeGhost le_ghost;
        // #64: BlueZ answering "In Progress" for ever to the classic connect.
        ClassicStuck classic_stuck;
        bool le_dial_wanted = false;
        std::atomic<bool> le_stuck{false};
        int64_t le_stuck_since = -1;
        void record_le_stuck(bool started, int64_t now, bool opened_by_phone);
        // The connection parameters asked of the phone as early as possible
        // on each LE link (le_conn_params.hpp). On when the helper, or the
        // capability, is there. Supervisor thread only.
        LeConnParamsPlan conn_params;
        std::unique_ptr<LeConnUpdater> conn_updater;
        // The link the request running was made on.
        uint64_t conn_params_link = 0;
        void step_conn_params(int64_t now);
        void read_conn_params_news(int64_t now);
        void wake_for_event() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                link_event = true;
            }
            wake.notify_all();
        }
        // What step_demand() last did to the classic link, for the next look.
        bool le_wait_logged = false;
        // The phone's sound is wanted here (the standing choice). It no longer
        // holds the link by itself (#43): see plan_audio_lease().
        std::atomic<bool> audio_here_wanted{false};
        // The user turned it on, or asked for it again: the link is offered
        // to the iPhone for PHONE_AUDIO_OFFER_SECONDS.
        std::atomic<bool> audio_asked{false};
        // Whether the call history was ever read, from the phone or from disk.
        bool call_history_known = false;

        // --- Incoming calls from the first ring (incoming_call.hpp). ---
        // LANDLINE_ANCS_CALLS=0 switches the whole of it off.
        bool ancs_calls = true;
        mutable std::mutex ring_mutex;
        IncomingCallTracker ring;
        void publish_ring(const IncomingCallTracker::Event& event);
        void step_ring(int64_t now);
        // Wakes the supervisor so that a lease just wanted is acted on now.
        void nudge();
        // A dial in progress wants the classic link (dial_plan.hpp).
        std::atomic<bool> dial_wants_link{false};
        // A call is being placed, is ringing, or is in progress: read by the
        // supervisor each tick (see call_busy()).
        std::atomic<bool> calls_busy{false};
        const bool obex_waits = obex_waits_for_calls(std::getenv("LANDLINE_OBEX_DURING_CALLS"));
        bool obex_wait_logged = false;
        // One dial at a time; its id, and whether it was asked to stop.
        std::mutex dial_mutex;
        bool dial_running = false;
        std::string dial_operation_id;
        bool dial_cancel = false;
        // When a call command was last sent, on the supervisor's clock.
        std::atomic<int64_t> call_command_at{-1};
        // When run() started: the supervisor's clock, for the other threads.
        std::chrono::steady_clock::time_point run_started = std::chrono::steady_clock::now();

        int64_t clock() const {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - run_started)
                .count();
        }
        LinkMode configured_link_mode() const;
        void apply_link_mode(int64_t now);
        void step_demand(int64_t now);
        void finish_data_round(int64_t now);
        void remember_verdicts();
        void save_memory() const;
        bool take_lease(Lease lease, int ttl_seconds = 0, bool urgent = false);
        void release_lease(Lease lease);
        // The MAP session object the senders use, made as soon as the
        // session opens and dropped when it closes.
        void adopt_map_session();
        void forget_map_session();
        std::string open_map_path;
        // Whether the phone answers a listing for a child of the message root.
        // Cleared once, permanently for the session, if it turns out not to.
        bool map_lists_subfolders = true;
        bool map_lists_sent = true;
        // The first listing on a new MAP session returns the phone's existing
        // inbox, which on a first run is every message it holds.
        bool map_session_backfill = true;
        std::string pulled_pbap_path;
        // The last full read of the address book by this process: when, and the
        // phone's entry count then. -1 for never and for unknown.
        int64_t contacts_pulled_at = -1;
        int contacts_size_at_pull = -1;
        // When a read put off by plan_contacts() falls due, -1 for none.
        int64_t contacts_due_at = -1;

        // The clock of the tick in progress, for refresh_status().
        int64_t tick_now = 0;
        // The drop the bearer supervisor made on purpose, of BR/EDR for
        // hands-free or of every link for LE, so the sessions it takes down
        // are not published as lost. `hfp_cycles_seen` counts both.
        PlannedDrop planned_drop;
        int hfp_cycles_seen = 0;
        // BluezMonitor::bluez_generation() as last acted on.
        uint64_t bluez_generation_seen = 0;
        // When BR/EDR was first seen reaching the phone while LE was down.
        int64_t seen_without_le_since = -1;
        // The "LE first" measures as configured, for what is decided here.
        LeFirstConfig le_first = le_first_off();
        // le_after_sleep.hpp: the computer's sleep, as two clocks tell it.
        SleepWatch sleep_watch;
        bool resume_watch = true;
        int64_t resumed_at = -1;
        bool le_seen_since_resume = true;
        // LE links that ended on a device BlueZ holds no pairing for.
        bool stranger_watch = true;
        uint64_t strangers_seen = 0;
        int64_t stranger_at = -1;
        int64_t stranger_rearmed_at = -1;
        // le_state and le_cause as last published, for the journal.
        LeState le_state_logged;
        int advert_fast_seconds = ADVERT_FAST_SECONDS;
        // Makes the long OBEX reads wait for the notification session.
        NotificationPriority notification_priority;
        int ancs_first_seconds = ANCS_FIRST_SECONDS;
        bool long_reads_held = false;
        // Set by a client that wants the profiles tried now.
        std::atomic<bool> retry_requested{false};
        // Whether the phone offered its notification service at the last look.
        bool ancs_offered = false;
        bool bus_closed_logged = false;
        // Whether the thread goes on with the quick profile attempts and the
        // notification client while it waits on a long OBEX read.
        bool long_read_attempts = true;
        std::chrono::steady_clock::time_point next_idle_pass{};

        // The phone's recent calls (PBAP cch), newest first. Memory only.
        std::vector<VCard> call_history;
        mutable std::mutex call_history_mutex;
        // Set by a new PBAP session and by a call ending, which can happen on
        // refresh_calls()'s thread.
        std::atomic<bool> call_history_stale{true};
        ConnectionManager::CallsFn on_call_history;

        std::shared_ptr<ancs::AncsClient> ancs_client;
        mutable std::mutex ancs_mutex;
        ConnectionManager::NotificationFn on_notification;
        ConnectionManager::WithdrawFn on_withdraw;
        bool ancs_ready = false;
        std::string ancs_reason;

        std::atomic<bool> ancs_wanted{true};
        std::atomic<bool> ancs_content_wanted{true};

        std::shared_ptr<TelephonyClient> telephony;
        mutable std::mutex telephony_mutex;
        ConnectionManager::CallsFn on_calls;
        std::atomic<bool> calls_wanted{false};
        bool calls_enabled = false;
        // Whether the sound of calls comes to this computer by default.
        std::atomic<bool> call_audio_here{false};
        // Last published call list. The mutex also serialises on_calls: the supervisor tick and
        // refresh_calls() both publish.
        nlohmann::json last_calls;
        std::mutex calls_publish_mutex;

        // Brings the telephony client in line with the wanted preference.
        void apply_calls_preference();

        // Publishes the call list when it differs from the last one.
        void sync_calls();

        // The current client, kept alive for as long as the caller.
        std::shared_ptr<TelephonyClient> calls_client() const {
            std::lock_guard<std::mutex> lock(telephony_mutex);
            return telephony;
        }

        void run();

        // Follows the phone's presence, publishes bt_presence, and locks the
        // session once the phone has been out of range long enough.
        void supervise_away_lock(int64_t now, bool obex_up);
        void publish_presence();

        void sync_messages(int64_t now);
        // Hands a listing asked for by another thread to the supervisor's clock.
        void date_sync_asks(int64_t now);
        void ask_message_sync(std::string_view reason);
        void sync_contacts(int64_t now);
        void sync_call_history();
        nlohmann::json call_history_json() const;
        void sync_ancs(int64_t now);
        void after_reset_pass(bool le_up);
        void bluez_restarted();
        void watch_sleep(int64_t now);
        void watch_strangers(int64_t now);
        void watch_classic_stuck(int64_t now);
        void watch_le_health(int64_t now);
        void watch_le_strikes(int64_t now);
        LeState le_state(int64_t now) const;

        // Brings the ANCS client and the bearer supervisor in line with the wanted preference and the controller's
        // current capability.
        void apply_ancs_preference();
        void build_ancs_client();

        // The current client, kept alive for as long as the caller.
        std::shared_ptr<ancs::AncsClient> ancs() const {
            std::lock_guard<std::mutex> lock(ancs_mutex);
            return ancs_client;
        }

        // Rebuilds the cached payload from the supervisors. Supervisor thread
        // only: it is the sole writer of BearerStatus and ProfileStatus, both of
        // which hold a std::string that no other thread may read directly.
        void refresh_status();

        // Hands the cached status to the subscriber only when it differs from the
        // last one published. The supervisors report their own internal
        // transitions as changes, and most of those are invisible in the payload,
        // so without this a subscriber receives the same status every second.
        void publish();

        // Guards everything below. publish() is reached from the supervisor
        // thread and from the ANCS readiness callback on the GLib thread, and
        // status() from whichever thread is serving a command.
        mutable std::mutex status_mutex;
        nlohmann::json current_status;
        nlohmann::json last_published;
        // The last bt_presence published, for a client that asks.
        nlohmann::json presence_status;
    };

    void ConnectionState::refresh_status() {
        nlohmann::json payload = to_json(bearers->status(), profiles->status());
        mask_planned_drop(payload, planned_drop, tick_now);
        auto client = calls_client();
        payload["calls"] = client ? client->status() : nlohmann::json(nullptr);
        const BatteryReading battery = phone_battery(monitor, address, payload["calls"]);
        if (battery.known()) {
            payload["battery"] = battery.percent;
            payload["battery_source"] = battery.source;
            payload["battery_state"] = battery_state(battery.percent);
        } else {
            payload["battery"] = nullptr;
            payload["battery_source"] = nullptr;
            payload["battery_state"] = nullptr;
        }
        // Read off the way the level moves; null when that does not settle it.
        if (const auto charging = charge_watch.observe(battery.percent, battery.source, tick_now * 1000))
            payload["battery_charging"] = *charging;
        else
            payload["battery_charging"] = nullptr;
        const auto alert = battery_watch.observe(battery.percent);
        // The notification link, said outright: a client can show
        // "Notifications paused" and why without reading five fields.
        const LeState le = le_state(tick_now);
        payload["le_state"] = le.state;
        payload["le_cause"] = le.cause;
        payload["le_detail"] = le.detail;
        // LE links that ended before they were of use since one last served.
        payload["le_strikes"] = le_strikes.strikes();
        // The iPhone has not come back since the controller was reset (#59):
        // whose Bluetooth to turn off and on, or false.
        if (const int hint = le_after_reset_hint.load(); hint == 0)
            // #62: the iPhone answers over the classic link but not over LE.
            payload["le_after_reset"] = le_stuck.load() ? nlohmann::json("phone") : nlohmann::json(false);
        else
            payload["le_after_reset"] = hint == 1 ? "computer" : "phone";
        // Whether other connection parameters were asked of the phone on
        // this LE link, and what came of it.
        payload["le_conn_params"] = conn_params.state();
        if (const auto& got = conn_params.in_force(); got && bearers->status().le_connected)
            payload["le_conn_values"] = {
                {"interval_ms", got->max_interval_ms}, {"latency", got->latency}, {"timeout_ms", got->timeout_ms}};
        else
            payload["le_conn_values"] = nullptr;
        if (le.state != le_state_logged.state || le.cause != le_state_logged.cause) {
            if (!le_state_logged.state.empty() || le.state == "paused")
                debug::log(INFO,
                           "bluetooth: notification link {}{}{}",
                           le.state,
                           le.cause.empty() ? "" : ": ",
                           le.cause);
            le_state_logged = le;
        }
        // The link mode and what the classic link is doing. The older fields
        // above keep their strict sense: what is up at this instant.
        const bool lean_now = effective.lean();
        const BearerStatus& bearer = bearers->status();
        const ProfileStatus& sessions = profiles->status();
        ClassicState classic_state = bearer.classic_connected ? ClassicState::Open : ClassicState::Idle;
        std::string opened_by = bearer.classic_connected ? "engine" : "";
        nlohmann::json leases = nlohmann::json::array();
        if (lean_now) {
            std::lock_guard<std::mutex> lock(demand_mutex);
            classic_state = demand.state();
            opened_by = demand.opened_by();
            for (const Lease lease : demand.leases())
                leases.push_back(to_string(lease));
        }
        payload["link_mode"] = to_string(configured_link_mode());
        payload["link_mode_effective"] = to_string(effective.mode);
        payload["link_mode_reason"] = effective.reason;
        payload["classic_state"] = to_string(classic_state);
        payload["classic_leases"] = std::move(leases);
        payload["classic_opened_by"] = opened_by;
        const auto service = [&](const char* open, bool held, ObexError error, ProfileVerdict verdict) {
            ServiceFacts facts;
            facts.open = payload.value(open, false);
            facts.held = held;
            facts.error_forbidden = error == ObexError::Forbidden;
            facts.error_other = error != ObexError::None && error != ObexError::Forbidden;
            facts.verdict = verdict;
            facts.lean = lean_now;
            facts.le_connected = bearer.le_connected;
            return service_state(facts);
        };
        payload["messages"] = service("map_open", sessions.map_held, sessions.map_error, memory.map);
        payload["contacts"] = service("pbap_open", sessions.pbap_held, sessions.pbap_error, memory.pbap);
        const int64_t synced = last_sync_wall.load();
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            payload["ancs_ready"] = ancs_ready;
            payload["ancs_reason"] = ancs_reason;
            payload["ancs_held"] = hold_ancs.load();
            CallsFacts calls;
            calls.calls_enabled = calls_enabled;
            calls.hands_free_ready = payload["calls"].is_object() && payload["calls"].value("available", false);
            calls.lean = lean_now;
            calls.le_connected = bearer.le_connected;
            calls.ancs_ready = ancs_ready;
            calls.classic_failed = lean_now && classic_state == ClassicState::Failed;
            payload["calls_available"] = calls_state(calls);
            if (synced > 0) {
                MessageSyncFacts sync;
                sync.ancs_ready = ancs_ready;
                sync.ancs_content = ancs_content_wanted.load();
                MessageSyncConfig sync_config;
                sync_config.background_override = message_poll_override();
                payload["last_sync"] = synced;
                payload["next_sync"] = synced + (lean_now ? MESSAGE_BACKGROUND_POLL_SECONDS
                                                          : message_background_seconds(sync, sync_config));
            } else {
                payload["last_sync"] = nullptr;
                payload["next_sync"] = nullptr;
            }
            current_status = std::move(payload);
        }
        if (alert) {
            debug::log(INFO,
                       "bluetooth: iPhone battery at {}%, under the {}% threshold{}",
                       alert->level,
                       alert->threshold,
                       alert->initial ? " (first reading)" : "");
            if (on_event)
                on_event({{"command", "bt_battery_low"},
                          {"level", alert->level},
                          {"threshold", alert->threshold},
                          {"state", battery_state(alert->level)},
                          {"source", battery.source},
                          {"initial", alert->initial}});
        }
    }

    LeState ConnectionState::le_state(int64_t now) const {
        const BearerStatus& bearer = bearers->status();
        LeStateFacts facts;
        facts.ancs_enabled = ancs_enabled;
        facts.device_paired = bearer.device_paired;
        facts.le_available = bearer.le_available;
        facts.le_connected = bearer.le_connected;
        facts.classic_connected = bearer.classic_connected;
        facts.obex_up = profiles->status().map_open || profiles->status().pbap_open;
        facts.bond_unusable = bearer.bond.needs_repair || bearer.bond.state == BondState::Broken;
        facts.holding_for_le = bearer.holding_for_le;
        facts.advert_registered = ancs_solicitation_active();
        facts.now = now;
        facts.le_down_since = le_down_since;
        facts.resumed_at = resumed_at;
        facts.le_seen_since_resume = le_seen_since_resume;
        facts.stranger_at = stranger_at;
        facts.unsteady = le_health.unsteady(now) || le_strikes.unsteady();
        facts.advert_paused = le_health.advert_paused(now);
        facts.holding_off = le_strikes.advert_paused(now);
        facts.lost_after_burst =
            le_health.lost_after_burst(le_down_since) || le_strikes.lost_after_strikes(le_down_since);
        facts.classic_seen_since_loss = le_health.classic_seen_since(le_down_since);
        return le_state_for(facts);
    }

    namespace {
        int64_t clock_ms(clockid_t clock) {
            timespec ts{};
            if (clock_gettime(clock, &ts) != 0)
                return 0;
            return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        }
    } // namespace

    // The computer slept. Every link was cut on the way down (BlueZ reports
    // Reason.Suspend), and the clock this thread runs on stood still, so
    // nothing here knows on its own that anything happened: the fast window
    // of the advert, the hold and the recycle were all dated before it.
    void ConnectionState::watch_sleep(int64_t now) {
        const auto slept = sleep_watch.observe(clock_ms(CLOCK_MONOTONIC), clock_ms(CLOCK_BOOTTIME));
        if (!slept || !resume_watch)
            return;
        debug::log(INFO,
                   "bluetooth: the computer slept for {}s; registering the advertisement again at its fast "
                   "interval and leaving BR/EDR alone first",
                   *slept / 1000);
        resumed_at = now;
        le_seen_since_resume = false;
        // The syncs were dated on a clock that stood still: a long sleep
        // makes the next one due when the LE link returns.
        {
            const int64_t slept_s = *slept / 1000;
            std::lock_guard<std::mutex> lock(demand_mutex);
            last_sync_at = dated_across_sleep(last_sync_at, slept_s);
            last_sync_lease_at = dated_across_sleep(last_sync_lease_at, slept_s);
            last_hourly_lease_at = dated_across_sleep(last_hourly_lease_at, slept_s);
        }
        // The links the sleep cut are not the phone's doing.
        le_health.reset(le_link_drops_seen);
        le_strikes.reset(le_link_drops_seen);
        // The sleep logind announced has happened, whether or not its
        // "woken up" was heard: leases are served again.
        suspended_soon = false;
        bearers->resumed(now);
        bearer_ops->forget_classic_refusal();
        // Whatever the controller kept of the advert across the sleep, it is
        // registered again: one D-Bus round trip against a silent radio.
        renew_ancs_solicitation();
        if (auto client = ancs())
            client->retry_now();
    }

    // The LE links that ended since the last look, and what follows from
    // them. Run once per pass, after the supervisor has read the links.
    void ConnectionState::watch_le_health(int64_t now) {
        const BearerStatus& bearer = bearers->status();
        uint64_t drops = 0;
        if (bearer.device_present) {
            const auto objects = monitor->snapshot();
            for (const auto& device : objects.devices)
                if (device.address == address || normalize_address(device.address) == normalize_address(address)) {
                    drops = monitor->le_link_drop_count(device.path);
                    break;
                }
        }
        // A phone BlueZ no longer lists reads 0: the count is kept as it was.
        le_link_drops_seen = std::max(le_link_drops_seen, drops);
        watch_le_strikes(now);
        const auto event = le_health.observe(now, bearer.le_connected, bearer.classic_connected, le_link_drops_seen);
        if (!event)
            return;
        switch (event->kind) {
        case LeHealth::Event::Kind::ClassicBlamed:
            debug::log(WARN,
                       "bluetooth: the LE link dropped {}s after the classic link was {}; the classic link is not "
                       "opened again for {}s unless a call or the user asks for it",
                       event->after_seconds,
                       to_string(event->touch),
                       event->quiet_seconds);
            break;
        case LeHealth::Event::Kind::Burst:
            if (event->pause_seconds > 0) {
                debug::log(WARN,
                           "bluetooth: the LE link dropped {} times within {}s; the advertisement is taken off air "
                           "for {}s so that the iPhone is not offered links that do not hold",
                           event->drops,
                           LE_BURST_WINDOW_SECONDS,
                           event->pause_seconds);
            } else {
                debug::log(WARN,
                           "bluetooth: the LE link dropped {} times within {}s",
                           event->drops,
                           LE_BURST_WINDOW_SECONDS);
            }
            break;
        case LeHealth::Event::Kind::Steady:
            debug::log(INFO, "bluetooth: the LE link has held; it is no longer treated as unsteady");
            break;
        }
    }

    // The LE links that ended before the notification service had answered
    // on them. Run before the notification client hears of the drop, so
    // that its readiness still describes the link that ended.
    void ConnectionState::watch_le_strikes(int64_t now) {
        const BearerStatus& bearer = bearers->status();
        LeStrikeFacts facts;
        facts.now = now;
        // Only a phone that has offered the service on a whole bond is held
        // to using it: a pairing under way drops and reopens the link.
        facts.armed = ancs_enabled && bearer.device_paired && bearer.le_available && !bearer.standing_back &&
                      !bearer.bond.needs_repair && bearer.bond.state != BondState::Broken &&
                      bearer_ops->phone_offers_ancs();
        facts.le_connected = bearer.le_connected;
        facts.drop_count = le_link_drops_seen;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            facts.service_live = ancs_ready;
        }
        const auto event = le_strikes.observe(facts);
        if (!event)
            return;
        switch (event->kind) {
        case LeStrikes::Event::Kind::Strike:
            if (event->pause_seconds > 0) {
                debug::log(WARN,
                           "bluetooth: {} ended before the notification service had answered (unused links in a "
                           "row: {}); the advertisement is taken off air for {}s so that the iPhone is not offered "
                           "another link that fails{}",
                           event->links == 1 ? std::string("the LE link")
                                             : std::to_string(event->links) + " LE links",
                           event->strikes,
                           event->pause_seconds,
                           le_strikes.slow_advert() ? ", then put back at its slow interval" : "");
                // Now, not at the end of the pass: the phone answers an
                // advert on air within a fraction of a second.
                supervise_ancs_solicitation(*monitor, false);
            } else if (le_strikes.slow_advert()) {
                debug::log(WARN,
                           "bluetooth: {} ended before the notification service had answered (unused links in a "
                           "row: {}); the advertisement stays on air at its slow interval",
                           event->links == 1 ? std::string("the LE link")
                                             : std::to_string(event->links) + " LE links",
                           event->strikes);
            } else if (le_strikes.sets_rhythm() && !le_strikes.braked()) {
                // On 18 November the iPhone came back after every unused
                // link: withholding the advert only delayed it (#29).
                debug::log(WARN,
                           "bluetooth: {} ended before the notification service had answered (unused links in a "
                           "row: {}); the advertisement stays on air, fast for {}s then at 152.5 ms, and the "
                           "iPhone is called again",
                           event->links == 1 ? std::string("the LE link")
                                             : std::to_string(event->links) + " LE links",
                           event->strikes,
                           le_strikes.config().fast_seconds);
            } else {
                debug::log(WARN,
                           "bluetooth: the LE link ended before the notification service had answered (unused "
                           "links in a row: {})",
                           event->strikes);
            }
            if (event->braked)
                debug::log(WARN,
                           "bluetooth: {} LE links in a row ended before the notification service had answered, "
                           "none served in between; the advertisement stays on air at 152.5 ms, with no fast phase, "
                           "until a link serves (LANDLINE_LE_STRIKE_BRAKE={})",
                           le_strikes.unused_run(),
                           le_strikes.config().brake_from);
            break;
        case LeStrikes::Event::Kind::Served:
            debug::log(INFO,
                       "bluetooth: the notification service has been live on the LE link for {}s; the {} unused "
                       "link(s) before it are forgotten",
                       le_strikes.config().served_seconds,
                       event->strikes);
            break;
        case LeStrikes::Event::Kind::Churn:
            debug::log(WARN,
                       "bluetooth: {} LE links that had served were lost within {}s; the advertisement goes at its "
                       "slow interval, with no fast phase, until a minute passes without another",
                       event->links,
                       LE_CHURN_WINDOW_SECONDS);
            break;
        case LeStrikes::Event::Kind::Forgotten:
            debug::log(INFO,
                       "bluetooth: no unused LE link for {}s; the {} counted before that are forgotten",
                       LE_STRIKE_MEMORY_SECONDS,
                       event->strikes);
            break;
        }
    }

    // An LE link came and went on a device BlueZ holds no pairing for,
    // while the paired iPhone has no LE link and the advert is on air.
    // #64: see classic_stuck.hpp.
    void ConnectionState::watch_classic_stuck(int64_t now) {
        const bool classic_up = bearer_ops->classic_connected();
        if (classic_up)
            bearer_ops->forget_classic_busy();
        const auto step = classic_stuck.observe(now,
                                                bearer_ops->classic_busy_answers(),
                                                classic_up,
                                                bearer_ops->le_connected(),
                                                bearer_ops->device_paired());
        switch (step) {
        case ClassicStuck::Step::None:
            break;
        case ClassicStuck::Step::Disconnect: {
            debug::log(WARN,
                       "bluetooth: BlueZ keeps answering \"In Progress\" to the BR/EDR connect with no link to show "
                       "for it: a connect of its own never ended (#64). Disconnecting the iPhone in BlueZ to release "
                       "it; the LE link comes back by itself, then the classic link is asked for by the other path");
            bearer_ops->forget_classic_busy();
            std::string err;
            if (!bearer_ops->disconnect_all(err))
                debug::log(WARN, "bluetooth: the disconnect could not be asked ({})", err);
            break;
        }
        case ClassicStuck::Step::Connect:
            if (bearer_ops->reconnect_through_device()) {
                classic_stuck.asked();
                bearer_ops->forget_classic_busy();
                debug::log(INFO,
                           "bluetooth: the LE link is back; asking BlueZ for the classic link through Device1.Connect, "
                           "which clears what its BR/EDR bearer kept (#64)");
            }
            break;
        case ClassicStuck::Step::GaveUp:
            debug::log(WARN,
                       "bluetooth: the LE link did not come back after the disconnect (#64); the classic link is "
                       "left to the usual rules");
            break;
        }
    }

    void ConnectionState::watch_strangers(int64_t now) {
        const BluezMonitor::LeStranger stranger = monitor->le_stranger();
        if (stranger.count == strangers_seen)
            return;
        strangers_seen = stranger.count;
        if (!stranger_watch)
            return;
        const BearerStatus& bearer = bearers->status();
        StrangerFacts facts;
        facts.address = stranger.path;
        facts.address_type = stranger.address_type;
        facts.le.ancs_enabled = ancs_enabled;
        facts.le.le_available = bearer.le_available;
        facts.le.phone_offers_ancs = bearer_ops->phone_offers_ancs();
        facts.le.standing_back = bearer.standing_back;
        facts.phone_le_connected = bearer.le_connected;
        facts.advert_registered = ancs_solicitation_active();
        if (!may_be_unresolved_phone(facts))
            return;
        stranger_at = now;
        const bool rearm = stranger_rearmed_at < 0 || now - stranger_rearmed_at >= STRANGER_REARM_SECONDS;
        debug::log(WARN,
                   "bluetooth: an LE device BlueZ holds no pairing for ({}, resolvable private address) connected "
                   "and left ({}) while the paired iPhone has no LE link and the advertisement is on air. It may "
                   "be the iPhone under an address that was not resolved to the pairing, or a device nearby.{}",
                   stranger.path,
                   stranger.reason,
                   rearm ? " Registering the advertisement again." : "");
        if (rearm) {
            stranger_rearmed_at = now;
            renew_ancs_solicitation();
        }
    }

    void ConnectionState::publish() {
        if (!on_change)
            return;

        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            if (current_status.is_null() || current_status == last_published)
                return;
            payload = current_status;
            last_published = payload;
        }
        on_change(payload);
    }

    nlohmann::json to_json(const BearerStatus& bearer, const ProfileStatus& profiles) {
        nlohmann::json payload = {
            {"command", "bt_connection_changed"},
            {"device_present", bearer.device_present},
            {"device_paired", bearer.device_paired},
            {"classic_connected", bearer.classic_connected},
            {"le_available", bearer.le_available},
            {"le_connected", bearer.le_connected},
            {"map_open", profiles.map_open},
            {"pbap_open", profiles.pbap_open},
            {"map_error", to_string(profiles.map_error)},
            {"pbap_error", to_string(profiles.pbap_error)},
            {"map_held", profiles.map_held},
            {"pbap_held", profiles.pbap_held},
            // Two independent halves, so both reasons are reported; the UI shows
            // whichever is actionable.
            {"link_reason", bearer.reason},
            {"profile_reason", profiles.reason},
        };
        // bond_state, bond_detail, needs_repair.
        put_bond(payload, bearer.bond);
        return payload;
    }

    // Pulls the inbox and folds it into the store, announcing anything new.
    void ConnectionState::sync_messages(int64_t now) {
        // The journal stays shut when the wallet was locked at startup
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            if (!g_journal.is_open() && secret::retention() != Retention::None && secret::have_key()) {
                if (g_journal.open()) {
                    // PBAP pull while locked lives only in memory
                    if (contact_store().size() == 0)
                        contact_store() = load_contacts();
                    else
                        save_contacts(contact_store());
                    restore_removed_threads();
                    if (!call_history_known) {
                        std::vector<VCard> calls = load_call_history();
                        call_history_known = !calls.empty();
                        std::lock_guard<std::mutex> history_lock(call_history_mutex);
                        if (call_history.empty())
                            call_history = std::move(calls);
                    }
                    auto history = g_journal.load(now);
                    for (const auto& message : history)
                        g_messages.add(message);
                    g_journal.compact(history);
                    debug::log(INFO, "bluetooth: wallet unlocked; replayed {} message(s)", history.size());
                }
            }
        }

        if (!profiles->status().map_open) {
            forget_map_session();
            message_request.expire(now);
            return;
        }

        adopt_map_session();

        MessageSyncFacts facts;
        facts.now = now;
        facts.last_poll_at = last_message_poll;
        facts.requested_at = message_request.requested_at;
        facts.request_attempts = message_request.attempts;
        facts.ancs_content = ancs_content_wanted.load();
        facts.session_new = map_session_new;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            facts.ancs_ready = ancs_ready;
        }
        MessageSyncConfig sync_config;
        sync_config.background_override = message_poll_override();
        const MessageSyncCause cause = message_sync_cause(facts, sync_config);
        if (cause == MessageSyncCause::None)
            return;
        last_message_poll = now;
        map_session_new = false;
        {
            const bool asked = message_request.pending() && cause != MessageSyncCause::Session;
            debug::log(DEBUG,
                       "bluetooth: inbox listed ({}{}{})",
                       to_string(cause),
                       asked ? ": " : "",
                       asked ? message_request.reason : std::string());
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }
        if (!session)
            return;

        std::string err;
        // Empty means the current folder; a name means that child of it.
        auto listings = session->list_messages(map_lists_subfolders ? MAP_FOLDER_INBOX : "", MESSAGE_LIST_MAX, err);

        // Some stacks only answer for the folder they are standing in. Walking
        // into the inbox costs the sent folder but keeps messages working, so it
        // is worth one attempt before declaring the session dead.
        if (!err.empty() && map_lists_subfolders && !map_session_dead(err)) {
            debug::log(
                WARN, "bluetooth: listing '{}' failed ({}); falling back to the inbox alone", MAP_FOLDER_INBOX, err);
            map_lists_subfolders = false;
            map_lists_sent = false;
            std::string nav_err;
            // Relative: the session is parked on the message root.
            if (!session->set_folder(MAP_FOLDER_INBOX, nav_err))
                debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_FOLDER_INBOX, nav_err);
            err.clear();
            listings = session->list_messages("", MESSAGE_LIST_MAX, err);
        }

        if (!err.empty()) {
            debug::log(WARN, "bluetooth: ListMessages failed: {}", err);
            // ponytail: a fail count, not an obexd ObjectManager watcher. Costs up
            // to MAP_FAILURES_BEFORE_REOPEN polls of latency; subscribe to
            // org.bluez.obex InterfacesRemoved if that ever proves too slow.
            if (map_session_dead(err) || ++map_failures >= MAP_FAILURES_BEFORE_REOPEN) {
                debug::log(WARN, "bluetooth: MAP session is not answering; reopening it");
                map_failures = 0;
                profiles->drop_map();
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session.reset();
                open_map_path.clear();
            }
            return;
        }
        map_failures = 0;

        // Replies sent from the phone itself live in a separate folder and name
        // the user as their sender, so without this a conversation only ever
        // shows one side of itself.
        if (map_lists_sent) {
            std::string sent_err;
            auto sent = session->list_messages(MAP_FOLDER_SENT, MESSAGE_LIST_MAX, sent_err);
            if (!sent_err.empty()) {
                if (!map_session_dead(sent_err)) {
                    debug::log(WARN,
                               "bluetooth: the sent folder is unavailable ({}); showing received messages only",
                               sent_err);
                    map_lists_sent = false;
                }
            } else {
                // An empty sent folder is just a folder with nothing new in it.
                // Only an error means the phone will not serve it.
                listings.insert(listings.end(), sent.begin(), sent.end());
            }
        }

        // A listing reports the message text in Subject, which is a preview.
        // Line breaks arrive flattened to spaces and a long one is cut. The
        // bMessage is the only source of the real body, so every message not
        // seen before costs one OBEX Get.
        struct Pending {
            Message message;
            const MapListing* listing;
        };

        std::vector<Pending> unseen;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            for (const auto& listing : listings) {
                Message message = message_from_listing(listing);
                if (message.thread_key.empty()) {
                    debug::log(WARN,
                               "bluetooth: dropping message {} from folder '{}': no usable address "
                               "(sender='{}' address='{}')",
                               listing.handle,
                               listing.folder,
                               listing.sender_name,
                               listing.sender_address);
                    continue;
                }
                if (!g_messages.find(message.handle)) {
                    unseen.push_back({std::move(message), &listing});
                    continue;
                }
                g_messages.add(message);
                g_messages.set_read(message.handle, message.read);
            }
        }

        // Newest first, so when the cap defers part of a backfill the messages
        // the user is looking at are the ones that get their real body now.
        std::sort(unseen.begin(), unseen.end(), [](const Pending& a, const Pending& b) {
            return a.message.timestamp > b.message.timestamp;
        });
        if (unseen.size() > MESSAGE_BODY_FETCH_MAX)
            unseen.resize(MESSAGE_BODY_FETCH_MAX);

        for (auto& pending : unseen) {
            BMessage parsed;
            std::string body_err;
            if (session->fetch_bmessage(pending.listing->handle, parsed, body_err))
                pending.message.body = parsed.body;
            else
                debug::log(DEBUG,
                           "bluetooth: no bMessage for {} ({}); keeping the listing preview",
                           pending.message.handle,
                           body_err);
        }

        std::vector<Message> fresh;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            for (auto& pending : unseen) {
                const MapListing& listing = *pending.listing;
                Message& message = pending.message;

                // raw, before anything here can correct or discard it
                if (listing.sent || is_outgoing_folder(listing.folder))
                    debug::log(DEBUG,
                               "bluetooth: phone lists sent msg folder='{}' recipient=[{}] sender=[{}] handle={}",
                               listing.folder,
                               listing.recipient_address,
                               listing.sender_address,
                               listing.handle);

                // MAP gives a group message one sender and no conversation
                // identifier. The correlated Messages notification is the only
                // side channel, and an ambiguous match is refused rather than
                // guessed: filing it wrongly would also aim a reply wrongly.
                if (g_group_replies_enabled) {
                    GroupInfo info;
                    std::lock_guard<std::mutex> group_lock(g_group_mutex);
                    if (g_correlator.correlate(message.body, now, info) == Correlation::Matched) {
                        const std::string key = group_thread_key(info);
                        if (!key.empty()) {

                            const bool stale = !message.outgoing && roster_contradicted(key, message.thread_key);

                            // The sender label is display only and never becomes
                            // part of the thread identity or the reply route.
                            message.peer_name = info.sender;
                            message.thread_key = key;
                            const bool was_stale = g_group_info[key].route_invalidated;
                            g_group_info[key] = info;
                            g_group_info[key].route_invalidated = was_stale || stale;
                        }
                    }
                }
                if (g_messages.add(message))
                    fresh.push_back(message);
                else
                    g_messages.set_read(message.handle, message.read);
            }
        }

        const bool backfill = map_session_backfill;
        map_session_backfill = false;
        // A listing that brought the message settles the request; one that
        // did not is made again, see message_sync_policy.hpp.
        message_request.listed(fresh.size());
        last_sync_wall = static_cast<int64_t>(std::time(nullptr));
        last_sync_at = now;

        for (const auto& message : fresh) {
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                g_journal.append(message);
            }
            if (on_message)
                on_message(message, backfill);
        }
    }

    void ConnectionState::forget_map_session() {
        if (open_map_path.empty())
            return;
        std::lock_guard<std::mutex> lock(g_map_mutex);
        g_map_session.reset();
        open_map_path.clear();
    }

    void ConnectionState::adopt_map_session() {
        if (!profiles->status().map_open || open_map_path == profiles->map_session())
            return;
        open_map_path = profiles->map_session();
        auto session = std::make_shared<MapSession>(profile_ops->bus(), open_map_path);
        // A freshly opened session starts at the root, and SetFolder walks the
        // MAP hierarchy from there — "msg" on its own is not a valid step.
        std::string err;
        if (!session->set_folder(MAP_MSG_ROOT, err))
            debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_MSG_ROOT, err);
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            g_map_session = session;
        }
        last_message_poll = -1;
        map_session_new = true;
        map_session_backfill = true;
        map_lists_subfolders = true;
        map_lists_sent = true;
    }

    void ConnectionState::ask_message_sync(std::string_view reason) {
        {
            std::lock_guard<std::mutex> lock(sync_ask_mutex);
            // The first reason of a burst names it; a removal alone expects no message.
            const bool expects_new = reason != "ancs_removed";
            if (!sync_asked.load() || (expects_new && !sync_ask_expects_new))
                sync_ask_reason = std::string(reason);
            sync_ask_expects_new = sync_ask_expects_new || expects_new;
            sync_asked = true;
        }
        debug::log(DEBUG, "bluetooth: message sync asked ({})", reason);
        {
            std::lock_guard<std::mutex> lock(mutex);
        }
        wake.notify_all();
    }

    void ConnectionState::date_sync_asks(int64_t now) {
        if (!sync_asked.load())
            return;
        std::string reason;
        bool expects_new = false;
        {
            std::lock_guard<std::mutex> lock(sync_ask_mutex);
            reason.swap(sync_ask_reason);
            expects_new = sync_ask_expects_new;
            sync_ask_expects_new = false;
            sync_asked = false;
        }
        message_request.ask(now, reason, expects_new);
        // In the lean link mode a new message is worth opening the link for,
        // and a backlog is worth a sync whatever the last one's date. A
        // removal or a message just sent is not: the next opening lists it.
        if (reason == "ancs" && !arrival_wanted) {
            arrival_wanted = true;
            arrival_wanted_at = now;
        } else if (reason == "ancs_backlog") {
            backlog_wanted = true;
        }
    }

    // Pulls the phonebook at most once per PBAP session. Contacts change far
    // more slowly than messages, and a full pull is an expensive transfer that
    // holds this thread for seconds: a session that merely reopened, after a
    // link loss or the hands-free cycle, keeps the book already in memory
    // unless the phone's entry count moved, and an older book is read again
    // only once the reconnection is behind.
    void ConnectionState::sync_contacts(int64_t now) {
        if (!profiles->status().pbap_open) {
            pulled_pbap_path.clear();
            contacts_due_at = -1;
            return;
        }
        const bool new_session = pulled_pbap_path != profiles->pbap_session();
        if (!new_session && (contacts_due_at < 0 || now < contacts_due_at))
            return;
        if (new_session) {
            pulled_pbap_path = profiles->pbap_session();
            // A session that opens many times a day does not read the call
            // history each time: a call ending, or the notification of one,
            // is what makes it stale.
            if (!effective.lean() || !call_history_known)
                call_history_stale = true;
        }
        contacts_due_at = -1;

        PbapSession session(profile_ops->bus(), pulled_pbap_path);
        std::string err;
        if (!session.select_phonebook(err)) {
            debug::log(WARN, "bluetooth: PBAP Select failed: {}", err);
            return;
        }

        const int size_now = session.phonebook_size();
        static const bool always = contacts_always_pulled(std::getenv("LANDLINE_CONTACTS_ALWAYS_PULL"));
        if (new_session && !always) {
            bool have_book = false;
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                have_book = contact_store().size() > 0;
            }
            const int64_t age = contacts_pulled_at < 0 ? -1 : now - contacts_pulled_at;
            switch (effective.lean() ? plan_contacts_on_demand(have_book, age, contacts_size_at_pull, size_now)
                                     : plan_contacts(have_book, age, contacts_size_at_pull, size_now)) {
            case ContactsPlan::Keep:
                debug::log(INFO,
                           "bluetooth: address book read {}s ago and still {} entries on the phone; not read again",
                           age,
                           size_now);
                return;
            case ContactsPlan::PullLater:
                contacts_due_at = now + CONTACTS_DEFER_SECONDS;
                debug::log(INFO,
                           "bluetooth: address book read {}s ago; reading it again in {}s",
                           age,
                           CONTACTS_DEFER_SECONDS);
                return;
            case ContactsPlan::PullNow:
                break;
            }
        }

        auto contacts = session.pull_all(err, PBAP_MAX_CONTACTS, PBAP_CONTACT_FIELDS, 1);
        if (!err.empty()) {
            debug::log(WARN, "bluetooth: PBAP PullAll failed: {}", err);
            return;
        }

        // The phone refuses fav when its "Phone Favorites" sync toggle is off.
        std::vector<VCard> favorites;
        if (session.select_phonebook(err, "int", "fav"))
            favorites = session.pull_all(err);
        if (err.empty())
            mark_favorites(contacts, favorites);
        else
            debug::log(INFO, "bluetooth: favorites unavailable: {}", err);

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store().set(std::move(contacts));
        }
        save_contacts(contact_store());
        contacts_pulled_at = now;
        contacts_size_at_pull = size_now;
        debug::log(INFO, "bluetooth: pulled {} contacts, {} favorites", contact_store().size(), favorites.size());
    }

    // Re-pulls recent calls when stale. Clearing the flag before the pull means a
    // call that ends mid-transfer schedules another pull instead of being missed.
    void ConnectionState::sync_call_history() {
        if (!profiles->status().pbap_open || pulled_pbap_path != profiles->pbap_session())
            return;
        if (!call_history_stale.exchange(false))
            return;

        PbapSession session(profile_ops->bus(), pulled_pbap_path);
        std::string err;
        std::vector<VCard> history;
        if (session.select_phonebook(err, "int", "cch"))
            history = session.pull_all(err, PBAP_MAX_CALLS, PBAP_CALL_FIELDS);
        // The phone refuses cch when its "Phone Recents" sync toggle is off.
        if (!err.empty()) {
            debug::log(INFO, "bluetooth: call history unavailable: {}", err);
            return;
        }

        debug::log(INFO, "bluetooth: pulled {} recent calls", history.size());
        save_call_history(history);
        call_history_known = true;
        {
            std::lock_guard<std::mutex> lock(call_history_mutex);
            call_history = std::move(history);
        }
        if (on_call_history)
            on_call_history(call_history_json());
    }

    nlohmann::json ConnectionState::call_history_json() const {
        std::vector<VCard> history;
        {
            std::lock_guard<std::mutex> lock(call_history_mutex);
            history = call_history;
        }

        nlohmann::json out = nlohmann::json::array();
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        for (const auto& card : history) {
            const std::string number = card.tels.empty() ? std::string{} : normalize_phone(card.tels.front());
            std::string name = card.name;
            if (name.empty() && !number.empty())
                name = contact_store().name_for("tel:" + number);
            std::string type = card.call_type;
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });

            nlohmann::json entry;
            entry["number"] = number;
            entry["name"] = name;
            entry["type"] = type;
            entry["timestamp"] = parse_map_timestamp(card.call_time);
            out.push_back(std::move(entry));
        }
        return out;
    }

    // Keeps the ANCS client pointed at the LE session and drains its work.
    // #62, point 5: each stuck episode written down with what preceded it,
    // one JSON line, in the state folder (le-stuck.jsonl), to find what
    // triggers it on the iPhone's side.
    void ConnectionState::record_le_stuck(bool started, int64_t now, bool opened_by_phone) {
        nlohmann::json line;
        line["event"] = started ? "stuck" : "back";
        line["at"] = static_cast<int64_t>(std::time(nullptr));
        line["le_missing_seconds"] = le_recovery.lost_since() >= 0 ? now - le_recovery.lost_since() : -1;
        line["probes"] = le_recovery.probes();
        line["dials"] = le_recovery.dials();
        if (started) {
            le_stuck_since = now;
            line["classic_opened_by_phone"] = opened_by_phone;
            line["seconds_since_wake"] = resumed_at >= 0 ? now - resumed_at : -1;
            using namespace std::chrono;
            const int64_t back = monitor->adapter_back_at_ms();
            const int64_t steady_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            line["seconds_since_adapter_back"] = back >= 0 ? (steady_ms - back) / 1000 : -1;
            nlohmann::json others = nlohmann::json::array();
            for (const auto& device : monitor->snapshot().devices)
                if (device.connected && normalize_address(device.address) != normalize_address(address))
                    others.push_back(device.name.empty() ? std::string("unnamed") : device.name);
            line["other_devices_connected"] = others;
            debug::log(WARN,
                       "bluetooth: the iPhone is here over the classic link but has not come back over LE after {} "
                       "dials; the menu asks for its Bluetooth to be turned off and on",
                       le_recovery.dials());
        } else {
            line["stuck_seconds"] = le_stuck_since >= 0 ? now - le_stuck_since : -1;
            le_stuck_since = -1;
            debug::log(INFO, "bluetooth: the LE link is back after the stuck state");
        }
        std::error_code ec;
        const auto dir = paths::state_dir();
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir / "le-stuck.jsonl", std::ios::app);
        if (out)
            out << line.dump() << '\n';
    }

    // #59: an iPhone that does not come back after the controller was reset.
    void ConnectionState::after_reset_pass(bool le_up) {
        using namespace std::chrono;
        const int64_t now_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        const int64_t back = monitor->adapter_back_at_ms();
        if (back != le_reset_seen_ms) {
            le_reset_seen_ms = back;
            le_restarted_at_ms = -1;
        }
        const bool due = le_after_reset.observe(now_ms, back, le_up);
        bool others = false;
        std::string adapter_path;
        if (due && le_restarted_at_ms < 0) {
            const auto objects = monitor->snapshot();
            for (const auto& device : objects.devices) {
                if (normalize_address(device.address) == normalize_address(address))
                    adapter_path = device.adapter_path;
                // Anything connected stops it, the iPhone itself included: its
                // LE link is down, so a connection is its classic link (a call,
                // its sound), which a restart would cut.
                if (device.connected)
                    others = true;
            }
            if (adapter_path.empty() && !objects.adapters.empty())
                adapter_path = objects.adapters.front().path;
            // An adapter the user turned off stays off.
            const Adapter* adapter = objects.find_adapter(adapter_path);
            if (!adapter || !adapter->powered)
                return;
        }
        int hint = 0;
        switch (after_reset_step(due, others, now_ms, le_restarted_at_ms)) {
        case AfterResetStep::Restart:
            le_restarted_at_ms = now_ms;
            debug::log(INFO,
                       "bluetooth: the iPhone has not come back since the controller was reset and nothing else is "
                       "connected; turning {} off and on",
                       adapter_path);
            restart_adapter(monitor->connection(), adapter_path);
            break;
        case AfterResetStep::AskComputer:
            hint = 1;
            break;
        case AfterResetStep::AskPhone:
            hint = 2;
            break;
        case AfterResetStep::None:
            break;
        }
        if (hint != le_after_reset_hint.exchange(hint))
            debug::log(INFO,
                       "bluetooth: after the controller reset, the menu {}",
                       hint == 1   ? "asks for this computer's Bluetooth to be turned off and on (another device is "
                                     "connected)"
                       : hint == 2 ? "asks for the iPhone's Bluetooth to be turned off and on"
                                   : "says nothing more");
    }

    void ConnectionState::sync_ancs(int64_t now) {
        auto client = ancs();
        if (!client) {
            // No phone to watch for (forgotten, notifications off): no hint.
            le_after_reset_hint = 0;
            return;
        }

        // ANCS rides the LE bearer, but BlueZ keeps exposing the bonded
        // characteristics while that bearer reads disconnected, and the bearer
        // flaps far faster than a GATT subscription can be rebuilt. Dropping the
        // path on every blip resets discovery and the sequencer, so the readiness
        // probe never lives long enough to be answered. Hold the session across a
        // short outage and let discovery decide; a real loss still clears it,
        // because an empty path is what makes the client forget a dead session.
        const bool le_up = bearers->status().le_connected;
        // Before anything below can return: a phone that stays silent after
        // the controller was reset is exactly the case to see (#59).
        after_reset_pass(le_up);
        if (le_up)
            le_down_since = -1;
        else if (le_down_since < 0)
            le_down_since = now;
        // A live subscription outranks the property: an iPhone that is answering
        // ANCS is reachable whatever Bearer.LE1 says, and dropping the path under
        // it would end a session that is working.
        bool ready;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ready = ancs_ready;
        }
        // ponytail: one fixed grace window rather than per-phase tuning. Widen it
        // if a controller is seen flapping slower than this.
        const bool hold = should_hold_ancs(ready, bearers->status(), now, le_down_since);

        std::string device_path;
        bool offers_ancs = false;
        if (hold && bearers->status().device_present) {
            auto objects = monitor->snapshot();
            for (const auto& device : objects.devices) {
                if (device.address == address || normalize_address(device.address) == normalize_address(address)) {
                    device_path = device.path;
                    offers_ancs = device.supports_ancs();
                    break;
                }
            }
        }

        // An LE link the daemon dialled can come up with no ANCS at all: the
        // phone never answered a solicitation, so it never offered the service.
        if (ready || !le_up || offers_ancs) {
            ancs_absent_since = -1;
        } else if (ancs_absent_since < 0) {
            ancs_absent_since = now;
        } else if (now - ancs_absent_since >= ANCS_ABSENT_GRACE_SECONDS) {
            std::lock_guard<std::mutex> lock(status_mutex);
            ancs_reason = _("The iPhone is not offering notifications on this link. Asking it again...");
            if (!current_status.is_null())
                current_status["ancs_reason"] = ancs_reason;
        }

        client->set_device(device_path);
        // The service has just appeared: "Share System Notifications" was
        // turned on. Discover it in this tick, not at the next retry date.
        if (offers_ancs && !ancs_offered && !ready) {
            debug::log(INFO, "ancs: the iPhone now offers its notification service");
            client->retry_now();
        }
        ancs_offered = offers_ancs;
        const uint64_t drops = device_path.empty() ? le_drops_seen : monitor->le_drop_count(device_path);
        if (drops != le_drops_seen) {
            le_drops_seen = drops;
            client->drop_session("LE link dropped");
        }
        // A new LE link: discovery, subscription and the first question go
        // out in this pass, whatever their retry dates had grown to while
        // the phone was away (five seconds after five minutes). BlueZ writes
        // the subscription again by itself when it still holds it; when it
        // does not, this is what subscribes.
        const BluezMonitor::LeLinkUp up = device_path.empty() ? BluezMonitor::LeLinkUp{} : monitor->le_link_up(device_path);
        if (!device_path.empty() && up.count != le_ups_seen) {
            le_ups_seen = up.count;
            if (le_prompt)
                client->retry_now();
        }
        client->tick(now);

        if (device_path.empty()) {
            le_ghost.forget();
            return;
        }

        // #63: BlueZ says up, the notifications never answer: is the link
        // really there? Asked of the kernel, which BlueZ can be out of step with.
        if (le_ghost.check_due(now, le_up, ready)) {
            std::optional<bool> in_kernel;
            std::string adapter_path;
            for (const auto& device : monitor->snapshot().devices)
                if (device.path == device_path)
                    adapter_path = device.adapter_path;
            const std::string adapter_id = adapter_path.substr(adapter_path.find_last_of('/') + 1);
            if (const auto index = hci_index_from(adapter_id))
                in_kernel = le_link_listed(*index, address);
            if (le_ghost.listed(now, in_kernel)) {
                debug::log(WARN,
                           "bluetooth: BlueZ says the LE link is up but the controller holds none and the "
                           "notifications never answered; disconnecting the LE bearer in BlueZ so that it catches up "
                           "(the classic link, if any, is left alone)");
                g_dbus_connection_call(monitor->connection(),
                                       BLUEZ_NAME,
                                       device_path.c_str(),
                                       IFACE_BEARER_LE,
                                       "Disconnect",
                                       nullptr,
                                       nullptr,
                                       G_DBUS_CALL_FLAGS_NONE,
                                       10000,
                                       nullptr,
                                       nullptr,
                                       nullptr);
            }
        }
        using namespace std::chrono;
        const int64_t now_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        const auto step = first_use.observe(up.count, up.at_ms, le_up, client->subscribed(), client->ready(), now_ms);
        if (!step)
            return;
        if (step->kind == LeFirstUse::Event::Kind::Subscribed)
            debug::log(INFO, "bluetooth: notification subscription confirmed {}ms after the LE link came up", step->after_ms);
        else if (step->after_subscribed_ms >= 0)
            debug::log(INFO,
                       "bluetooth: notification service in use {}ms after the LE link came up, {}ms after the "
                       "subscription",
                       step->after_ms,
                       step->after_subscribed_ms);
        else
            debug::log(INFO, "bluetooth: notification service in use {}ms after the LE link came up", step->after_ms);
    }

    // What the request and the watch that follows it have to say.
    void ConnectionState::read_conn_params_news(int64_t now) {
        const LeConnParams& wanted = conn_params.config().wanted;
        while (const auto result = conn_updater->take()) {
            // News of a link that has ended since is of no use.
            if (conn_params_link != le_ups_seen)
                continue;
            switch (result->outcome) {
            case LeConnUpdateOutcome::Changed:
                debug::log(INFO,
                           "bluetooth: the LE connection parameters changed: interval {} ms, latency {}, supervision "
                           "{} ms (handle {}){}",
                           result->got.max_interval_ms,
                           result->got.latency,
                           result->got.timeout_ms,
                           result->handle,
                           result->got.timeout_ms >= wanted.timeout_ms ? ""
                                                                       : "; shorter than asked for, asked again "
                                                                         "within a minute");
                conn_params.report(now, *result);
                continue;
            case LeConnUpdateOutcome::Ended:
                debug::log(INFO,
                           "bluetooth: the LE link whose parameters were watched ended (handle {}, HCI reason {})",
                           result->handle,
                           result->status);
                continue;
            default:
                break;
            }
            const bool last = conn_params.report(now, *result);
            switch (result->outcome) {
            case LeConnUpdateOutcome::Applied:
                debug::log(INFO,
                           "bluetooth: LE connection parameters now in force: interval {} ms, latency {}, supervision "
                           "{} ms (handle {}){}",
                           result->got.max_interval_ms,
                           result->got.latency,
                           result->got.timeout_ms,
                           result->handle,
                           conn_params.settled() ? ""
                           : last              ? "; short of what was asked for, left as they are on this link"
                                               : "; short of what was asked for, asking again");
                break;
            case LeConnUpdateOutcome::NoRight:
                debug::log(INFO,
                           "bluetooth: LE connection parameters cannot be asked for by this process ({}); the link "
                           "keeps the iPhone's own. Not tried again",
                           result->detail);
                break;
            default:
                debug::log(WARN,
                           "bluetooth: LE connection parameters not obtained ({}{}{}){}",
                           to_string(result->outcome),
                           result->status >= 0 ? ", HCI status " + std::to_string(result->status) : std::string(),
                           result->detail.empty() ? std::string() : ": " + result->detail,
                           le_conn_collision(result->status)
                               ? (last ? std::string("; too many collisions, left as they are on this link")
                                       : "; a collision with the iPhone's own procedure, asking again in " +
                                             std::to_string(LE_CONN_PARAMS_COLLISION_RETRY_SECONDS) + "s")
                           : last ? std::string("; left as they are on this link, which is kept")
                                  : "; attempt " + std::to_string(conn_params.attempts()) + " of " +
                                        std::to_string(LE_CONN_PARAMS_ATTEMPTS));
                break;
            }
        }
    }

    // Asks the phone for connection parameters that survive a radio gap, as
    // early as possible on each LE link, and watches what becomes of them.
    // Nothing here ever ends a link.
    void ConnectionState::step_conn_params(int64_t now) {
        if (!conn_params.config().enabled || !conn_updater || conn_params.stopped())
            return;
        read_conn_params_news(now);
        if (conn_params.stopped())
            return;

        LeConnParamsFacts facts;
        facts.now = now;
        facts.le_connected = bearers->status().le_connected;
        facts.link = le_ups_seen;
        facts.busy = conn_updater->requesting();
        // The link the watch was about has ended, or a new one is up: the
        // watch has nothing more to say.
        if (conn_updater->active() && !facts.busy && (!facts.le_connected || conn_params_link != le_ups_seen))
            conn_updater->stop();
        // A watch that ran its course on a link that is still up: once more.
        if (!conn_updater->active() && facts.le_connected && conn_params_link == le_ups_seen && conn_params.settled())
            conn_params.watch_ended(now);
        if (!conn_params.due(facts))
            return;
        const auto index = hci_index_from(monitor->capability().adapter_id);
        if (!index) {
            LeConnUpdateResult none;
            none.detail = "the controller is not known as hciN";
            conn_params.report(now, none);
            return;
        }
        const LeConnParams& wanted = conn_params.config().wanted;
        debug::log(INFO,
                   "bluetooth: asking the iPhone for LE connection parameters {}-{} ms, latency {}, supervision {} ms "
                   "(attempt {} of {}, by {}){}",
                   wanted.min_interval_ms,
                   wanted.max_interval_ms,
                   wanted.latency,
                   wanted.timeout_ms,
                   conn_params.attempts(),
                   LE_CONN_PARAMS_ATTEMPTS,
                   conn_updater->way(),
                   conn_params.config().wait_encryption ? ", once the link is encrypted" : "");
        conn_params_link = le_ups_seen;
        conn_updater->start(*index, address, wanted, conn_params.config().wait_encryption);
    }

    // bluetoothd left the bus and came back: a crash, a restart, an update.
    // The new one knows the bond and nothing else. What this process had
    // registered is gone without a word (the advertisement), what it was
    // waiting for will never be answered (a connect, a subscription), and
    // every link dropped. Nothing is unregistered or stopped here, there is
    // no one to tell: the state is forgotten and the next tick builds it
    // again, LE first.
    void ConnectionState::bluez_restarted() {
        debug::log(WARN,
                   "bluetooth: bluetoothd was restarted; registering the advertisement again and rebuilding "
                   "every session");
        forget_ancs_solicitation(*monitor);
        bearer_ops->forget_classic_refusal();
        bearers->bluez_restarted();
        le_health.reset(le_link_drops_seen);
        le_strikes.reset(le_link_drops_seen);
        profiles->reset();
        map_failures = 0;
        link_was_ready = false;
        // Dated again by the next tick: the advert starts over at its fast interval.
        le_down_since = -1;
        ancs_absent_since = -1;
        seen_without_le_since = -1;
        if (auto client = ancs()) {
            client->drop_session("bluetoothd restarted");
            client->retry_now();
        }
    }

    void ConnectionState::build_ancs_client() {
        auto client = std::make_shared<ancs::AncsClient>(
            *monitor,
            [this](const ancs::Notification& notification) {
                AncsSyncAsk ask;
                {
                    std::lock_guard<std::mutex> lock(triggers_mutex);
                    ask = sync_triggers.on_notification(notification.uid,
                                                        notification.app_id,
                                                        notification.category,
                                                        notification.pre_existing,
                                                        notification.session);
                }
                if (ask.calls_changed)
                    call_history_stale = true;
                if (!ask.reason.empty())
                    ask_message_sync(ask.reason);
                // A call is ringing: said at once, and the classic link is
                // wanted for hands-free. A missed call replayed with the
                // backlog says nothing about the call ringing now.
                const bool about_calls = notification.category == ancs::CategoryId::IncomingCall ||
                                         (notification.category == ancs::CategoryId::MissedCall &&
                                          !notification.pre_existing);
                if (about_calls && ancs_calls && calls_wanted.load()) {
                    IncomingCallTracker::Event event;
                    {
                        std::lock_guard<std::mutex> lock(ring_mutex);
                        event = ring.on_notification(notification.uid,
                                                     notification.category,
                                                     notification.title,
                                                     notification.has_positive_action,
                                                     notification.has_negative_action,
                                                     clock(),
                                                     static_cast<int64_t>(std::time(nullptr)));
                    }
                    publish_ring(event);
                    nudge();
                }
                if (on_notification)
                    on_notification(notification);
            },
            [this](uint32_t uid) {
                AncsSyncAsk ask;
                {
                    std::lock_guard<std::mutex> lock(triggers_mutex);
                    ask = sync_triggers.on_removed(uid);
                }
                if (!ask.reason.empty())
                    ask_message_sync(ask.reason);
                {
                    IncomingCallTracker::Event event;
                    bool following = false;
                    {
                        std::lock_guard<std::mutex> lock(ring_mutex);
                        following = ring.active();
                        event = ring.on_removed(uid, clock());
                    }
                    publish_ring(event);
                    if (following)
                        nudge();
                }
                if (on_withdraw)
                    on_withdraw(uid);
            },
            [this](bool ready, const std::string& reason) {
                {
                    // runs on the glib thread, which must not read supervisor state.
                    std::lock_guard<std::mutex> lock(status_mutex);
                    ancs_ready = ready;
                    ancs_reason = reason;
                    if (!current_status.is_null()) {
                        current_status["ancs_ready"] = ready;
                        current_status["ancs_reason"] = reason;
                    }
                }
                debug::log(INFO, "ancs: {}", reason);
                publish();
            });
        client->set_content_enabled(ancs_content_wanted.load());
        client->set_fast_settle(ancs_first_seconds > 0);
        if (le_prompt)
            client->set_activity_listener([this] { wake_for_event(); });
        std::lock_guard<std::mutex> lock(ancs_mutex);
        ancs_client = std::move(client);
    }

    void ConnectionState::apply_ancs_preference() {
        ancs_preference = ancs_wanted.load();
        // Re-read like the rest: a BR/EDR-only bond is only one to make again
        // on a machine that could carry the LE half.
        bearers->set_le_expected(ancs_available());
        const bool effective = ancs_preference && !hold_ancs.load() && ancs_available();
        if (effective == ancs_enabled) {
            if (auto client = ancs())
                client->set_content_enabled(ancs_content_wanted.load());
            return;
        }

        ancs_enabled = effective;
        bearers->set_ancs_enabled(effective);
        ancs_absent_since = -1;
        if (effective && on_notification) {
            build_ancs_client();
        } else {
            std::lock_guard<std::mutex> lock(ancs_mutex);
            ancs_client.reset();
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ancs_ready = false;
            ancs_reason.clear();
        }
        debug::log(INFO, "bluetooth: notification mirroring {}", effective ? "enabled" : "disabled");
    }

    void ConnectionState::apply_calls_preference() {
        const bool wanted = calls_wanted.load() && on_calls;
        if (wanted == calls_enabled)
            return;
        calls_enabled = wanted;
        bearers->set_calls_enabled(wanted);

        std::shared_ptr<TelephonyClient> client;
        if (wanted) {
            client = make_telephony_client(*monitor, address);
            client->set_call_audio_here(call_audio_here.load());
            // Where the sound of a call wanted here ended up: said to the
            // clients, which otherwise only see it missing.
            client->set_audio_event_handler([this](const std::string& stage, const std::string& reason) {
                if (!on_event)
                    return;
                nlohmann::json event{{"command", "bt_call_progress"}, {"stage", stage}};
                if (!reason.empty())
                    event["reason"] = reason;
                on_event(event);
            });
        }
        {
            std::lock_guard<std::mutex> lock(telephony_mutex);
            telephony = std::move(client);
        }
        std::lock_guard<std::mutex> lock(calls_publish_mutex);
        last_calls = nullptr;
        debug::log(INFO, "bluetooth: call control {}", wanted ? "enabled" : "disabled");
    }

    void ConnectionState::sync_calls() {
        auto client = calls_client();
        std::lock_guard<std::mutex> lock(calls_publish_mutex);
        if (!client) {
            last_calls = nullptr;
            return;
        }
        nlohmann::json calls = client->poll_calls();
        // What hands-free shows of a call that is ringing: "Answer here"
        // comes alive when it shows it.
        if (ancs_calls && calls.is_array()) {
            std::vector<CallSight> sights;
            for (const auto& call : calls)
                sights.push_back({call.value("path", std::string{}), call.value("state", std::string{})});
            bool gateway = !sights.empty();
            if (!gateway) {
                std::lock_guard<std::mutex> status_lock(status_mutex);
                const auto found = current_status.find("calls");
                gateway = found != current_status.end() && found->is_object() &&
                          !found->value("path", std::string{}).empty();
            }
            IncomingCallTracker::Event event;
            {
                std::lock_guard<std::mutex> ring_lock(ring_mutex);
                event = ring.on_calls(sights, gateway, clock());
            }
            publish_ring(event);
        }
        if (calls == last_calls)
            return;
        // Every transition leaves a line: without one, a call that rang with
        // nothing in the journal cannot be told from a call that was never seen.
        for (const std::string& line : describe_call_changes(last_calls, calls))
            debug::log(INFO, "bluetooth: {}", line);
        if (last_calls.is_array() && calls.size() < last_calls.size()) {
            call_history_stale = true;
            // The sessions waited for the call: the link it leaves open is
            // used once for the call history, then closed as usual.
            if (calls.empty() && lean.load() && obex_waits) {
                std::lock_guard<std::mutex> demand_lock(demand_mutex);
                if (demand.take(Lease::Sync, clock()))
                    debug::log(INFO, "bluetooth: the call ended; reading the call history over the link it leaves");
            }
        }
        last_calls = calls;
        if (on_calls)
            on_calls(std::move(calls));
    }

    void ConnectionState::supervise_away_lock(int64_t now, bool obex_up) {
        PresenceInput in;
        in.now = now;
        in.bearer = bearers->status();
        in.obex_up = obex_up;
        in.planned_drop = planned_drop.active(now);
        in.lock_enabled = lock_on_away.load();
        in.grace_seconds = lock_away_seconds.load();
        in.wake_on_return = wake_on_return.load();
        const bool reachable = in.bearer.classic_connected || in.bearer.le_connected || obex_up;
        if (monitor) {
            if (reachable) {
                // A link that came back must not be judged on why the last one ended.
                monitor->clear_disconnect_reason(address);
            } else {
                in.disconnect_reason = monitor->last_disconnect_reason(address);
                const auto cap = monitor->capability();
                in.adapter_powered = cap.adapter_present && cap.powered;
            }
        }

        const PresenceStep step = presence.step(in);
        if (step.lock_now) {
            debug::log(INFO, "bluetooth: iPhone out of range, locking the session");
            const bool locked = lock_session(load_config().lock_command);
            if (!locked)
                debug::log(WARN, "bluetooth: could not lock the session");
            presence.lock_result(locked);
        }
        if (step.wake_now) {
            debug::log(INFO, "bluetooth: iPhone back after an away lock; asking for the login screen");
            wake_session(load_config().wake_command);
        }
        if (step.changed || step.lock_now)
            publish_presence();
    }

    void ConnectionState::publish_presence() {
        nlohmann::json event = presence.to_json();
        debug::log(INFO,
                   "bluetooth: iPhone {}{}{} (lock: {})",
                   event.value("state", ""),
                   event.value("reason", "").empty() ? "" : ", ",
                   event.value("reason", ""),
                   event.value("lock", ""));
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            presence_status = event;
        }
        if (on_event)
            on_event(event);
    }

    LinkMode ConnectionState::configured_link_mode() const {
        static const std::optional<LinkMode> forced = link_mode_from_env(std::getenv("LANDLINE_LINK_MODE"));
        return forced.value_or(static_cast<LinkMode>(link_mode_wanted.load()));
    }

    namespace {
        std::string link_memory_path() {
            const std::filesystem::path dir = paths::state_dir();
            return dir.empty() ? std::string{} : (dir / "link.json").string();
        }

        LinkMemory load_link_memory(const std::string& address) {
            LinkMemory memory;
            memory.address = address;
            const std::string path = link_memory_path();
            if (path.empty())
                return memory;
            std::ifstream in(path);
            if (!in.is_open())
                return memory;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return deserialize_link_memory(text, address);
        }
    } // namespace

    void ConnectionState::save_memory() const {
        const std::string path = link_memory_path();
        if (path.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open())
                return;
            out << serialize_link_memory(memory);
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec)
            std::filesystem::remove(tmp, ec);
    }

    // The last answer of each profile, kept for when the link is closed.
    void ConnectionState::remember_verdicts() {
        const ProfileStatus& sessions = profiles->status();
        const auto verdict = [](bool open, ObexError error, ProfileVerdict current) {
            return open ? ProfileVerdict::Granted : error == ObexError::Forbidden ? ProfileVerdict::Forbidden : current;
        };
        const ProfileVerdict map = verdict(sessions.map_open, sessions.map_error, memory.map);
        const ProfileVerdict pbap = verdict(sessions.pbap_open, sessions.pbap_error, memory.pbap);
        if (map == memory.map && pbap == memory.pbap)
            return;
        const int64_t at = static_cast<int64_t>(std::time(nullptr));
        if (map != memory.map) {
            memory.map = map;
            memory.map_at = at;
        }
        if (pbap != memory.pbap) {
            memory.pbap = pbap;
            memory.pbap_at = at;
        }
        save_memory();
    }

    // Which link mode is applied this tick, and the supervisors told when
    // it changes.
    void ConnectionState::apply_link_mode(int64_t now) {
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ready = ancs_ready;
        }
        // Notifications granted once are counted on across restarts, until
        // the phone has plainly stopped offering them.
        if (ready && !memory.ancs_ready_once) {
            memory.ancs_ready_once = true;
            save_memory();
        } else if (!ready && memory.ancs_ready_once && ancs_absent_since >= 0 &&
                   now - ancs_absent_since >= ANCS_ABSENT_GRACE_SECONDS) {
            memory.ancs_ready_once = false;
            save_memory();
        }
        bool keeps = false;
        {
            std::lock_guard<std::mutex> lock(demand_mutex);
            keeps = demand.phone_keeps_link(now);
        }
        // A bond without an LE half never brings the link lean rests on.
        const bool le_possible = ancs_available() && (!bearer_ops->device_present() || !bearer_ops->device_paired() ||
                                                      bearer_ops->le_bearer_available());
        EffectiveLinkMode next =
            effective_link_mode(configured_link_mode(), le_possible, ancs_enabled, memory.ancs_ready_once, keeps);
        // BlueZ may not have finished enumerating the adapter in the first
        // seconds: "no LE" then is not a verdict, and falling back to the
        // full mode on it would page the phone at every start.
        if (next.reason == "no_le" && now < LINK_MODE_SETTLE_SECONDS)
            next = EffectiveLinkMode{LinkMode::Lean, ""};
        if (link_mode_applied && next == effective)
            return;
        const bool was_applied = link_mode_applied;
        effective = next;
        link_mode_applied = true;
        lean = next.lean();
        debug::log(INFO,
                   "bluetooth: link mode {}{}{}{}",
                   to_string(next.mode),
                   next.lean() ? " (LE kept, the classic link on demand)" : " (both links kept)",
                   next.reason.empty() ? "" : ", instead of lean: ",
                   next.reason);
        bearers->set_link_mode(next.lean());
        profiles->set_on_demand(next.lean());
        std::lock_guard<std::mutex> lock(demand_mutex);
        demand.reset();
        // A link the full mode was keeping is this side's own: used if a
        // lease wants it, then closed, and not held against the iPhone.
        if (next.lean() && was_applied && bearer_ops->classic_connected())
            demand.adopt_engine_link(now);
        demand_state_logged = demand.state();
        sync_edge = false;
        phone_link_seen = false;
        // Asked for under the other mode: not this one's to serve.
        arrival_wanted = false;
        backlog_wanted = false;
    }

    bool ConnectionState::take_lease(Lease lease, int ttl_seconds, bool urgent) {
        bool taken = false;
        {
            std::lock_guard<std::mutex> lock(demand_mutex);
            taken = demand.take(lease, clock(), ttl_seconds, urgent);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            lease_changed = true;
        }
        wake.notify_all();
        return taken;
    }

    void ConnectionState::release_lease(Lease lease) {
        {
            std::lock_guard<std::mutex> lock(demand_mutex);
            demand.release(lease, clock());
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            lease_changed = true;
        }
        wake.notify_all();
    }

    void ConnectionState::nudge() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lease_changed = true;
        }
        wake.notify_all();
    }

    void ConnectionState::publish_ring(const IncomingCallTracker::Event& event) {
        if (!event)
            return;
        debug::log(INFO,
                   "bluetooth: incoming call {} (answer here: {}{})",
                   event->state,
                   event->answer_here,
                   event->path.empty() ? "" : ", seen by hands-free");
        if (on_event)
            on_event(to_json(*event));
    }

    // The clock of a ring: "Answer here" given up on, a ring nobody ended.
    void ConnectionState::step_ring(int64_t now) {
        IncomingCallTracker::Event event;
        {
            std::lock_guard<std::mutex> lock(ring_mutex);
            event = ring.step(now);
        }
        publish_ring(event);
    }

    // Returns the automatic leases once what they opened the link for is done:
    // the mailbox listed and the request settled, the address book looked at.
    void ConnectionState::finish_data_round(int64_t now) {
        {
            std::lock_guard<std::mutex> lock(demand_mutex);
            if (demand.state() != ClassicState::Open ||
                !(demand.holds(Lease::Sync) || demand.holds(Lease::Arrival) || demand.holds(Lease::Hourly)))
                return;
        }
        const ProfileStatus& sessions = profiles->status();
        const bool map_done =
            sessions.map_open
                ? open_map_path == profiles->map_session() && !map_session_new && !message_request.pending()
                : sessions.map_held ||
                      (profiles->demand_attempted() && sessions.map_error != ObexError::DaemonDown);
        const bool pbap_done = sessions.pbap_open ? pulled_pbap_path == profiles->pbap_session() &&
                                                        contacts_due_at < 0 && !call_history_stale.load()
                                                  : sessions.pbap_held ||
                                                        (profiles->demand_attempted() &&
                                                         sessions.pbap_error != ObexError::DaemonDown);
        // obexd being brought back is waited for (18 November, 01:45: the
        // sync was called done while obexd was restarting, and the mailbox
        // was not read). The automatic leases' own lifetime bounds the wait.
        if (!map_done || !pbap_done)
            return;
        debug::log(DEBUG, "bluetooth: sync done; the classic link is no longer needed for it");
        std::lock_guard<std::mutex> lock(demand_mutex);
        demand.release(Lease::Sync, now);
        demand.release(Lease::Arrival, now);
        demand.release(Lease::Hourly, now);
    }

    // The lean link mode, once per pass of the quick steps: what the links
    // are, which automatic leases are due, what the machine decides, done.
    void ConnectionState::step_demand(int64_t now) {
        if (!effective.lean()) {
            bool listed = false;
            {
                std::lock_guard<std::mutex> lock(calls_publish_mutex);
                listed = last_calls.is_array() && !last_calls.empty();
            }
            calls_busy = call_busy(dial_wants_link.load(), false, listed);
            // Nothing to close before a sleep: logind is not kept waiting.
            if (suspended_soon.load())
                sleep_announcer.ready_to_sleep();
            return;
        }
        finish_data_round(now);

        const BearerStatus& bearer = bearers->status();
        const ProfileStatus& sessions = profiles->status();
        DemandFacts facts;
        facts.now = now;
        facts.le_connected = bearer.le_connected;
        facts.classic_connected = bearer.classic_connected;
        facts.map_open = sessions.map_open;
        facts.pbap_open = sessions.pbap_open;
        // Hands-free rides the classic link: not worth asking the stack without one.
        if (bearer.classic_connected) {
            if (auto client = calls_client())
                facts.hands_free = client->present();
        }
        {
            std::lock_guard<std::mutex> lock(calls_publish_mutex);
            facts.call_active = last_calls.is_array() && !last_calls.empty();
        }
        facts.phone_audio_streaming = bearer.classic_connected && bearer_ops->audio_playing();
        facts.suspended_soon = suspended_soon.load();
        facts.data_refused = profiles->demand_refused();
        facts.held_by_other = bearer.classic_connected && bearer_ops->tethering_active();

        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            ready = ancs_ready;
        }

        DemandAction action = DemandAction::None;
        ClassicState state = ClassicState::Idle;
        bool want = false;
        bool keep = false;
        std::string held;
        std::string opened_by;
        {
            std::lock_guard<std::mutex> lock(demand_mutex);
            // The phone's sound here is a standing wish, not a moment.
            // The classic link is opened unasked only on an LE link that has
            // held, and not soon after an opening or a close took it down. A
            // link already there costs the LE link nothing more.
            const bool link_there = demand.state() == ClassicState::Opening || demand.state() == ClassicState::Open ||
                                    demand.state() == ClassicState::Lingering;
            const bool calm = link_there || le_health.auto_open_allowed(now);
            // #43: the choice opens the link when it is made (or asked for
            // again) and while the sound plays, not for ever.
            {
                const bool wanted = audio_here_wanted.load();
                const bool asked = wanted && audio_asked.exchange(false);
                const AudioLeasePlan plan =
                    plan_audio_lease(wanted, asked, facts.phone_audio_streaming, demand.holds(Lease::Audio));
                if (plan.move == AudioLeaseMove::Take && (asked || link_there))
                    demand.take(Lease::Audio, now, plan.ttl_seconds);
                else if (plan.move == AudioLeaseMove::Release)
                    demand.release(Lease::Audio, now);
                if (asked)
                    debug::log(INFO, "bluetooth: the iPhone's sound asked for here; the classic link is offered for {}s",
                               PHONE_AUDIO_OFFER_SECONDS);
            }

            // A call: wanted while one rings and a moment after, while one is
            // being placed, and for as long as the call it brought lasts.
            bool ring_wanted = false;
            bool ring_now = false;
            {
                std::lock_guard<std::mutex> ring_lock(ring_mutex);
                ring_wanted = ring.wants_link(now);
                ring_now = ring.ringing();
            }
            calls_busy = call_busy(dial_wants_link.load(), ring_wanted, facts.call_active);
            demand.set_data_waits_for_calls(obex_waits);
            const bool call_wanted =
                ring_wanted || dial_wants_link.load() || (facts.call_active && demand.holds(Lease::Call));
            if (call_wanted && !demand.holds(Lease::Call))
                demand.take(Lease::Call, now, 0, ring_now);
            else if (!call_wanted && demand.holds(Lease::Call))
                demand.release(Lease::Call, now);

            // The LE link is back with its notifications: one sync, unless
            // the last is recent and nothing says there is something new.
            const bool returned = ready && facts.le_connected;
            const bool edge = returned && calm;
            const bool rising = edge && !sync_edge;
            sync_edge = edge;
            // Kept while the sync waits for the LE link to have held.
            const bool backlog = backlog_wanted && (edge || !returned);
            if (edge || !returned)
                backlog_wanted = false;
            if (returned && !calm && !le_wait_logged) {
                le_wait_logged = true;
                const int64_t quiet = le_health.quiet_until(now);
                if (quiet >= 0)
                    debug::log(INFO,
                               "bluetooth: the classic link is left closed for another {}s (the LE link dropped right "
                               "after it was last opened or closed); the sync waits",
                               quiet - now);
                else
                    debug::log(INFO,
                               "bluetooth: the LE link is back; the classic link is not opened before it has held "
                               "for {}s, the sync waits",
                               le_health.config().stable_seconds);
            } else if (!returned || calm) {
                le_wait_logged = false;
            }
            if (edge && (rising || backlog) && sync_due_on_le_return(now, last_sync_lease_at, backlog) &&
                demand.take(Lease::Sync, now)) {
                last_sync_lease_at = now;
                debug::log(INFO, "bluetooth: sync wanted ({})", backlog ? "notifications waiting on the iPhone" : "the iPhone is back");
            }
            // A new message: the link is opened once the phone has filed it.
            if (arrival_wanted && !facts.le_connected)
                arrival_wanted = false;
            if (arrival_wanted && calm && now - arrival_wanted_at >= MESSAGE_SYNC_DEBOUNCE_SECONDS) {
                arrival_wanted = false;
                if (!demand.take(Lease::Arrival, now))
                    debug::log(INFO,
                               "bluetooth: a message arrived and the classic link is not opened for it ({}); the "
                               "notification stands until the next sync",
                               demand.capped() ? "hourly ceiling reached" : "waiting after a failed opening");
            }
            // The safety net: an hour with nothing listed. Not tied to the
            // notifications being ready: it is what is left without them.
            if (facts.le_connected && calm && demand.state() == ClassicState::Idle) {
                const int64_t reference = std::max({last_sync_at, last_sync_lease_at, last_hourly_lease_at});
                if ((reference >= 0 || reference == SYNC_LONG_AGO) && now - reference >= MESSAGE_BACKGROUND_POLL_SECONDS) {
                    last_hourly_lease_at = now;
                    if (demand.take(Lease::Hourly, now))
                        debug::log(INFO, "bluetooth: hourly sync");
                }
            }

            // #62: an iPhone near that does not come back over LE.
            {
                LeRecoveryFacts rf;
                rf.now = now;
                rf.le_connected = facts.le_connected;
                rf.classic_connected = facts.classic_connected;
                rf.busy = facts.call_active || facts.phone_audio_streaming || facts.held_by_other ||
                          demand.holds(Lease::Call) || facts.suspended_soon;
                const LeRecoveryStep r = le_recovery.step(rf);
                if (facts.classic_connected && demand.holds(Lease::Probe)) {
                    demand.release(Lease::Probe, now);
                    debug::log(INFO, "bluetooth: the iPhone answered over the classic link: it is here");
                }
                if (r.probe) {
                    if (demand.take(Lease::Probe, now))
                        debug::log(INFO,
                                   "bluetooth: no LE link for {}s; paging the iPhone over the classic link to see "
                                   "whether it is here (probe {})",
                                   now - le_recovery.lost_since(),
                                   le_recovery.probes());
                    else
                        debug::log(INFO,
                                   "bluetooth: no LE link; the probe over the classic link waits ({})",
                                   demand.capped() ? "hourly ceiling reached" : "waiting after a failed opening");
                }
                if (r.dial)
                    le_dial_wanted = true;
                if (r.stuck_started || r.stuck_ended) {
                    le_stuck = r.stuck_started;
                    record_le_stuck(r.stuck_started, now, std::string_view(demand.opened_by()) == "phone");
                }
            }

            action = demand.step(facts);
            if (demand.cycled_for_le(now))
                debug::log(INFO,
                           "bluetooth: no LE link for {}s while the classic link is up; closing the classic link once so "
                           "that the iPhone comes back over LE (its leases wait for it)",
                           CLASSIC_LE_CYCLE_SECONDS);

            // A link the iPhone opened is put to use: a sync if the last is old.
            const bool phone_link = std::string_view(demand.opened_by()) == "phone";
            if (phone_link && !phone_link_seen) {
                debug::log(INFO,
                           "bluetooth: the iPhone opened the BR/EDR link itself; using it, then closing it once idle "
                           "(kept up to {}s while the LE link is missing)",
                           PHONE_LINK_WITHOUT_LE_SECONDS);
                if (sync_due_on_le_return(now, std::max(last_sync_at, last_sync_lease_at), false) &&
                    demand.take(Lease::Sync, now))
                    last_sync_lease_at = now;
            }
            phone_link_seen = phone_link;

            state = demand.state();
            want = demand.want_data();
            keep = demand.keep_data();
            opened_by = demand.opened_by();
            for (const Lease lease : demand.leases()) {
                if (!held.empty())
                    held += ", ";
                held += to_string(lease);
            }
        }

        if (state != demand_state_logged || action != DemandAction::None) {
            const char* doing = action == DemandAction::OpenData          ? "; opening for data"
                                : action == DemandAction::OpenVoice       ? "; connecting for hands-free"
                                : action == DemandAction::RecycleForVoice ? "; dropping the data link for hands-free"
                                : action == DemandAction::Close           ? "; closing"
                                                                          : "";
            debug::log(INFO,
                       "bluetooth: classic link {}{} (leases: {}{}{})",
                       to_string(state),
                       doing,
                       held.empty() ? "none" : held,
                       opened_by.empty() ? "" : "; opened by ",
                       opened_by);
            demand_state_logged = state;
        }

        // #62: the LE link dialled from here while the iPhone is there.
        if (le_dial_wanted) {
            le_dial_wanted = false;
            bearers->dial_le();
        }

        // Sessions first: removed before the link under them is dropped.
        profiles->set_demand(want, keep);
        if (!keep)
            forget_map_session();
        bearers->set_demand(action, now);
        if (action == DemandAction::OpenData || action == DemandAction::OpenVoice)
            le_health.note_classic(ClassicTouch::Opened, now);
        else if (action == DemandAction::Close || action == DemandAction::RecycleForVoice)
            le_health.note_classic(ClassicTouch::Closed, now);

        // Before a sleep: logind goes on once nothing is left to close.
        if (facts.suspended_soon && state != ClassicState::Closing && action == DemandAction::None)
            sleep_announcer.ready_to_sleep();
    }

    void ConnectionState::run() {
        using namespace std::chrono;
        const auto started = steady_clock::now();
        run_started = started;

        // One tick, in the order run_supervision_tick() fixes.
        struct Tick final : SupervisionSteps {
            Tick(ConnectionState& state, steady_clock::time_point started) : s(state), started(started) {
                read_clock();
                s.lease_changed = false;
                s.date_sync_asks(now);
                s.apply_link_mode(now);
            }

            // links() runs several times in a tick that held the thread, and
            // each run must judge by the time it is, not the time it was.
            void read_clock() {
                now = duration_cast<seconds>(steady_clock::now() - started).count();
                s.tick_now = now;
            }

            // Whether a step held the thread long enough for the links to have
            // moved under it.
            static bool held_thread(const std::function<void()>& step) {
                const auto before = steady_clock::now();
                step();
                return steady_clock::now() - before >= milliseconds(LONG_STEP_MS);
            }

            void links() override {
                read_clock();
                const uint64_t generation = s.monitor->bluez_generation();
                if (generation != s.bluez_generation_seen) {
                    s.bluez_generation_seen = generation;
                    s.bluez_restarted();
                }
                s.watch_sleep(now);
                s.watch_strangers(now);
                s.watch_classic_stuck(now);
                // The BR/EDR link has dropped since the last look. The OBEX
                // sessions that rode on it are forgotten before the supervisor
                // runs, not after: it took them for a link still up, asked
                // BlueZ for BR/EDR, and only the next pass began the hold
                // that was meant to leave BR/EDR alone.
                if (s.le_first.strict_hold && s.link_was_ready && !s.bearer_ops->classic_connected()) {
                    s.profiles->reset();
                    s.bearers->reset();
                    s.map_failures = 0;
                    s.link_was_ready = false;
                }
                obex_up = s.profiles->status().map_open || s.profiles->status().pbap_open;

                if (const int64_t commanded = s.call_command_at.load(); commanded >= 0)
                    s.bearers->note_call_command(commanded);
                s.bearers->tick(now, obex_up);
                s.watch_le_health(now);
                if (s.bearers->status().le_connected)
                    s.le_seen_since_resume = true;
                // The supervisor has just cut BR/EDR for hands-free. What was
                // open goes on being published as open while it comes back.
                if (s.bearers->planned_drops() != s.hfp_cycles_seen) {
                    s.hfp_cycles_seen = s.bearers->planned_drops();
                    const auto& open = s.profiles->status();
                    s.planned_drop.until = now + HFP_CYCLE_MASK_SECONDS;
                    s.planned_drop.map = open.map_open;
                    s.planned_drop.pbap = open.pbap_open;
                    s.planned_drop.reason = open.reason;
                    s.planned_drop.fell = false;
                }
                s.planned_drop.observe(s.profiles->status().map_open, s.profiles->status().pbap_open, now);
                const bool present = s.bearers->status().device_present;
                const bool link_ready = s.bearers->status().classic_connected;

                // A phone that walks back into range must not sit out a backoff that
                // grew to minutes while it was gone.
                if (present && !s.device_was_present)
                    s.bearers->reset();
                s.device_was_present = present;

                // A dropped Classic link invalidates every OBEX session on top of it,
                // along with any backoff accumulated against the link that just died.
                if (s.link_was_ready && !link_ready) {
                    s.profiles->reset();
                    s.bearers->reset();
                    s.map_failures = 0;
                }
                s.link_was_ready = link_ready;
            }

            void notifications() override {
                s.sync_ancs(now);
                s.step_conn_params(now);
            }

            void demand() override {
                read_clock();
                s.step_ring(now);
                s.step_demand(now);
            }

            void calls() override { s.sync_calls(); }

            // publish() drops a payload identical to the last one, so refreshing
            // more than once a tick costs nothing and cannot miss a transition.
            void publish() override {
                s.refresh_status();
                s.publish();
            }

            bool profiles() override {
                bool changed = false;
                const bool held = held_thread([&] { changed = open_profiles(); });
                return held || changed;
            }

            // `apply_holds` is false from inside a long read: releasing a
            // profile there would remove the session being read.
            bool open_profiles(bool apply_holds = true) {
                if (apply_holds) {
                    const bool inactive = s.yield_when_inactive.load() && session_inactive();
                    s.profiles->set_held(s.hold_map.load() || inactive, s.hold_pbap.load() || inactive, inactive);
                }
                // Every obexd session went with the bus, and the daemon is
                // about to leave (see BusWatch). Say why until it does.
                if (s.profile_ops->bus_closed()) {
                    if (!s.bus_closed_logged)
                        debug::log(ERR, "bluetooth: the session bus connection is closed; messages and contacts are lost");
                    s.bus_closed_logged = true;
                    s.profiles->bus_lost();
                }
                if (s.retry_requested.exchange(false)) {
                    debug::log(INFO, "bluetooth: a client asked for the profiles to be tried now");
                    s.profiles->retry_now(now);
                    if (auto client = s.ancs())
                        client->retry_now();
                }
                // Without an LE bearer to watch, or with the spacing switched
                // off, the phone is always taken to be reachable.
                const auto& bearer = s.bearers->status();
                static const bool spaced = obex_absent_backoff_enabled(std::getenv("LANDLINE_OBEX_ABSENT_BACKOFF"));
                const bool reachable =
                    !spaced || bearer.classic_connected || bearer.le_connected || !bearer.le_available;
                // A broken bond between two windows: an attempt only makes
                // obexd page the phone for another refusal. What is open stays.
                const bool ready = bearer.device_paired &&
                                   should_open_profiles(bearer, s.profiles->status().map_open ||
                                                                    s.profiles->status().pbap_open);
                const bool changed = s.profiles->tick(now, ready, reachable, bearer.classic_connected);
                s.remember_verdicts();
                return changed;
            }

            // Runs from inside a long OBEX read, each time the transfer is
            // found still in progress. The read holds this thread for seconds
            // (an address book of 767 entries: about 11 s), and what follows
            // used to wait for it: the attempt on the other profile, which
            // the user may have just allowed on the phone, and the
            // notification client. Neither touches the session being read.
            void during_long_read() {
                const auto at = steady_clock::now();
                if (at < s.next_idle_pass)
                    return;
                s.next_idle_pass = at + seconds(SUPERVISOR_TICK_SECONDS);
                read_clock();
                if (auto client = s.ancs())
                    client->tick(now);
                if (open_profiles(false))
                    publish();
            }

            bool long_read(const std::function<void()>& step) {
                if (!s.long_read_attempts)
                    return held_thread(step);
                TransferIdleScope scope([this] { during_long_read(); });
                return held_thread(step);
            }

            void presence() override {
                const auto& bearer = s.bearers->status();
                s.supervise_away_lock(now, obex_up);
                // A phone BR/EDR reaches while LE is down is here, and the
                // advert is what brings its LE link.
                if (bearer.le_connected || !(bearer.classic_connected || obex_up))
                    s.seen_without_le_since = -1;
                else if (s.seen_without_le_since < 0)
                    s.seen_without_le_since = now;
                // Fast for the first minutes after the LE link went down, which
                // sync_ancs() has just dated, then at the steady interval for
                // as long as the phone stays away.
                supervise_ancs_solicitation(
                    *s.monitor,
                    should_solicit_ancs(s.ancs_enabled, bearer, now, s.ancs_absent_since) &&
                        !s.le_health.advert_paused(now) && !s.le_strikes.advert_paused(now),
                    // After a link that ended unused, fast for a moment then
                    // 152.5 ms until one has served; slow while links that
                    // served keep dropping: the phone is at the edge of range
                    // (le_strikes.hpp).
                    advert_interval_with_strikes(s.le_strikes,
                                                 now,
                                                 s.le_down_since,
                                                 s.advert_fast_seconds,
                                                 s.seen_without_le_since,
                                                 s.bearers->advert_rearmed_at()));
            }

            bool long_reads_allowed() override {
                // A call first: a long read holds this thread, and the call
                // list with it, for as long as the phone takes to answer.
                if (s.obex_waits && s.calls_busy.load()) {
                    if (!s.obex_wait_logged)
                        debug::log(INFO, "bluetooth: a call is in hand; address book, call history and messages wait for it");
                    s.obex_wait_logged = true;
                    return false;
                }
                s.obex_wait_logged = false;
                bool ready = false;
                {
                    std::lock_guard<std::mutex> lock(s.status_mutex);
                    ready = s.ancs_ready;
                }
                const bool wanted = s.ancs_enabled && s.ancs() != nullptr;
                const bool held = s.notification_priority.hold(
                    wanted, ready, s.bearers->status().le_connected, now, s.ancs_first_seconds);
                if (held && !s.long_reads_held)
                    debug::log(INFO,
                               "bluetooth: notifications first; address book and messages wait up to {}s for them",
                               s.ancs_first_seconds);
                s.long_reads_held = held;
                return !held;
            }

            bool contacts() override {
                return long_read([&] { s.sync_contacts(now); });
            }

            bool call_history() override {
                return long_read([&] { s.sync_call_history(); });
            }

            bool messages() override {
                return long_read([&] { s.sync_messages(now); });
            }

            ConnectionState& s;
            steady_clock::time_point started;
            int64_t now = 0;
            bool obex_up = false;
        };

        while (running) {
            apply_ancs_preference();
            apply_calls_preference();

            // Cleared before the pass: what happens during it starts another.
            const auto pass_started = steady_clock::now();
            link_event = false;
            Tick tick(*this, started);
            run_supervision_tick(tick);

            std::unique_lock<std::mutex> lock(mutex);
            // Under a call the list is read four times a second: its states
            // (dialing, alerting, active) are what the user is looking at.
            const auto pause = calls_busy.load() ? milliseconds(CALL_TICK_MS)
                                                 : duration_cast<milliseconds>(seconds(SUPERVISOR_TICK_SECONDS));
            wake.wait_for(lock, pause, [this] {
                return !running || retry_requested.load() || sync_asked.load() || lease_changed.load() ||
                       link_event.load();
            });
            // An LE link came up or ended, or the phone sent a packet: the
            // pass starts now, but never two of them back to back.
            if (running && link_event.load()) {
                const int wait = event_pass_wait_ms(
                    duration_cast<milliseconds>(steady_clock::now() - pass_started).count());
                if (wait > 0)
                    wake.wait_for(lock, milliseconds(wait), [this] { return !running.load(); });
            }
        }
    }

    ConnectionManager::ConnectionManager(BluezMonitor& monitor, StatusFn on_change, MessageFn on_message)
        : state_(std::make_unique<ConnectionState>()) {
        state_->monitor = &monitor;
        state_->on_change = std::move(on_change);
        state_->on_message = std::move(on_message);
    }

    ConnectionManager::~ConnectionManager() { stop(); }

    bool ConnectionManager::start(const std::string& address, bool ancs_enabled) {
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        return start_locked(address, ancs_enabled);
    }

    void ConnectionManager::stop() {
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        stop_locked();
    }

    bool ConnectionManager::start_locked(const std::string& address, bool ancs_enabled) {
        if (state_->running)
            return true;
        if (address.empty()) {
            debug::log(INFO, "bluetooth: no paired iPhone selected; supervision idle");
            return false;
        }

        state_->address = address;
        state_->ancs_preference = ancs_enabled;
        state_->ancs_wanted = ancs_enabled;
        const Config config = load_config();
        state_->ancs_content_wanted = config.ancs_content_enabled;
        state_->calls_wanted = config.calls_enabled;
        state_->call_audio_here = config.call_audio_here;
        state_->lock_on_away = config.lock_on_away;
        state_->lock_away_seconds = config.lock_away_seconds;
        state_->wake_on_return = config.wake_on_return;
        // A pause in the supervision of the same phone keeps what was already
        // announced about its battery; another phone starts from nothing.
        if (!same_address(state_->watched_address, address)) {
            state_->battery_watch.reset();
            state_->charge_watch.reset();
            state_->watched_address = address;
        }
        state_->yield_when_inactive = config.yield_when_inactive;
        state_->link_mode_wanted = static_cast<int>(config.link_mode);
        state_->audio_here_wanted = config.phone_audio == PhoneAudio::Here;
        state_->memory = load_link_memory(address);
        state_->effective = EffectiveLinkMode{LinkMode::Full, ""};
        state_->link_mode_applied = false;
        state_->lean = false;
        state_->suspended_soon = false;
        {
            std::lock_guard<std::mutex> lock(state_->demand_mutex);
            state_->demand = ClassicDemand{};
        }
        state_->demand_state_logged = ClassicState::Idle;
        state_->sync_edge = false;
        state_->phone_link_seen = false;
        state_->last_sync_lease_at = -1;
        state_->last_hourly_lease_at = -1;
        state_->last_sync_at = -1;
        state_->arrival_wanted = false;
        state_->backlog_wanted = false;
        state_->ancs_calls = ancs_calls_enabled(std::getenv("LANDLINE_ANCS_CALLS"));
        {
            std::lock_guard<std::mutex> lock(state_->ring_mutex);
            state_->ring.reset();
        }
        state_->dial_wants_link = false;
        state_->call_command_at = -1;
        // Read the controller's capability here rather than letting each caller
        // latch its own copy. At startup BlueZ may not have finished enumerating
        // the adapter, and its free advertising-instance count drops to zero
        // while one of our own adverts is registered — a caller that sampled
        // either would disable mirroring for the rest of the daemon's life.
        const bool ancs_effective = ancs_enabled && ancs_available();
        state_->ancs_enabled = ancs_effective;

        // Replay history before the first poll so the store is never briefly
        // empty, and so previously seen handles dedupe instead of re-announcing.
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store() = load_contacts();
            restore_removed_threads();
            {
                // The recent calls as last read, so the list is there before
                // any link is opened.
                std::vector<VCard> calls = load_call_history();
                state_->call_history_known = !calls.empty();
                std::lock_guard<std::mutex> history_lock(state_->call_history_mutex);
                state_->call_history = std::move(calls);
            }
            if (g_journal.open()) {
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                auto history = g_journal.load(now);
                for (const auto& message : history)
                    g_messages.add(message);
                g_journal.compact(history);
                debug::log(INFO, "bluetooth: replayed {} message(s) from the journal", history.size());
            }
        }
        state_->bearer_ops = std::make_unique<BluezBearerOps>(*state_->monitor, address);
        state_->bearer_ops->set_telephony_probe([state = state_.get()] {
            auto client = state->calls_client();
            return client && client->present();
        });
        state_->ancs_first_seconds =
            ancs_first_seconds_from(std::getenv("LANDLINE_ANCS_FIRST_SECONDS"), ANCS_FIRST_SECONDS);
        state_->advert_fast_seconds =
            advert_fast_seconds_from(std::getenv("LANDLINE_ADVERT_FAST_SECONDS"), ADVERT_FAST_SECONDS);
        state_->seen_without_le_since = -1;
        state_->notification_priority = NotificationPriority{};
        state_->long_reads_held = false;
        state_->profile_ops = std::make_unique<ObexProfileOps>(address);
        state_->bearers = std::make_unique<BearerSupervisor>(*state_->bearer_ops, ancs_effective);
        state_->bearers->set_hfp_absent_seconds(
            hfp_absent_seconds_from(std::getenv("LANDLINE_HFP_ABSENT_SECONDS"), HFP_ABSENT_SECONDS));
        state_->bearers->set_hfp_request_enabled(hfp_request_enabled(std::getenv("LANDLINE_HFP_CONNECT_PROFILE")));
        const LeFirstConfig le_first = le_first_config_from(std::getenv("LANDLINE_LE_FIRST"),
                                                            std::getenv("LANDLINE_LE_FIRST_HOLD_SECONDS"),
                                                            std::getenv("LANDLINE_LE_RECYCLE_SECONDS"),
                                                            std::getenv("LANDLINE_LE_FIRST_STRICT"),
                                                            std::getenv("LANDLINE_LE_SETTLE_SECONDS"));
        state_->bearers->set_le_first(le_first);
        state_->le_first = le_first;
        state_->resume_watch = resume_watch_enabled(std::getenv("LANDLINE_RESUME_WATCH"));
        state_->le_health = LeHealth{};
        state_->le_prompt = le_prompt_enabled(std::getenv("LANDLINE_LE_PROMPT"));
        state_->le_ups_seen = 0;
        state_->first_use = LeFirstUse{};
        state_->link_event = false;
        {
            const LeConnParamsConfig wanted = le_conn_params_config_from(std::getenv("LANDLINE_LE_CONN_PARAMS"),
                                                                         std::getenv("LANDLINE_LE_CONN_INTERVAL_MS"),
                                                                         std::getenv("LANDLINE_LE_CONN_LATENCY"),
                                                                         std::getenv("LANDLINE_LE_CONN_TIMEOUT_MS"),
                                                                         std::getenv("LANDLINE_LE_CONN_DELAY_SECONDS"),
                                                                         std::getenv("LANDLINE_LE_CONN_ENCRYPTED"));
            if (state_->conn_updater)
                state_->conn_updater->stop();
            state_->conn_updater.reset();
            state_->conn_params = LeConnParamsPlan{};
            state_->conn_params_link = 0;
            LeConnParamsConfig config = wanted;
            if (wanted.enabled) {
                if (!wanted.note.empty())
                    debug::log(WARN,
                               "bluetooth: the LE connection parameters set in the environment are not ones an "
                               "iPhone takes ({}); the defaults are used",
                               wanted.note);
                state_->conn_updater = std::make_unique<LeConnUpdater>();
                if (state_->conn_updater->able()) {
                    debug::log(INFO,
                               "bluetooth: LE connection parameters are asked for {} on each LE link ({}-{} ms, "
                               "latency {}, supervision {} ms), by {}",
                               wanted.delay_seconds > 0 ? std::to_string(wanted.delay_seconds) + "s in"
                               : wanted.wait_encryption ? std::string("as soon as it is encrypted")
                                                        : std::string("as soon as it is up"),
                               wanted.wanted.min_interval_ms,
                               wanted.wanted.max_interval_ms,
                               wanted.wanted.latency,
                               wanted.wanted.timeout_ms,
                               state_->conn_updater->way());
                } else {
                    // Said once, and not an error: nothing is asked.
                    debug::log(INFO,
                               "bluetooth: LE connection parameters are not asked for: this process holds no "
                               "CAP_NET_RAW and there is no usable helper at {} (\"landline-setup apply\" installs "
                               "it). The LE link keeps the iPhone's own parameters",
                               state_->conn_updater->helper());
                    state_->conn_updater.reset();
                }
            }
            state_->conn_params.configure(config);
            if (wanted.enabled && !state_->conn_updater) {
                // Published as no_right from the start.
                LeConnUpdateResult none;
                none.outcome = LeConnUpdateOutcome::NoRight;
                state_->conn_params.report(0, none);
            }
        }
        state_->le_strikes = LeStrikes{};
        state_->le_strikes.configure(le_strikes_config_from(std::getenv("LANDLINE_LE_STRIKES"),
                                                            std::getenv("LANDLINE_LE_STRIKE_PAUSE_SECONDS"),
                                                            std::getenv("LANDLINE_LE_STRIKE_LONG_PAUSE_SECONDS"),
                                                            std::getenv("LANDLINE_LE_SERVED_SECONDS"),
                                                            std::getenv("LANDLINE_LE_STRIKE_FAST_SECONDS"),
                                                            std::getenv("LANDLINE_LE_STRIKE_SLOW"),
                                                            std::getenv("LANDLINE_LE_STRIKE_BRAKE")));
        {
            LeHealthConfig health = le_health_config_from(std::getenv("LANDLINE_LE_STABLE_SECONDS"),
                                                          std::getenv("LANDLINE_CLASSIC_QUIET_SECONDS"),
                                                          std::getenv("LANDLINE_LE_BURST_PAUSE"));
            // The strike count has taken over from the pause after three
            // drops in a minute, which is still written in the journal.
            // LANDLINE_LE_STRIKES=0 gives the old rule back.
            if (state_->le_strikes.config().enabled)
                health.burst_pause = false;
            else
                debug::log(INFO, "bluetooth: unused LE links are not counted (LANDLINE_LE_STRIKES=0)");
            state_->le_health.configure(health);
        }
        state_->le_link_drops_seen = 0;
        state_->le_wait_logged = false;
        state_->sleep_watch = SleepWatch{};
        state_->resumed_at = -1;
        state_->le_seen_since_resume = true;
        state_->stranger_watch = le_stranger_enabled(std::getenv("LANDLINE_LE_STRANGER"));
        state_->strangers_seen = state_->monitor->le_stranger().count;
        state_->stranger_at = -1;
        state_->stranger_rearmed_at = -1;
        state_->le_state_logged = LeState{};
        if (le_first == le_first_off())
            debug::log(INFO, "bluetooth: LE-first measures are off (LANDLINE_LE_FIRST=0)");
        // Read on the supervising thread, which is the one that writes it.
        state_->bearer_ops->set_call_probe(
            [state = state_.get()] { return state->last_calls.is_array() && !state->last_calls.empty(); });
        state_->bluez_generation_seen = state_->monitor->bluez_generation();
        state_->profiles = std::make_unique<ProfileSupervisor>(*state_->profile_ops);
        state_->profiles->set_forbidden_fast_seconds(
            forbidden_fast_seconds_from(std::getenv("LANDLINE_FORBIDDEN_FAST_SECONDS"), PROFILE_FORBIDDEN_FAST_SECONDS));
        // A session that has just opened is published before the attempt on
        // the other profile, which can wait on obexd, and before any read.
        state_->profiles->set_opened_handler([state = state_.get()] {
            // The session object first: a message waiting to be sent goes
            // out now, not after the reads that follow.
            state->adopt_map_session();
            state->refresh_status();
            state->publish();
        });
        state_->long_read_attempts = long_read_attempts_enabled(std::getenv("LANDLINE_LONG_READ_ATTEMPTS"));
        state_->retry_requested = false;
        state_->ancs_offered = false;
        state_->bus_closed_logged = false;

        // Only built when the caller can actually show notifications and the
        // bond can carry them.
        if (ancs_effective && state_->on_notification)
            state_->build_ancs_client();

        // The computer's sleep, announced: the classic link is closed first,
        // so BlueZ has nothing to bring back on wake.
        if (sleep_close_enabled(std::getenv("LANDLINE_SLEEP_CLOSE"))) {
            BluezMonitor* monitor = state_->monitor;
            state_->sleep_announcer.start(
                monitor->connection(),
                [monitor](const std::function<void()>& fn) { monitor->invoke_sync(fn); },
                [state = state_.get()](bool sleeping) {
                    state->suspended_soon = sleeping;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->lease_changed = true;
                    }
                    state->wake.notify_all();
                });
        }

        if (state_->le_prompt)
            state_->monitor->set_link_listener([state = state_.get()] { state->wake_for_event(); });
        else
            debug::log(INFO, "bluetooth: LE links are looked at once a second (LANDLINE_LE_PROMPT=0)");

        state_->running = true;
        state_->thread = std::thread([this] { state_->run(); });
        debug::log(INFO, "bluetooth: supervising {} (ANCS {})", address, ancs_effective ? "enabled" : "disabled");
        return true;
    }

    void ConnectionManager::stop_locked() {
        if (!state_->running && !state_->thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->running = false;
        }
        state_->wake.notify_all();
        if (state_->thread.joinable())
            state_->thread.join();
        state_->monitor->set_link_listener({});
        state_->sleep_announcer.stop();
        state_->lean = false;

        // MapSession borrows profile_ops' bus connection, which is released
        // below. Drop the session before anything can hand out a pointer into a
        // connection that is about to go away.
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            g_map_session.reset();
        }

        // Everything below is scoped to one device and one set of sessions, and
        // must not leak into the next start().
        {
            std::lock_guard<std::mutex> lock(state_->ancs_mutex);
            state_->ancs_client.reset();
        }

        {
            std::lock_guard<std::mutex> lock(state_->telephony_mutex);
            state_->telephony.reset();
        }
        state_->calls_enabled = false;
        state_->last_calls = nullptr;

        // The supervisors borrow the ops objects — ~ProfileSupervisor removes the
        // obexd sessions through them — so they go first, and start() only ever
        // builds into empty pointers.
        state_->profiles.reset();
        state_->bearers.reset();
        state_->profile_ops.reset();
        state_->bearer_ops.reset();

        state_->open_map_path.clear();
        state_->pulled_pbap_path.clear();
        state_->contacts_pulled_at = -1;
        state_->contacts_size_at_pull = -1;
        state_->contacts_due_at = -1;
        state_->planned_drop.clear();
        state_->hfp_cycles_seen = 0;
        {
            std::lock_guard<std::mutex> lock(state_->call_history_mutex);
            state_->call_history.clear();
        }
        state_->map_session_backfill = true;
        state_->map_failures = 0;
        state_->last_message_poll = -1;
        state_->map_session_new = true;
        state_->message_request.clear();
        {
            std::lock_guard<std::mutex> lock(state_->sync_ask_mutex);
            state_->sync_asked = false;
            state_->sync_ask_reason.clear();
            state_->sync_ask_expects_new = false;
        }
        {
            std::lock_guard<std::mutex> lock(state_->triggers_mutex);
            state_->sync_triggers.reset();
        }
        state_->link_was_ready = false;
        state_->device_was_present = false;
        state_->ancs_absent_since = -1;
        // Nothing is known of a phone that is no longer followed. Not
        // broadcast: the bt_status that goes with a stop says why.
        state_->presence.reset();
        {
            std::lock_guard<std::mutex> lock(state_->status_mutex);
            state_->presence_status = nullptr;
            state_->current_status = nullptr;
            state_->last_published = nullptr;
            state_->ancs_ready = false;
            state_->ancs_reason.clear();
        }
    }

    void ConnectionManager::set_notification_handlers(NotificationFn on_notification, WithdrawFn on_withdraw) {
        state_->on_notification = std::move(on_notification);
        state_->on_withdraw = std::move(on_withdraw);
    }

    void ConnectionManager::set_ancs_content_enabled(bool enabled) { state_->ancs_content_wanted = enabled; }

    bool ConnectionManager::perform_notification_action(uint32_t uid, ancs::ActionId action) {
        auto client = state_->ancs();
        return client && client->perform_action(uid, action);
    }

    nlohmann::json ConnectionManager::notifications(size_t limit) const {
        nlohmann::json out = nlohmann::json::array();
        auto client = state_->ancs();
        if (!client)
            return out;
        for (const auto& notification : client->recent_notifications(limit))
            out.push_back(ancs::to_json(notification));
        return out;
    }

    void ConnectionManager::set_call_handler(CallsFn on_calls) { state_->on_calls = std::move(on_calls); }

    void ConnectionManager::refresh_calls() { state_->sync_calls(); }

    void ConnectionManager::set_call_history_handler(CallsFn on_history) {
        state_->on_call_history = std::move(on_history);
    }

    nlohmann::json ConnectionManager::call_history() const { return state_->call_history_json(); }

    void ConnectionManager::set_calls_enabled(bool enabled) { state_->calls_wanted = enabled; }

    void ConnectionManager::set_call_audio_here(bool here) {
        state_->call_audio_here = here;
        if (auto client = state_->calls_client())
            client->set_call_audio_here(here);
    }

    bool ConnectionManager::set_hold(const std::string& profile, bool held) {
        const bool all = profile == "all";
        if (!all && profile != "map" && profile != "pbap" && profile != "ancs")
            return false;
        if (all || profile == "map")
            state_->hold_map = held;
        if (all || profile == "pbap")
            state_->hold_pbap = held;
        if (all || profile == "ancs")
            state_->hold_ancs = held;
        return true;
    }

    void ConnectionManager::set_yield_when_inactive(bool enabled) { state_->yield_when_inactive = enabled; }

    void ConnectionManager::request_message_sync(std::string_view reason) {
        if (state_->running)
            state_->ask_message_sync(reason);
    }

    void ConnectionManager::mark_call_history_stale() { state_->call_history_stale = true; }

    bool ConnectionManager::on_demand() const { return state_->running && state_->lean.load(); }

    void ConnectionManager::set_link_mode(LinkMode mode) {
        state_->link_mode_wanted = static_cast<int>(mode);
        if (!state_->running)
            return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->lease_changed = true;
        }
        state_->wake.notify_all();
    }

    bool ConnectionManager::take_lease(Lease lease, bool urgent) {
        return state_->running && state_->take_lease(lease, 0, urgent);
    }

    void ConnectionManager::release_lease(Lease lease) {
        if (state_->running)
            state_->release_lease(lease);
    }

    bool ConnectionManager::client_lease(const std::string& name, bool held) {
        Lease lease;
        if (name == "reading")
            lease = Lease::Reading;
        else if (name == "setup")
            lease = Lease::Setup;
        else
            return false;
        if (!state_->running)
            return false;
        if (held)
            state_->take_lease(lease, CLIENT_LEASE_SECONDS);
        else
            state_->release_lease(lease);
        return true;
    }

    void ConnectionManager::ask_phone_audio() {
        if (!state_->audio_here_wanted.load())
            return;
        state_->audio_asked = true;
        state_->wake.notify_all();
    }

    bool ConnectionManager::set_phone_audio_here(bool here) {
        state_->audio_here_wanted = here;
        if (!here)
            state_->audio_asked = false;
        if (!state_->running)
            return false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->lease_changed = true;
        }
        state_->wake.notify_all();
        return state_->lean.load();
    }

    std::shared_ptr<MapSession> ConnectionManager::await_messages(int seconds, std::string& err) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + std::chrono::seconds(seconds);
        // Not before the supervisor has had a tick to start the opening.
        const auto judged_from = steady_clock::now() + milliseconds(1500);
        while (state_->running && steady_clock::now() < deadline) {
            if (auto session = current_map_session())
                return session;
            if (steady_clock::now() >= judged_from) {
                std::lock_guard<std::mutex> lock(state_->status_mutex);
                const nlohmann::json& status = state_->current_status;
                if (status.is_object()) {
                    const std::string classic = status.value("classic_state", "");
                    const std::string messages = status.value("messages", "");
                    // The iPhone's switch is off: no wait will change that.
                    if (messages == "forbidden" && (classic == "open" || classic == "failed")) {
                        err = status.value("profile_reason", "");
                        if (err.empty())
                            err = _("Messages are not connected.");
                        return nullptr;
                    }
                    if (classic == "failed")
                        break;
                }
            }
            std::this_thread::sleep_for(milliseconds(100));
        }
        err = _("Couldn’t reach the iPhone to send.");
        return nullptr;
    }

    bool ConnectionManager::retry_profiles() {
        if (!state_->running)
            return false;
        // In the lean link mode there is nothing to retry on a closed link:
        // whoever asks is looking, so the link is opened for a moment.
        if (state_->lean.load())
            state_->take_lease(Lease::Reading, RETRY_LEASE_SECONDS);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->retry_requested = true;
        }
        state_->wake.notify_all();
        return true;
    }

    void ConnectionManager::set_lock_on_away(bool enabled, int grace_seconds, bool wake_on_return) {
        state_->lock_on_away = enabled;
        state_->lock_away_seconds = grace_seconds;
        state_->wake_on_return = wake_on_return;
    }

    void ConnectionManager::set_event_handler(EventFn on_event) { state_->on_event = std::move(on_event); }

    nlohmann::json ConnectionManager::presence() const {
        std::lock_guard<std::mutex> lock(state_->status_mutex);
        if (state_->presence_status.is_null())
            return PresenceTracker{}.to_json();
        return state_->presence_status;
    }

    bool ConnectionManager::dial(const std::string& number, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        state_->call_command_at = state_->clock();
        return client->dial(number, err);
    }

    namespace {
        std::string dial_failure_message(const std::string& reason) {
            if (reason == "calls_off")
                return _("Call control is off.");
            if (reason == "busy")
                return _("Another call is already being placed.");
            if (reason == "cancelled")
                return _("The call was cancelled.");
            if (reason == "no_hands_free")
                return _("The iPhone has not connected Hands-Free to this computer.");
            return _("Couldn’t reach the iPhone.");
        }
    } // namespace

    // Off the caller's thread from the first line: the link may have to be
    // opened and hands-free waited for, and the event loop serves the other
    // clients meanwhile.
    void ConnectionManager::dial_async(const std::string& number,
                                       const std::string& audio,
                                       const std::string& operation_id,
                                       DialProgressFn progress,
                                       DialResultFn done) {
        ConnectionState* state = state_.get();
        std::thread([state, number, audio, operation_id, progress = std::move(progress), done = std::move(done)]() {
            using namespace std::chrono;
            const auto report = [&done](bool ok, const std::string& reason, const std::string& message) {
                if (done)
                    done(ok, reason, message);
            };
            if (normalize_dial_string(number).empty()) {
                report(false, "refused", _("Not a dialable number."));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(state->dial_mutex);
                const bool calls_on = state->running && state->calls_wanted.load() && state->calls_client() != nullptr;
                if (const char* refusal = dial_refusal(calls_on, state->dial_running)) {
                    report(false, refusal, dial_failure_message(refusal));
                    return;
                }
                state->dial_running = true;
                state->dial_operation_id = operation_id;
                state->dial_cancel = false;
            }

            // Only the lean link mode opens a link for the call. In the full
            // mode hands-free is there or it is not, and the answer is the
            // one it always was.
            const bool on_demand = state->lean.load();
            DialOperation operation(state->clock());
            state->dial_wants_link = true;
            state->nudge();

            // Someone placing a call from this computer is sitting at it:
            // its sound comes here unless the client says otherwise.
            const bool here = dial_audio_here(audio, state->call_audio_here.load(), std::getenv("LANDLINE_DIAL_AUDIO_DEFAULT"));
            std::shared_ptr<TelephonyClient> routed_client;

            bool ok = false;
            std::string reason;
            std::string message;
            while (true) {
                auto client = state->calls_client();
                DialFacts facts;
                facts.calls_enabled = state->running && client != nullptr;
                facts.hands_free_ready = client && client->available();
                {
                    std::lock_guard<std::mutex> lock(state->status_mutex);
                    facts.link_up = state->current_status.is_object() &&
                                    state->current_status.value("classic_connected", false);
                }
                {
                    std::lock_guard<std::mutex> lock(state->dial_mutex);
                    if (state->dial_cancel)
                        operation.cancel();
                }
                const DialOperation::Step step = operation.step(facts, state->clock());
                if (!on_demand && !operation.terminal() && step.stage != DialStage::Dialing) {
                    reason = "no_hands_free";
                    message = dial_failure_message(reason);
                    break;
                }
                if (step.changed && dial_stage_published(step.stage) && progress)
                    progress(to_string(step.stage));
                if (step.stage == DialStage::Dialing) {
                    // Where the sound of this call goes, set before it is
                    // placed: the voice link the iPhone offers with the call
                    // is accepted or refused on that.
                    debug::log(INFO,
                               "bluetooth: dial asked, sound {} (client said: {})",
                               here ? "on this computer" : "on the iPhone",
                               audio.empty() ? "nothing" : audio);
                    std::string route_err;
                    if (!client->call_action({}, here ? "audio_here" : "audio_phone", route_err))
                        debug::log(INFO, "telephony: call audio not routed: {}", route_err);
                    routed_client = client;
                    state->call_command_at = state->clock();
                    std::string err;
                    ok = client->dial(number, err);
                    operation.dialed(ok);
                    if (!ok) {
                        reason = "refused";
                        message = err;
                    }
                    break;
                }
                if (operation.terminal()) {
                    reason = operation.reason();
                    message = dial_failure_message(reason);
                    break;
                }
                std::this_thread::sleep_for(milliseconds(250));
            }

            {
                std::lock_guard<std::mutex> lock(state->dial_mutex);
                state->dial_running = false;
                state->dial_operation_id.clear();
                state->dial_cancel = false;
            }
            debug::log(INFO,
                       "bluetooth: dial {}{}{}",
                       ok ? "accepted by the iPhone" : "not placed",
                       reason.empty() ? "" : ": ",
                       reason);
            report(ok, reason, message);

            // The link stays wanted until the call shows, which then keeps
            // it; a dial that brought no call gives it back.
            bool seen = false;
            if (ok) {
                const auto deadline = steady_clock::now() + seconds(DIAL_CALL_APPEARS_SECONDS);
                while (state->running && !seen && steady_clock::now() < deadline) {
                    {
                        std::lock_guard<std::mutex> lock(state->calls_publish_mutex);
                        seen = state->last_calls.is_array() && !state->last_calls.empty();
                    }
                    if (!seen)
                        std::this_thread::sleep_for(milliseconds(250));
                }
                if (seen) {
                    // One tick with the call seen and the lease still held.
                    std::this_thread::sleep_for(milliseconds(1500));
                } else if (state->running) {
                    // The iPhone took the number and shows no call: said,
                    // instead of the link closing on a silence.
                    debug::log(WARN,
                               "bluetooth: the iPhone accepted the dial and no call has shown in {}s; giving the "
                               "classic link back",
                               DIAL_CALL_APPEARS_SECONDS);
                    if (progress)
                        progress(DIAL_STAGE_NO_CALL);
                }
            }
            // A choice made for a call that never came does not wait for the next.
            if (!seen && routed_client)
                routed_client->forget_call_audio();
            state->dial_wants_link = false;
            state->nudge();
        }).detach();
    }

    bool ConnectionManager::cancel_dial(const std::string& operation_id) {
        std::lock_guard<std::mutex> lock(state_->dial_mutex);
        if (!state_->dial_running)
            return false;
        if (!operation_id.empty() && operation_id != state_->dial_operation_id)
            return false;
        state_->dial_cancel = true;
        return true;
    }

    bool ConnectionManager::call_action(const std::string& path, const std::string& action, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        state_->call_command_at = state_->clock();
        return client->call_action(path, action, err);
    }

    nlohmann::json ConnectionManager::incoming_call() const {
        std::lock_guard<std::mutex> lock(state_->ring_mutex);
        const auto current = state_->ring.current();
        return current ? to_json(*current) : nlohmann::json(nullptr);
    }

    bool ConnectionManager::call_action(const std::string& path,
                                        const std::string& action,
                                        const CallActionOptions& options,
                                        std::string& err,
                                        std::string& via) {
        via.clear();
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        const bool audio_named = options.audio == "here" || options.audio == "phone";
        // Where the sound of that one call goes, set before it is taken.
        const auto route_audio = [&] {
            if (action != "answer" || !audio_named)
                return;
            std::string route_err;
            if (!client->call_action({}, options.audio == "here" ? "audio_here" : "audio_phone", route_err))
                debug::log(INFO, "telephony: call audio not routed: {}", route_err);
        };

        std::optional<IncomingCall> ringing;
        {
            std::lock_guard<std::mutex> lock(state_->ring_mutex);
            ringing = state_->ring.current();
        }
        const bool by_notification = options.uid.has_value() || ringing.has_value();
        if ((action != "answer" && action != "decline") || !by_notification) {
            // No ring is being followed: hands-free, exactly as before.
            via = "hfp";
            route_audio();
            return call_action(path, action == "decline" ? "hangup" : action, err);
        }

        // The call as hands-free shows it, if it does.
        std::string ringing_path;
        for (const auto& call : client->calls()) {
            const std::string state = call.value("state", std::string{});
            const std::string call_path = call.value("path", std::string{});
            if ((state == "incoming" || state == "waiting") && (path.empty() || path == call_path)) {
                ringing_path = call_path;
                break;
            }
        }
        const uint32_t uid = options.uid.value_or(ringing ? ringing->uid : 0);
        // Flags are only known for the notification being followed; another
        // uid is tried and the iPhone decides.
        const bool known = !ringing || ringing->uid != uid;
        const CallActionPlan plan = plan_call_action(action,
                                                     audio_named ? options.audio : std::string{},
                                                     !ringing_path.empty(),
                                                     true,
                                                     known || ringing->can_answer_on_phone,
                                                     known || ringing->can_decline);
        switch (plan.via) {
        case CallRoute::None:
            err = _(plan.error.c_str());
            return false;
        case CallRoute::Ancs: {
            via = "ancs";
            state_->call_command_at = state_->clock();
            const bool sent = perform_notification_action(uid, plan.ancs_action);
            if (!sent) {
                err = _("The iPhone did not take the action.");
                return false;
            }
            if (plan.ancs_action == ancs::ActionId::Negative) {
                std::lock_guard<std::mutex> lock(state_->ring_mutex);
                state_->ring.on_declined(uid);
            }
            debug::log(INFO,
                       "bluetooth: incoming call {} through its notification",
                       plan.ancs_action == ancs::ActionId::Negative ? "declined" : "answered on the iPhone");
            return true;
        }
        case CallRoute::Hfp:
            break;
        }
        via = "hfp";
        route_audio();
        return call_action(ringing_path, plan.hfp_action, err);
    }

    bool ConnectionManager::call_tones(const std::string& tones, std::string& err) {
        auto client = state_->calls_client();
        if (!client) {
            err = _("Call control is off.");
            return false;
        }
        return client->send_tones(tones, err);
    }

    bool ConnectionManager::call_in_hand() const {
        return state_->obex_waits && (state_->calls_busy.load() || state_->dial_wants_link.load());
    }

    bool ConnectionManager::calls_available() const {
        auto client = state_->calls_client();
        return client && client->available();
    }

    nlohmann::json ConnectionManager::calls() const {
        auto client = state_->calls_client();
        if (!client)
            return nlohmann::json::array();

        nlohmann::json calls = client->calls();
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        for (auto& call : calls) {
            const std::string number = call.value("number", "");
            if (number.empty() || !call.value("name", "").empty())
                continue;
            if (std::string name = contact_store().name_for("tel:" + number); !name.empty())
                call["name"] = std::move(name);
        }
        return calls;
    }

    void ConnectionManager::set_device(const std::string& address, bool ancs_enabled) {
        // Restarting is simpler than mutating supervisor state under the worker,
        // and selecting a device is rare. stop() clears the per-device state.
        std::lock_guard<std::mutex> lock(state_->lifecycle);
        stop_locked();
        start_locked(address, ancs_enabled);
    }

    void ConnectionManager::set_ancs_enabled(bool enabled) { state_->ancs_wanted = enabled; }

    nlohmann::json ConnectionManager::status() const {
        // The supervisors' own status objects belong to the supervisor thread;
        // callers get the snapshot it published instead.
        std::lock_guard<std::mutex> lock(state_->status_mutex);
        if (state_->current_status.is_null()) {
            nlohmann::json idle;
            idle["command"] = "bt_connection_changed";
            idle["device_present"] = false;
            idle["device_paired"] = false;
            idle["link_reason"] = _("No paired iPhone selected.");
            idle["profile_reason"] = "";
            idle["link_mode"] = to_string(state_->configured_link_mode());
            put_bond(idle, BondHealth{});
            return idle;
        }
        return state_->current_status;
    }

} // namespace tether::bluetooth
