#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/ancs/liveness.hpp"
#include "tether/bluetooth/ancs/sequencer.hpp"
#include "tether/bluetooth/ancs/subscription_guard.hpp"
#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <gio/gio.h>
#include <mutex>
#include <optional>

namespace tether::bluetooth::ancs {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_CHARACTERISTIC = "org.bluez.GattCharacteristic1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";
        constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        constexpr int CALL_TIMEOUT_MS = 10000;
        constexpr int STOP_NOTIFY_TIMEOUT_MS = 2000;

        // When this process last sent a StopNotify, on the clock below; -1
        // for never. Shared by every client the process makes: turning
        // notifications off and on again builds a new one, and the write the
        // old one started is still BlueZ's to finish.
        std::atomic<int64_t> g_last_stop_at{-1};

        int64_t guard_clock() {
            using namespace std::chrono;
            return duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
        }

    } // namespace

    struct AncsClientState {
        BluezMonitor* monitor = nullptr;
        AncsClient::NotificationFn on_notification;
        AncsClient::WithdrawFn on_withdraw;
        AncsClient::StatusFn on_status;
        // Called on the GLib thread when the phone has sent something.
        std::function<void()> on_activity;

        std::string device_path;
        // The last non-empty device path, so an LE teardown can be told apart
        // from actually being pointed at a different phone.
        std::string last_known_device;
        std::string notification_source_path;
        std::string control_point_path;
        std::string data_source_path;

        bool subscribed = false;

        std::atomic<bool> ready{false};
        bool content_enabled = false;
        std::string reason = "Waiting for the iPhone's notification service.";

        int64_t next_discover = 0;
        // When the session began to be rebuilt; -1 means "at the next tick".
        int64_t settle_since = -1;
        bool fast_settle = true;
        int64_t next_subscribe = 0;
        int64_t next_probe = 0;
        int64_t next_verify = 0;
        int64_t next_resubscribe = 0;
        // What the last subscription attempt was told, so the same answer is
        // written once and not every second.
        std::string last_subscribe_note;

        // Whether a session that worked still does (liveness.hpp).
        AncsLiveness liveness;

        // Guards everything that crosses a thread boundary: the two GATT buffers
        // filled on the GLib thread, the characteristic paths that thread compares
        // against, and actions queued by whichever thread is serving a command.
        std::mutex inbox;
        std::deque<std::vector<uint8_t>> pending_source;
        std::deque<std::vector<uint8_t>> pending_data;
        std::deque<Request> pending_actions;

        // The registry is written by tick() and read by whichever thread answers
        // a "list notifications" command.
        mutable std::mutex registry_mutex;

        guint source_signal = 0;
        guint data_signal = 0;

        NotificationRegistry registry;
        std::unique_ptr<ControlPointSequencer> sequencer;

        // UIDs whose attributes are being fetched, so a notification can be
        // rebuilt when its response lands.
        std::map<uint32_t, SourceEvent> in_progress;

        // Bundle id -> display name, as answered by GetAppAttributes. Only
        // touched from tick()'s thread. An entry with an empty value marks a
        // request already in flight, so an app is asked about once per session.
        std::map<std::string, std::string> app_names;

        bool write_control_point(const std::vector<uint8_t>& payload);
        ProbeAnswer probe_liveness();
        void step_liveness(int64_t now);
        void set_status(bool now_ready, const std::string& text);
        bool discover();
        bool subscribe();
        std::optional<bool> read_notifying(const std::string& path) const;
        StartOutcome start_notify(const std::string& path, std::string& detail);
        void attach_signals();
        void detach_signals();
        void release(Release why);
        void note_subscribe(const std::string& note);
        void drop_gatt_paths();
        void verify_subscription();
        void rebuild(const std::string& reason);
        bool link_alive() const;
        void handle_source_event(const SourceEvent& event, int64_t now);
        void handle_response(const Request& request, const Response& response, int64_t now);
        std::string app_display_name(const std::string& app_id);
    };

    namespace {

        void on_properties_changed(GDBusConnection*,
                                   const gchar*,
                                   const gchar* object_path,
                                   const gchar*,
                                   const gchar*,
                                   GVariant* parameters,
                                   gpointer user_data) {
            auto* state = static_cast<AncsClientState*>(user_data);
            if (!state || !object_path)
                return;

            const gchar* iface = nullptr;
            GVariant* changed = nullptr;
            GVariantIter* invalidated = nullptr;
            g_variant_get(parameters, "(&s@a{sv}as)", &iface, &changed, &invalidated);
            if (invalidated)
                g_variant_iter_free(invalidated);
            if (!changed)
                return;

            GVariant* value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
            g_variant_unref(changed);
            if (!value)
                return;

            gsize length = 0;
            const guchar* bytes = static_cast<const guchar*>(g_variant_get_fixed_array(value, &length, 1));
            bool buffered = false;
            if (bytes && length) {
                std::vector<uint8_t> copy(bytes, bytes + length);
                std::lock_guard<std::mutex> lock(state->inbox);
                if (state->notification_source_path == object_path) {
                    state->pending_source.push_back(std::move(copy));
                    buffered = true;
                } else if (state->data_source_path == object_path) {
                    state->pending_data.push_back(std::move(copy));
                    buffered = true;
                }
            }
            g_variant_unref(value);
            // The thread that drains the buffers is asked to do so now: an
            // answer used to wait up to a second for its next pass, and the
            // next question with it.
            if (buffered && state->on_activity)
                state->on_activity();
        }

    } // namespace

    void AncsClientState::set_status(bool now_ready, const std::string& text) {
        if (ready.load() == now_ready && reason == text)
            return;
        ready = now_ready;
        reason = text;
        if (on_status)
            on_status(ready.load(), reason);
    }

    bool AncsClientState::write_control_point(const std::vector<uint8_t>& payload) {
        if (control_point_path.empty() || !monitor || !monitor->connection())
            return false;

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));

        GVariant* value =
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload.data(), payload.size(), sizeof(uint8_t));

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      control_point_path.c_str(),
                                                      IFACE_CHARACTERISTIC,
                                                      "WriteValue",
                                                      g_variant_new("(@aya{sv})", value, &options),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            const std::string message = error ? error->message : "unknown error";
            g_clear_error(&error);
            // Until the user approves the phone's prompt, the control point
            // refuses every write. That is a pending permission, not a fault.
            debug::log(WARN, "ancs: control point write failed: {}", message);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    // The probe of liveness.hpp: GetNotificationAttributes for a UID no
    // notification has, outside the sequencer, whose answer is an error and
    // never a packet on the Data Source.
    ProbeAnswer AncsClientState::probe_liveness() {
        if (control_point_path.empty() || !monitor || !monitor->connection())
            return ProbeAnswer::Inconclusive;
        const Request request = build_notification_request(ANCS_PROBE_UID, {NotificationAttributeId::AppIdentifier});
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        GVariant* value = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, request.payload.data(), request.payload.size(), sizeof(uint8_t));
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      control_point_path.c_str(),
                                                      IFACE_CHARACTERISTIC,
                                                      "WriteValue",
                                                      g_variant_new("(@aya{sv})", value, &options),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      ANCS_PROBE_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (reply) {
            g_variant_unref(reply);
            return classify_probe(true, false, {});
        }
        const bool timed_out = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        const std::string message = error ? error->message : "unknown error";
        g_clear_error(&error);
        const ProbeAnswer answer = classify_probe(false, timed_out, message);
        if (answer == ProbeAnswer::Inconclusive)
            debug::log(INFO, "ancs: the liveness probe was neither answered nor timed out ({}); tried again later", message);
        return answer;
    }

    void AncsClientState::step_liveness(int64_t now) {
        if (ready)
            liveness.ready(now);
        else
            liveness.lost();
        const bool busy = sequencer->in_flight() || sequencer->queued() > 0;
        AncsLiveness::Action action = liveness.due(now, busy);
        if (action == AncsLiveness::Action::Probe) {
            const auto started = std::chrono::steady_clock::now();
            const ProbeAnswer answer = probe_liveness();
            const auto took =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (answer == ProbeAnswer::Alive)
                debug::log(DEBUG, "ancs: the iPhone answered the liveness probe in {}ms", took);
            action = liveness.probe_answer(now, answer);
        }
        switch (action) {
        case AncsLiveness::Action::ResubscribeSilent:
            debug::log(WARN,
                       "ancs: the iPhone did not answer the liveness probe within {}ms; the notification session is "
                       "subscribed again",
                       ANCS_PROBE_TIMEOUT_MS);
            rebuild("the liveness probe went unanswered");
            break;
        case AncsLiveness::Action::ResubscribeQuiet:
            debug::log(INFO,
                       "ancs: no notification from the iPhone for {}h; the notification session is subscribed again in "
                       "case the iPhone's side of it is stuck",
                       liveness.config().quiet_seconds / 3600);
            rebuild("no notification for a long time");
            break;
        default:
            break;
        }
    }

    bool AncsClientState::discover() {
        if (!monitor || !monitor->connection() || device_path.empty())
            return false;

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            g_clear_error(&error);
            return false;
        }

        // Collected locally, then published under `inbox`: the GLib thread
        // compares incoming values against these paths.
        std::string source_path, control_path, data_path;

        GVariant* objects = g_variant_get_child_value(reply, 0);
        GVariantIter iter;
        const gchar* path = nullptr;
        GVariant* interfaces = nullptr;
        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            const std::string object_path = path ? path : "";
            // Characteristics live under the device that owns them.
            if (object_path.rfind(device_path + "/", 0) != 0)
                continue;

            GVariant* props = g_variant_lookup_value(interfaces, IFACE_CHARACTERISTIC, G_VARIANT_TYPE("a{sv}"));
            if (!props)
                continue;

            GVariant* uuid_value = g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
            if (uuid_value) {
                const std::string uuid = g_variant_get_string(uuid_value, nullptr);
                if (uuid == UUID_NOTIFICATION_SOURCE)
                    source_path = object_path;
                else if (uuid == UUID_CONTROL_POINT)
                    control_path = object_path;
                else if (uuid == UUID_DATA_SOURCE)
                    data_path = object_path;
                g_variant_unref(uuid_value);
            }
            g_variant_unref(props);
        }
        g_variant_unref(objects);
        g_variant_unref(reply);

        {
            std::lock_guard<std::mutex> lock(inbox);
            notification_source_path = source_path;
            control_point_path = control_path;
            data_source_path = data_path;
        }

        // All three are needed. A partial tree means BlueZ is still enumerating.
        return !source_path.empty() && !control_path.empty() && !data_path.empty();
    }

    // The characteristic's Notifying property, or nothing when the object is gone.
    std::optional<bool> AncsClientState::read_notifying(const std::string& path) const {
        if (path.empty() || !monitor || !monitor->connection())
            return std::nullopt;
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      path.c_str(),
                                                      IFACE_PROPS,
                                                      "Get",
                                                      g_variant_new("(ss)", IFACE_CHARACTERISTIC, "Notifying"),
                                                      G_VARIANT_TYPE("(v)"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            g_clear_error(&error);
            return std::nullopt;
        }
        GVariant* boxed = nullptr;
        g_variant_get(reply, "(v)", &boxed);
        const bool notifying =
            boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(boxed);
        if (boxed)
            g_variant_unref(boxed);
        g_variant_unref(reply);
        return notifying;
    }

    // One StartNotify, and what the characteristic reads right after it. The
    // call is short on purpose: BlueZ answers at once for a subscriber it
    // already holds, and one it queues behind another write is not worth
    // holding this thread for. Whatever the answer, nothing here ever
    // follows it with a StopNotify (see subscription_guard.hpp).
    StartOutcome AncsClientState::start_notify(const std::string& path, std::string& detail) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                      BLUEZ_NAME,
                                                      path.c_str(),
                                                      IFACE_CHARACTERISTIC,
                                                      "StartNotify",
                                                      nullptr,
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      START_NOTIFY_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        const bool ok = reply != nullptr;
        const bool timed_out = !ok && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        const std::string message = ok ? std::string() : error ? error->message : "unknown";
        if (reply)
            g_variant_unref(reply);
        g_clear_error(&error);

        const bool notifying = read_notifying(path).value_or(false);
        const StartOutcome outcome = classify_start(ok, timed_out, message, notifying);
        if (outcome == StartOutcome::Pending)
            detail = ok ? "accepted, not notifying yet" : timed_out ? "no answer yet" : message;
        else if (outcome != StartOutcome::Live)
            detail = message;
        return outcome;
    }

    void AncsClientState::note_subscribe(const std::string& note) {
        if (note == last_subscribe_note)
            return;
        last_subscribe_note = note;
        if (!note.empty())
            debug::log(INFO, "ancs: {}", note);
    }

    void AncsClientState::attach_signals() {
        monitor->invoke_sync([this] {
            GDBusConnection* bus = monitor->connection();
            if (!source_signal) {
                source_signal = g_dbus_connection_signal_subscribe(bus,
                                                                   BLUEZ_NAME,
                                                                   IFACE_PROPS,
                                                                   "PropertiesChanged",
                                                                   notification_source_path.c_str(),
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_properties_changed,
                                                                   this,
                                                                   nullptr);
            }
            if (!data_signal) {
                data_signal = g_dbus_connection_signal_subscribe(bus,
                                                                 BLUEZ_NAME,
                                                                 IFACE_PROPS,
                                                                 "PropertiesChanged",
                                                                 data_source_path.c_str(),
                                                                 nullptr,
                                                                 G_DBUS_SIGNAL_FLAGS_NONE,
                                                                 on_properties_changed,
                                                                 this,
                                                                 nullptr);
            }
        });
    }

    void AncsClientState::detach_signals() {
        if (!monitor || !monitor->connection())
            return;
        GDBusConnection* conn = monitor->connection();
        if (source_signal) {
            g_dbus_connection_signal_unsubscribe(conn, source_signal);
            source_signal = 0;
        }
        if (data_signal) {
            g_dbus_connection_signal_unsubscribe(conn, data_signal);
            data_signal = 0;
        }
    }

    bool AncsClientState::subscribe() {
        if (!monitor || !monitor->connection())
            return false;

        // cached tree can accept StartNotify with no link
        if (!link_alive())
            return false;

        // A StopNotify of this process may still have its write on the link:
        // a subscriber queued behind it is what bluetoothd asserts on.
        if (start_notify_wait(guard_clock(), g_last_stop_at.load()) > 0) {
            note_subscribe("waiting for BlueZ to finish turning notifications off before asking for them again");
            return false;
        }

        // Before the calls, so that what the phone sends the moment the
        // subscription takes is buffered rather than lost.
        attach_signals();

        std::string data_detail, source_detail;
        const StartOutcome data = start_notify(data_source_path, data_detail);
        const StartOutcome source = start_notify(notification_source_path, source_detail);

        if (data == StartOutcome::Gone || source == StartOutcome::Gone) {
            note_subscribe("the characteristics moved, rediscovering the notification service");
            drop_gatt_paths();
            return false;
        }
        if (data == StartOutcome::Live && source == StartOutcome::Live) {
            last_subscribe_note.clear();
            return true;
        }

        const std::string& detail = data != StartOutcome::Live ? data_detail : source_detail;
        const bool pending = data != StartOutcome::Failed && source != StartOutcome::Failed;
        note_subscribe(pending ? "BlueZ is still setting the subscription up (" + detail + ")"
                               : "StartNotify not ready yet (" + detail + ")");
        return false;
    }

    // Lets go of the subscription. A rebuild only forgets it on this side:
    // BlueZ keeps the subscriber and writes the phone's CCC again by itself
    // when the link returns (btd_gatt_client_connected), and a StartNotify
    // sent again is answered at once for a subscriber it holds. StopNotify is
    // for notifications being turned off, and only when stop_notify_verdict()
    // says its write cannot meet another one.
    void AncsClientState::release(Release why) {
        static const bool stop_enabled = stop_notify_enabled(std::getenv("LANDLINE_ANCS_STOP_NOTIFY"));
        if (why == Release::TurnedOff && subscribed && monitor && monitor->connection()) {
            const bool alive = link_alive();
            const int64_t last_stop = g_last_stop_at.load();
            bool sent = false;
            for (const std::string& path : {data_source_path, notification_source_path}) {
                if (path.empty())
                    continue;
                StopFacts facts;
                facts.why = why;
                facts.link_alive = alive;
                facts.notifying = alive && read_notifying(path).value_or(false);
                facts.now = guard_clock();
                facts.last_stop_at = last_stop;
                facts.enabled = stop_enabled;
                if (stop_notify_verdict(facts) != StopVerdict::Send)
                    continue;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(monitor->connection(),
                                                              BLUEZ_NAME,
                                                              path.c_str(),
                                                              IFACE_CHARACTERISTIC,
                                                              "StopNotify",
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              STOP_NOTIFY_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
                sent = true;
            }
            if (sent)
                g_last_stop_at = guard_clock();
        }
        detach_signals();
    }

    // The cached characteristic paths belong to one GATT session. Cycling
    // Bluetooth on the phone rebuilds the tree under fresh paths, and BlueZ then
    // answers every call on the old ones with UnknownObject forever. Only
    // rediscovery recovers, so drop what is cached and let the next tick run it.
    void AncsClientState::drop_gatt_paths() {
        release(Release::Rebuild);
        subscribed = false;
        next_subscribe = 0;
        next_discover = 0;
        std::lock_guard<std::mutex> lock(inbox);
        notification_source_path.clear();
        control_point_path.clear();
        data_source_path.clear();
    }

    // `subscribed` records that StartNotify once succeeded, and `ready` that iOS once answered on the Data Source.
    void AncsClientState::verify_subscription() {
        if (!monitor || !monitor->connection())
            return;

        bool object_gone = false;
        bool notifying = true;
        for (const std::string& path : {notification_source_path, data_source_path}) {
            if (path.empty())
                continue;
            const std::optional<bool> read = read_notifying(path);
            if (!read)
                object_gone = true;
            else if (!*read)
                notifying = false;
        }

        // Notifying stays set on a cached tree after the LE link drops.
        const bool link_gone = !object_gone && notifying && !link_alive();
        if (!object_gone && notifying && !link_gone)
            return;

        if (object_gone) {
            debug::log(INFO,
                       "ancs: the notification subscription is no longer live (characteristics gone), rebuilding it");
            drop_gatt_paths();
            set_status(false, "Waiting for the iPhone's notification service.");
            sequencer->reset();
            in_progress.clear();
        } else {
            rebuild(link_gone ? "LE link gone" : "BlueZ cleared Notifying");
        }
    }

    void AncsClientState::rebuild(const std::string& reason) {
        debug::log(INFO, "ancs: the notification subscription is no longer live ({}), rebuilding it", reason);
        release(Release::Rebuild);
        subscribed = false;
        next_subscribe = 0;
        // The session that comes next is asked its first question as soon
        // as it exists, not when the one before would have been asked again.
        next_probe = 0;
        settle_since = -1;
        set_status(false, "Subscribing to the iPhone's notifications...");
        sequencer->reset();
        in_progress.clear();
        // Buffered packets carry the old session's UIDs.
        std::lock_guard<std::mutex> lock(inbox);
        pending_source.clear();
        pending_data.clear();
        pending_actions.clear();
    }

    bool AncsClientState::link_alive() const {
        for (const auto& device : monitor->snapshot().devices) {
            if (device.path == device_path)
                return gatt_link_alive(monitor->connection(), device.gap_name_path);
        }
        return true;
    }

    void AncsClientState::handle_source_event(const SourceEvent& event, int64_t now) {
        (void)now;
        Decision decision;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            decision = registry.classify(event);
            if (decision == Decision::Ignore)
                registry.remember(event);
            else if (decision == Decision::Withdraw)
                registry.forget(event.uid);
        }
        switch (decision) {
        case Decision::Ignore:
            return;
        case Decision::Withdraw:
            // A fetch still queued or in flight must not bring it back.
            in_progress.erase(event.uid);
            if (on_withdraw)
                on_withdraw(event.uid);
            return;
        case Decision::Fetch:
            break;
        }

        if (event.pre_existing() && sequencer->queued() >= MAX_QUEUED_REQUESTS / 2)
            return;

        std::vector<NotificationAttributeId> attributes{NotificationAttributeId::AppIdentifier,
                                                        NotificationAttributeId::Date};
        if (content_enabled) {
            attributes.push_back(NotificationAttributeId::Title);
            attributes.push_back(NotificationAttributeId::Subtitle);
            attributes.push_back(NotificationAttributeId::Message);
        }

        in_progress[event.uid] = event;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            registry.remember(event);
        }
        if (!sequencer->submit(build_notification_request(event.uid, attributes)))
            in_progress.erase(event.uid);
    }

    // Never blocks on the phone: the derived name is used now and the real one
    // backfills the registry when GetAppAttributes answers.
    std::string AncsClientState::app_display_name(const std::string& app_id) {
        if (app_id.empty())
            return {};
        auto [entry, inserted] = app_names.try_emplace(app_id);
        if (inserted)
            sequencer->submit(build_app_request(app_id));
        return entry->second.empty() ? derive_app_name(app_id) : entry->second;
    }

    void AncsClientState::handle_response(const Request& request, const Response& response, int64_t now) {
        // Any answer on the Data Source proves the phone has authorized
        // notification access, which StartNotify alone does not: the
        // readiness probe, or the attributes of a notification the phone
        // listed first. An action is acknowledged here without one.
        if (!ready &&
            (response.command == CommandId::GetAppAttributes ||
             (response.command == CommandId::GetNotificationAttributes && response.uid == request.uid &&
              !request.attribute_ids.empty())))
            set_status(true, "Notification mirroring is active.");

        if (response.command == CommandId::GetAppAttributes) {
            const std::string name = response.attribute(static_cast<uint8_t>(AppAttributeId::DisplayName));
            if (!response.app_id.empty() && !name.empty()) {
                app_names[response.app_id] = name;
                // The name arrives after the notifications that triggered the
                // lookup were already stored and broadcast.
                std::lock_guard<std::mutex> lock(registry_mutex);
                registry.rename_app(response.app_id, name);
            }
            return;
        }
        if (response.command != CommandId::GetNotificationAttributes)
            return;

        if (response.uid != request.uid) {
            debug::log(
                WARN, "ancs: discarding a response for uid {} while {} was in flight", response.uid, request.uid);
            return;
        }

        auto found = in_progress.find(request.uid);
        if (found == in_progress.end())
            return;
        const SourceEvent event = found->second;
        in_progress.erase(found);

        Notification notification;
        notification.uid = request.uid;
        notification.app_id = response.attribute(static_cast<uint8_t>(NotificationAttributeId::AppIdentifier));
        notification.app_name = app_display_name(notification.app_id);
        notification.title = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Title));
        notification.subtitle = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Subtitle));
        notification.body = response.attribute(static_cast<uint8_t>(NotificationAttributeId::Message));
        notification.category = event.category;
        notification.silent = event.silent();
        notification.has_positive_action = event.has_positive_action();
        notification.has_negative_action = event.has_negative_action();
        notification.pre_existing = event.pre_existing();
        // ANCS dates are the phone's wall clock with no zone, some apps send none at all.
        const int64_t delivered =
            parse_map_timestamp(response.attribute(static_cast<uint8_t>(NotificationAttributeId::Date)));
        notification.received = delivered ? delivered : now;

        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            registry.store(notification);
        }
        if (on_notification)
            on_notification(notification);
    }

    AncsClient::AncsClient(BluezMonitor& monitor,
                           NotificationFn on_notification,
                           WithdrawFn on_withdraw,
                           StatusFn on_status)
        : state_(std::make_unique<AncsClientState>()) {
        state_->monitor = &monitor;
        state_->on_notification = std::move(on_notification);
        state_->on_withdraw = std::move(on_withdraw);
        state_->on_status = std::move(on_status);
        state_->liveness.configure(liveness_config_from(std::getenv("LANDLINE_ANCS_LIVENESS"),
                                                        std::getenv("LANDLINE_ANCS_PROBE_SECONDS"),
                                                        std::getenv("LANDLINE_ANCS_QUIET_SECONDS")));

        AncsClientState* raw = state_.get();
        state_->sequencer = std::make_unique<ControlPointSequencer>(
            [raw](const std::vector<uint8_t>& payload) { return raw->write_control_point(payload); },
            [raw](const Request& request, const Response& response) {
                raw->handle_response(request, response, static_cast<int64_t>(std::time(nullptr)));
            },
            [raw](const Request& request, const std::string& reason) {
                raw->in_progress.erase(request.uid);
                debug::log(WARN, "ancs: request failed: {}", reason);
            });
    }

    AncsClient::~AncsClient() {
        if (state_)
            state_->release(Release::TurnedOff);
    }

    void AncsClient::set_device(const std::string& device_path) {
        if (state_->device_path == device_path)
            return;

        // The LE bearer drops and returns constantly, which shows up here as the
        // path alternating with "". That is one session ending, not a different
        // phone: wiping the registry for it would empty the notification list on
        // every blip, and iOS does not always replay its backlog to refill it.
        const bool same_device =
            device_path.empty() || state_->last_known_device.empty() || device_path == state_->last_known_device;
        if (!device_path.empty())
            state_->last_known_device = device_path;

        // The path going empty is the link being lost, and another path is
        // another GATT tree: neither is a reason to write to the phone.
        state_->release(Release::Rebuild);
        state_->device_path = device_path;
        state_->subscribed = false;
        state_->next_discover = 0;
        state_->next_subscribe = 0;
        state_->settle_since = -1;
        state_->next_probe = 0;
        state_->next_verify = 0;
        state_->next_resubscribe = 0;
        state_->last_subscribe_note.clear();
        state_->sequencer->reset();
        state_->in_progress.clear();
        {
            std::lock_guard<std::mutex> lock(state_->registry_mutex);
            if (same_device) {
                state_->registry.begin_session();
            } else {
                state_->app_names.clear();
                state_->registry.clear();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state_->inbox);
            state_->notification_source_path.clear();
            state_->control_point_path.clear();
            state_->data_source_path.clear();
            state_->pending_source.clear();
            state_->pending_data.clear();
            state_->pending_actions.clear();
        }

        if (device_path.empty())
            state_->set_status(false, "The iPhone is not connected over LE.");
        else
            state_->set_status(false, "Waiting for the iPhone's notification service.");
    }

    void AncsClient::set_content_enabled(bool enabled) { state_->content_enabled = enabled; }

    void AncsClient::set_fast_settle(bool enabled) { state_->fast_settle = enabled; }

    void AncsClient::set_activity_listener(std::function<void()> listener) {
        state_->on_activity = std::move(listener);
    }

    bool AncsClient::subscribed() const { return state_->subscribed; }

    void AncsClient::retry_now() {
        state_->next_discover = 0;
        state_->next_subscribe = 0;
        state_->next_probe = 0;
        state_->settle_since = -1;
    }

    void AncsClient::drop_session(const std::string& reason) {
        if (state_->subscribed)
            state_->rebuild(reason);
    }

    bool AncsClient::ready() const { return state_->ready; }

    const std::string& AncsClient::status_reason() const { return state_->reason; }

    std::vector<Notification> AncsClient::recent_notifications(size_t limit) const {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        return state_->registry.recent(limit);
    }

    bool AncsClient::perform_action(uint32_t uid, ActionId action) {
        {
            std::lock_guard<std::mutex> lock(state_->registry_mutex);
            const Notification* stored = state_->registry.find(uid);
            if (stored && stored->session != state_->registry.session()) {
                state_->registry.forget(uid);
                if (state_->on_withdraw)
                    state_->on_withdraw(uid);
                return true;
            }
        }
        if (!state_->ready)
            return false;
        // The sequencer belongs to tick()'s thread; handing it a request from a
        // command thread would race the queue it pops from. Queue it instead.
        std::lock_guard<std::mutex> lock(state_->inbox);
        if (state_->pending_actions.size() >= MAX_QUEUED_REQUESTS)
            return false;
        state_->pending_actions.push_back(build_action_request(uid, action));
        return true;
    }

    void AncsClient::tick(int64_t now) {
        auto* s = state_.get();
        if (s->device_path.empty())
            return;

        if (s->settle_since < 0)
            s->settle_since = now;
        const int retry = ancs_retry_seconds(now, s->settle_since, s->fast_settle);

        if (s->subscribed && now >= s->next_verify) {
            s->next_verify =
                now + (s->ready ? ANCS_VERIFY_SECONDS
                                : unanswered_seconds(now, s->settle_since, ANCS_RETRY_SECONDS, ANCS_WATCH_SECONDS));
            s->verify_subscription();
        }

        if (s->notification_source_path.empty()) {
            if (now < s->next_discover)
                return;
            s->next_discover = now + retry;
            if (!s->discover()) {
                s->set_status(false, "Waiting for the iPhone's notification service.");
                return;
            }
        }

        if (!s->subscribed) {
            if (now < s->next_subscribe)
                return;
            s->next_subscribe = now + retry;
            if (!s->subscribe()) {
                s->set_status(false, "Subscribing to the iPhone's notifications...");
                return;
            }
            s->subscribed = true;
            s->next_resubscribe = now + RESUBSCRIBE_SECONDS;
            uint64_t session = 0;
            {
                std::lock_guard<std::mutex> lock(s->registry_mutex);
                s->registry.begin_session();
                session = s->registry.session();
            }
            debug::log(INFO, "ancs: notification session {} started", session);
        }

        // Subscribed as far as BlueZ says, and the phone has not answered:
        // the subscriber BlueZ registered again when the link came back may
        // have been refused and dropped, which leaves Notifying reading true.
        // StartNotify is answered at once when the subscriber is still there
        // and makes a new one when it is not.
        if (!s->ready && now >= s->next_resubscribe) {
            s->next_resubscribe =
                now + unanswered_seconds(now, s->settle_since, RESUBSCRIBE_SECONDS, ANCS_WATCH_SECONDS);
            s->subscribe();
            if (s->notification_source_path.empty()) {
                s->set_status(false, "Waiting for the iPhone's notification service.");
                return;
            }
        }

        // Drain what the other threads buffered.
        std::deque<std::vector<uint8_t>> source, data;
        std::deque<Request> actions;
        {
            std::lock_guard<std::mutex> lock(s->inbox);
            source.swap(s->pending_source);
            data.swap(s->pending_data);
            actions.swap(s->pending_actions);
        }

        for (auto& action : actions)
            s->sequencer->submit(std::move(action));

        for (const auto& packet : data)
            s->sequencer->on_data_source(packet.data(), packet.size(), now);

        if (!source.empty())
            s->liveness.packet(now);
        for (const auto& packet : source) {
            SourceEvent event;
            if (!parse_source_event(packet.data(), packet.size(), event)) {
                debug::log(WARN, "ancs: discarding a malformed notification packet");
                continue;
            }
            s->handle_source_event(event, now);
        }

        s->sequencer->tick(now);

        // A successful StartNotify proves only that the subscription exists, not
        // that iOS authorized notification access.
        if (!s->ready && now >= s->next_probe) {
            s->next_probe = now + unanswered_seconds(now, s->settle_since, ANCS_RETRY_SECONDS, ANCS_WATCH_SECONDS);
            s->set_status(false,
                          "Waiting for the iPhone to allow notification access. Settings > Bluetooth > (i) > "
                          "Share System Notifications.");
            s->sequencer->submit(build_app_request(APP_ID_MESSAGES));
            // Written now when nothing is in flight: it used to wait for the
            // next pass, a second during which the link was of no use yet.
            s->sequencer->tick(now);
        }

        s->step_liveness(now);
    }

} // namespace tether::bluetooth::ancs
