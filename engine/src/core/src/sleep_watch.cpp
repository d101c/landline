#include "tether/sleep_watch.hpp"

#include <tether/log.hpp>

#include <unistd.h>

#if __has_include(<gio/gunixfdlist.h>)
#include <gio/gunixfdlist.h>
#define LANDLINE_HAVE_FD_LIST 1
#endif

namespace tether {

    namespace {
        constexpr const char* LOGIND_NAME = "org.freedesktop.login1";
        constexpr const char* LOGIND_PATH = "/org/freedesktop/login1";
        constexpr const char* LOGIND_MANAGER = "org.freedesktop.login1.Manager";
        constexpr int CALL_TIMEOUT_MS = 1000;
    } // namespace

    void SleepAnnouncer::start(GDBusConnection* conn, Invoker invoke, Handler handler) {
        stop();
        if (!conn || !invoke || !handler)
            return;
        conn_ = conn;
        invoke_ = std::move(invoke);
        handler_ = std::move(handler);
        invoke_([this] {
            subscription_ = g_dbus_connection_signal_subscribe(conn_,
                                                               LOGIND_NAME,
                                                               LOGIND_MANAGER,
                                                               "PrepareForSleep",
                                                               LOGIND_PATH,
                                                               nullptr,
                                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                                               on_signal,
                                                               this,
                                                               nullptr);
        });
        take_inhibitor();
    }

    void SleepAnnouncer::stop() {
        if (conn_ && subscription_ && invoke_) {
            invoke_([this] {
                g_dbus_connection_signal_unsubscribe(conn_, subscription_);
                if (hold_source_) {
                    if (GSource* source =
                            g_main_context_find_source_by_id(g_main_context_get_thread_default(), hold_source_))
                        g_source_destroy(source);
                    hold_source_ = 0;
                }
            });
        }
        subscription_ = 0;
        drop_inhibitor();
        conn_ = nullptr;
        invoke_ = nullptr;
        handler_ = nullptr;
    }

    void SleepAnnouncer::ready_to_sleep() { drop_inhibitor(); }

    void SleepAnnouncer::take_inhibitor() {
#ifdef LANDLINE_HAVE_FD_LIST
        if (!conn_)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inhibitor_fd_ >= 0)
                return;
        }
        GError* error = nullptr;
        GUnixFDList* fds = nullptr;
        GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(
            conn_,
            LOGIND_NAME,
            LOGIND_PATH,
            LOGIND_MANAGER,
            "Inhibit",
            g_variant_new("(ssss)", "sleep", "Landline", "Closing the Bluetooth link to the iPhone", "delay"),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START,
            CALL_TIMEOUT_MS,
            nullptr,
            &fds,
            nullptr,
            &error);
        if (!reply) {
            // Without it the announcement still comes; the close may not be done in time.
            debug::log(DEBUG, "sleep: no delay inhibitor ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }
        gint32 handle = -1;
        g_variant_get(reply, "(h)", &handle);
        const int fd = fds ? g_unix_fd_list_get(fds, handle, nullptr) : -1;
        g_variant_unref(reply);
        if (fds)
            g_object_unref(fds);
        std::lock_guard<std::mutex> lock(mutex_);
        inhibitor_fd_ = fd;
#endif
    }

    void SleepAnnouncer::drop_inhibitor() {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd = inhibitor_fd_;
            inhibitor_fd_ = -1;
        }
        if (fd >= 0)
            close(fd);
    }

    gboolean SleepAnnouncer::on_hold_expired(gpointer user_data) {
        auto* self = static_cast<SleepAnnouncer*>(user_data);
        self->hold_source_ = 0;
        self->drop_inhibitor();
        return G_SOURCE_REMOVE;
    }

    void SleepAnnouncer::on_signal(GDBusConnection*,
                                   const gchar*,
                                   const gchar*,
                                   const gchar*,
                                   const gchar*,
                                   GVariant* parameters,
                                   gpointer user_data) {
        auto* self = static_cast<SleepAnnouncer*>(user_data);
        gboolean sleeping = FALSE;
        g_variant_get(parameters, "(b)", &sleeping);
        debug::log(INFO, "sleep: logind says the computer {}", sleeping ? "is about to sleep" : "has woken up");
        if (sleeping && !self->hold_source_) {
            // Whatever the supervisor does, logind is not held for long.
            GSource* source = g_timeout_source_new(SLEEP_HOLD_MAX_MS);
            g_source_set_callback(source, on_hold_expired, self, nullptr);
            self->hold_source_ = g_source_attach(source, g_main_context_get_thread_default());
            g_source_unref(source);
        }
        if (self->handler_)
            self->handler_(sleeping);
        if (!sleeping)
            self->take_inhibitor();
    }

} // namespace tether
