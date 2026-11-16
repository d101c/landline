#include "tether/bluetooth/advert.hpp"
#include "tether/identity.hpp"
#include "tether/bluetooth/objects.hpp"
#include "tether/log.hpp"

#include <chrono>

namespace tether::bluetooth {

    struct AdvertState {
        GDBusConnection* conn = nullptr;
        std::string adapter_path;
        guint registration_id = 0;
        bool registered_with_bluez = false;
        AdvertInterval interval;
        std::chrono::steady_clock::time_point registered_at{};
    };

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* ADVERT_PATH = "/io/github/d101c/landline/bt_advert";
        constexpr const char* ADVERT_MANAGER = "org.bluez.LEAdvertisingManager1";

        // Private/test identifiers that claim no Apple or hardware-vendor
        // identity, following the convention ancs4linux established. No meaning;
        // they exist because the advert iOS responds to carries them.
        constexpr guint16 INERT_MANUFACTURER_ID = 0xffff;
        constexpr const char* INERT_SERVICE_UUID = "00009999-0000-1000-8000-00805f9b34fb";

        constexpr guint16 ADVERT_TIMEOUT_SECONDS = ANCS_ADVERT_TIMEOUT_SECONDS;

        constexpr const char* ADVERT_XML_HEAD = R"XML(
<node>
  <interface name='org.bluez.LEAdvertisement1'>
    <method name='Release'/>
    <property name='Type' type='s' access='read'/>
    <property name='SolicitUUIDs' type='as' access='read'/>
    <property name='ManufacturerData' type='a{qv}' access='read'/>
    <property name='ServiceData' type='a{sv}' access='read'/>
    <property name='Discoverable' type='b' access='read'/>
    <property name='Timeout' type='q' access='read'/>
    <property name='LocalName' type='s' access='read'/>
)XML";

        // Experimental in BlueZ, which bluetoothd --experimental already is here.
        constexpr const char* ADVERT_XML_INTERVALS = R"XML(    <property name='MinInterval' type='u' access='read'/>
    <property name='MaxInterval' type='u' access='read'/>
)XML";

        constexpr const char* ADVERT_XML_TAIL = R"XML(  </interface>
