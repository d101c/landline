#include <gtest/gtest.h>
#include <tether/bluetooth/obex_transfer.hpp>
#include <tether/bluetooth/profile_supervisor.hpp>
#include <tether/bus_watch.hpp>

#include <atomic>
#include <chrono>
#include <gio/gio.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace tether;

namespace {

    // A D-Bus connection over a socket pair: no bus, no daemon, nothing of the
    // user's session. Closing `peer_fd` is the far end going away, which is
    // what a session bus that restarts looks like from inside the process.
    struct PeerConnection {
        GDBusConnection* conn = nullptr;
        int peer_fd = -1;

        PeerConnection() {
            int fds[2] = {-1, -1};
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
                return;
            peer_fd = fds[1];
            GError* error = nullptr;
            GSocket* socket = g_socket_new_from_fd(fds[0], &error);
            if (!socket) {
                g_clear_error(&error);
                close(fds[0]);
                return;
            }
            GSocketConnection* stream = g_socket_connection_factory_create_connection(socket);
            conn = g_dbus_connection_new_sync(
                G_IO_STREAM(stream), nullptr, G_DBUS_CONNECTION_FLAGS_NONE, nullptr, nullptr, &error);
            g_clear_error(&error);
            g_object_unref(stream);
            g_object_unref(socket);
        }

        void hang_up() {
            if (peer_fd >= 0)
                close(peer_fd);
            peer_fd = -1;
        }

        ~PeerConnection() {
            hang_up();
            g_clear_object(&conn);
        }
    };

    struct Closed {
        std::mutex mutex;
        std::vector<std::string> names;

        void add(const std::string& name) {
            std::lock_guard<std::mutex> lock(mutex);
            names.push_back(name);
        }

        size_t count() {
            std::lock_guard<std::mutex> lock(mutex);
            return names.size();
        }

        bool wait(size_t wanted, int timeout_ms = 3000) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                if (count() >= wanted)
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return count() >= wanted;
        }
    };

} // namespace

TEST(BusWatch, SwitchedOffOnlyByZero) {
    EXPECT_TRUE(bus_watch_enabled(nullptr));
    EXPECT_TRUE(bus_watch_enabled(""));
    EXPECT_TRUE(bus_watch_enabled("1"));
    EXPECT_FALSE(bus_watch_enabled("0"));
}

// Restart=on-failure only restarts on a status that is not zero.
TEST(BusWatch, ExitStatusIsAFailure) { EXPECT_NE(BUS_CLOSED_EXIT_STATUS, 0); }

TEST(BusWatch, ReportsAConnectionWhoseFarEndWentAway) {
    PeerConnection peer;
    ASSERT_NE(peer.conn, nullptr);

    Closed closed;
    BusWatch watch([&](const std::string& name) { closed.add(name); }, 10);
    watch.watch("session", [&] { return G_DBUS_CONNECTION(g_object_ref(peer.conn)); });
    watch.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(closed.count(), 0u) << "a live connection must not be reported";

    peer.hang_up();
    ASSERT_TRUE(closed.wait(1));
    EXPECT_EQ(closed.names.front(), "session");

    // Once: the daemon leaves on the first report.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(closed.count(), 1u);
    watch.stop();
}

// A session bus that is not there yet at startup is taken up when it appears.
TEST(BusWatch, KeepsAskingForAConnectionThatIsNotThereYet) {
    PeerConnection peer;
    ASSERT_NE(peer.conn, nullptr);

    std::atomic<int> asked{0};
    Closed closed;
    BusWatch watch([&](const std::string& name) { closed.add(name); }, 10);
    watch.watch(
        "late",
        [&]() -> GDBusConnection* {
            if (++asked < 3)
                return nullptr;
            return G_DBUS_CONNECTION(g_object_ref(peer.conn));
        },
        10);
    watch.start();

    peer.hang_up();
    ASSERT_TRUE(closed.wait(1));
    EXPECT_GE(asked.load(), 3);
    EXPECT_EQ(closed.names.front(), "late");
}

TEST(BusWatch, WatchesSeveralConnectionsSeparately) {
    PeerConnection first, second;
    ASSERT_NE(first.conn, nullptr);
    ASSERT_NE(second.conn, nullptr);

    Closed closed;
    BusWatch watch([&](const std::string& name) { closed.add(name); }, 10);
    watch.watch("session", [&] { return G_DBUS_CONNECTION(g_object_ref(first.conn)); });
    watch.watch("system", [&] { return G_DBUS_CONNECTION(g_object_ref(second.conn)); });
    watch.start();

    second.hang_up();
    ASSERT_TRUE(closed.wait(1));
    EXPECT_EQ(closed.names.front(), "system");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(closed.count(), 1u) << "the other connection is still open";
}

// The text GDBus gives a call made on a closed connection is what the profile
// supervisor classifies, so it is taken from GDBus itself and not from memory.
TEST(BusWatch, ACallOnAClosedConnectionIsRecognised) {
    PeerConnection peer;
    ASSERT_NE(peer.conn, nullptr);
    peer.hang_up();
    for (int i = 0; i < 600 && !g_dbus_connection_is_closed(peer.conn); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(g_dbus_connection_is_closed(peer.conn));

    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(peer.conn,
                                                  nullptr,
                                                  "/org/bluez/obex",
                                                  "org.bluez.obex.Client1",
                                                  "CreateSession",
                                                  nullptr,
                                                  nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE,
                                                  1000,
                                                  nullptr,
                                                  &error);
    ASSERT_EQ(reply, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_TRUE(is_bus_closed_error(error));
    EXPECT_EQ(bluetooth::classify_obex_error(error->message), bluetooth::ObexError::BusClosed) << error->message;
    g_clear_error(&error);

    EXPECT_FALSE(is_bus_closed_error(nullptr));
    GError* other = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Timeout was reached");
    EXPECT_FALSE(is_bus_closed_error(other));
    g_clear_error(&other);
}

TEST(TransferIdle, RunsOnlyInsideTheScopeAndOnlyOnItsThread) {
    int runs = 0;
    bluetooth::run_transfer_idle();
    EXPECT_EQ(runs, 0);
    {
        bluetooth::TransferIdleScope scope([&] { ++runs; });
        bluetooth::run_transfer_idle();
        bluetooth::run_transfer_idle();
        EXPECT_EQ(runs, 2);

        // Another thread waiting on its own transfer (a message being sent)
        // must not run the supervisor's work.
        std::thread other([] { bluetooth::run_transfer_idle(); });
        other.join();
        EXPECT_EQ(runs, 2);
    }
    bluetooth::run_transfer_idle();
    EXPECT_EQ(runs, 2);
}

// The idle work may itself wait on a transfer; it must not start itself again.
TEST(TransferIdle, IsNotEnteredFromInsideItself) {
    int runs = 0;
    bluetooth::TransferIdleScope scope([&] {
        ++runs;
        bluetooth::run_transfer_idle();
    });
    bluetooth::run_transfer_idle();
    EXPECT_EQ(runs, 1);
    bluetooth::run_transfer_idle();
    EXPECT_EQ(runs, 2);
}

TEST(TransferIdle, ScopesNest) {
    std::string seen;
    bluetooth::TransferIdleScope outer([&] { seen += "o"; });
    {
        bluetooth::TransferIdleScope inner([&] { seen += "i"; });
        bluetooth::run_transfer_idle();
    }
    bluetooth::run_transfer_idle();
    EXPECT_EQ(seen, "io");
}