</node>)XML";

        GVariant* inert_bytes() {
            const guint8 payload[] = {0x00};
            return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload, sizeof(payload), sizeof(guint8));
        }

        void handle_method(GDBusConnection*,
                           const gchar*,
                           const gchar*,
                           const gchar*,
                           const gchar* method,
                           GVariant*,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
            // BlueZ calls Release when it drops the advertisement on its own, which
            // the Timeout above guarantees will happen. The registration is gone by
            // then, so leaving the flag set only makes the next unregister fail.
            if (g_strcmp0(method, "Release") == 0) {
                debug::log(INFO, "bluetooth: ANCS advertisement released by BlueZ");
                if (auto* state = static_cast<AdvertState*>(user_data))
                    state->registered_with_bluez = false;
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }

        GVariant* handle_get_property(GDBusConnection*,
                                      const gchar*,
                                      const gchar*,
                                      const gchar*,
                                      const gchar* name,
                                      GError**,
                                      gpointer user_data) {
            const std::string prop = name ? name : "";
            const auto* state = static_cast<const AdvertState*>(user_data);

            // Only declared, and so only ever asked for, when an interval is set.
            if (prop == "MinInterval" && state)
                return g_variant_new_uint32(state->interval.min_ms);

            if (prop == "MaxInterval" && state)
                return g_variant_new_uint32(state->interval.max_ms);

            if (prop == "Type")
                return g_variant_new_string("peripheral");

            if (prop == "SolicitUUIDs") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
                g_variant_builder_add(&builder, "s", UUID_ANCS);
                return g_variant_builder_end(&builder);
            }

            if (prop == "ManufacturerData") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("a{qv}"));
                g_variant_builder_add(&builder, "{qv}", INERT_MANUFACTURER_ID, inert_bytes());
                return g_variant_builder_end(&builder);
            }

            if (prop == "ServiceData") {
                GVariantBuilder builder;
                g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&builder, "{sv}", INERT_SERVICE_UUID, inert_bytes());
                return g_variant_builder_end(&builder);
            }

            if (prop == "Discoverable")
                return g_variant_new_boolean(TRUE);

            if (prop == "Timeout")
                return g_variant_new_uint16(ADVERT_TIMEOUT_SECONDS);

            if (prop == "LocalName")
                return g_variant_new_string(identity::DISPLAY_NAME);

            return nullptr;
        }

        const GDBusInterfaceVTable ADVERT_VTABLE = {handle_method, handle_get_property, nullptr, {nullptr}};

    } // namespace

    std::string advert_introspection_xml(const AdvertInterval& interval) {
        std::string xml = ADVERT_XML_HEAD;
        if (interval.set())
            xml += ADVERT_XML_INTERVALS;
        xml += ADVERT_XML_TAIL;
        return xml;
    }

    AncsAdvertisement::AncsAdvertisement(GDBusConnection* connection, std::string adapter_path, AdvertInterval interval)
        : state_(std::make_unique<AdvertState>()) {
        state_->conn = connection;
        state_->adapter_path = std::move(adapter_path);
        state_->interval = interval.set() ? interval : AdvertInterval{};
    }

    AdvertInterval AncsAdvertisement::interval() const { return state_->interval; }

    int64_t AncsAdvertisement::age_seconds() const {
        if (!state_->registered_with_bluez)
            return 0;
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                 state_->registered_at)
            .count();
    }

    AncsAdvertisement::~AncsAdvertisement() {
        unregister_with_bluez();
        unexport_object();
    }

    bool AncsAdvertisement::active() const { return state_->registered_with_bluez; }

    bool AncsAdvertisement::export_object() {
        if (state_->registration_id != 0)
            return true;

        GError* error = nullptr;
        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(advert_introspection_xml(state_->interval).c_str(), &error);
        if (!info) {
            debug::log(ERR, "bluetooth: bad advertisement introspection: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        state_->registration_id = g_dbus_connection_register_object(
            state_->conn, ADVERT_PATH, info->interfaces[0], &ADVERT_VTABLE, state_.get(), nullptr, &error);
        g_dbus_node_info_unref(info);

        if (state_->registration_id == 0) {
            debug::log(ERR, "bluetooth: cannot export advertisement: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }
        return true;
    }

    bool AncsAdvertisement::register_with_bluez(std::string* failure) {
        if (state_->registered_with_bluez)
            return true;
        if (state_->registration_id == 0)
            return false;

        GError* error = nullptr;
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                      BLUEZ_NAME,
                                                      state_->adapter_path.c_str(),
                                                      ADVERT_MANAGER,
                                                      "RegisterAdvertisement",
                                                      g_variant_new("(oa{sv})", ADVERT_PATH, &options),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      10000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            const std::string message = error && error->message ? error->message : "unknown error";
            const bool already = message.find("AlreadyExists") != std::string::npos;
            const bool timed_out = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
            g_clear_error(&error);

            if (already) {
                state_->registered_with_bluez = true;
                state_->registered_at = std::chrono::steady_clock::now();
                debug::log(INFO, "bluetooth: ANCS solicitation was already registered; adopted it");
                return true;
            }

            debug::log(ERR, "bluetooth: RegisterAdvertisement failed: {}", message);
            if (failure)
                *failure = message;

            // A call that timed out has an unknown outcome.
            if (timed_out) {
                state_->registered_with_bluez = true;
                unregister_with_bluez();
            }
            return false;
        }
        g_variant_unref(reply);

        state_->registered_with_bluez = true;
        state_->registered_at = std::chrono::steady_clock::now();
        if (state_->interval.set())
            debug::log(INFO,
                       "bluetooth: soliciting ANCS for {}s, advertising every {}-{} ms",
                       ADVERT_TIMEOUT_SECONDS,
                       state_->interval.min_ms,
                       state_->interval.max_ms);
        else
            debug::log(INFO, "bluetooth: soliciting ANCS for {}s", ADVERT_TIMEOUT_SECONDS);
        return true;
    }

    void AncsAdvertisement::unregister_with_bluez() {
        if (state_->registered_with_bluez) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(state_->conn,
                                                          BLUEZ_NAME,
                                                          state_->adapter_path.c_str(),
                                                          ADVERT_MANAGER,
                                                          "UnregisterAdvertisement",
                                                          g_variant_new("(o)", ADVERT_PATH),
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (reply)
                g_variant_unref(reply);
            g_clear_error(&error);
            state_->registered_with_bluez = false;
        }
    }

    void AncsAdvertisement::forget_registration() { state_->registered_with_bluez = false; }

    void AncsAdvertisement::unexport_object() {
        if (state_->registration_id != 0) {
            g_dbus_connection_unregister_object(state_->conn, state_->registration_id);
            state_->registration_id = 0;
        }
    }

} // namespace tether::bluetooth
