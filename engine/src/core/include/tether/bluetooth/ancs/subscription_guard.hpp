#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// When StartNotify and StopNotify may be sent to BlueZ for an ANCS
// characteristic. Everything here is a decision on facts handed in, so that
// the rules can be tested without BlueZ.
//
// Why there are rules at all. bluetoothd (5.87, and its main branch as of
// November 2026) aborts on
//
//     src/shared/gatt-client.c: disable_ccc_callback:
//         Assertion `notify_data->chrc->ccc_write_id' failed.
//
// when two "disable notifications" writes are outstanding on one
// characteristic. A D-Bus client produces that with three calls made before
// the phone has answered the first write:
//
//     StopNotify    the last subscriber goes: write CCC=0 (call it D1)
//     StartNotify   queued behind D1, no reply yet
//     StopNotify    the queued subscriber goes: its count drops to zero and a
//                   second CCC=0 is written (D2), overwriting the id of D1
//
// D1's answer clears the id; D2's answer then finds it clear and asserts. On
// a healthy link D1 is answered within a connection interval and the window
// is milliseconds. On a link that has stopped answering it is the whole ATT
// timeout, thirty seconds, at the end of which both writes are failed in a
// row and bluetoothd dies, taking every Bluetooth device of the session with
// it. A subscriber that leaves the bus while queued does the same as the
// third call.
//
// The rules: a session being rebuilt is never stopped, BlueZ re-registers
// its subscribers on its own when the link returns; the one StopNotify left
// (notifications turned off) goes out only on a live link whose
// characteristic reads Notifying; and no StartNotify follows a StopNotify
// until the ATT timeout has run out, so nothing is ever queued behind D1.
namespace tether::bluetooth::ancs {

    // BlueZ fails an ATT request after 30 s (ATT_TIMEOUT_INTERVAL). Past this
    // long after a StopNotify, its write has been answered or failed.
    inline constexpr int CCC_WRITE_SETTLE_SECONDS = 35;

    // How long a StartNotify may take before the caller stops waiting. BlueZ
    // goes on with it; what it becomes is read from Notifying.
    inline constexpr int START_NOTIFY_TIMEOUT_MS = 4000;

    // How often StartNotify is sent again for a session that reads subscribed
    // and gets no answer from the phone. It costs nothing when BlueZ still
    // holds the subscriber, and makes a new one when BlueZ dropped it.
    inline constexpr int RESUBSCRIBE_SECONDS = 10;

    // What is asked of a phone that is subscribed to and does not answer:
    // the readiness probe (a write), the liveness read behind every
    // subscription attempt, the check of the subscription. Close together
    // while someone may be turning the switch on, `watch_seconds` from the
    // start of the session or from a client asking; far apart after that. A
    // phone whose owner never allows notifications was otherwise written to
    // every five seconds for as long as the link lasted.
    inline constexpr int UNANSWERED_FAR_SECONDS = 60;

    inline int unanswered_seconds(int64_t now, int64_t settle_since, int close_seconds, int watch_seconds) {
        if (settle_since < 0 || now < settle_since || now - settle_since < watch_seconds)
            return close_seconds;
        return close_seconds > UNANSWERED_FAR_SECONDS ? close_seconds : UNANSWERED_FAR_SECONDS;
    }

    // TETHER_ANCS_STOP_NOTIFY=0 never sends StopNotify at all.
    inline bool stop_notify_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // Why a subscription is being let go.
    enum class Release {
        // The session is rebuilt: link lost, Notifying cleared, paths moved,
        // another device path. The subscriber stays in BlueZ.
        Rebuild,
        // Notifications were turned off, or the client is going away.
        TurnedOff,
    };

    struct StopFacts {
        Release why = Release::Rebuild;
        // The ATT link answered a read just now.
        bool link_alive = false;
        // The characteristic read Notifying just now: the subscriber is
        // registered and its own CCC write has been answered.
        bool notifying = false;
        // Seconds on the caller's clock; `last_stop_at` is negative when no
        // StopNotify was sent by this process.
        int64_t now = 0;
        int64_t last_stop_at = -1;
        bool enabled = true;
    };

    enum class StopVerdict {
        Send,
        // A rebuild never stops anything.
        KeepForRebuild,
        // Nothing to write to: BlueZ drops the session with the link.
        LinkDown,
        // Not registered, or registered and still waiting for its write: the
        // case that queues a second disable.
        NotNotifying,
        // A StopNotify went out less than CCC_WRITE_SETTLE_SECONDS ago.
        TooSoon,
        Disabled,
    };

    inline StopVerdict stop_notify_verdict(const StopFacts& facts) {
        if (!facts.enabled)
            return StopVerdict::Disabled;
        if (facts.why == Release::Rebuild)
            return StopVerdict::KeepForRebuild;
        if (!facts.link_alive)
            return StopVerdict::LinkDown;
        if (!facts.notifying)
            return StopVerdict::NotNotifying;
        if (facts.last_stop_at >= 0 && facts.now - facts.last_stop_at < CCC_WRITE_SETTLE_SECONDS)
            return StopVerdict::TooSoon;
        return StopVerdict::Send;
    }

    // Whether StartNotify may go out: never while a StopNotify of ours may
    // still have its write outstanding.
    inline bool start_notify_allowed(int64_t now, int64_t last_stop_at) {
        return last_stop_at < 0 || now < last_stop_at || now - last_stop_at >= CCC_WRITE_SETTLE_SECONDS;
    }

    // Seconds left before start_notify_allowed() turns true, zero when it is.
    inline int start_notify_wait(int64_t now, int64_t last_stop_at) {
        if (start_notify_allowed(now, last_stop_at))
            return 0;
        return static_cast<int>(CCC_WRITE_SETTLE_SECONDS - (now - last_stop_at));
    }

    // What one StartNotify came to.
    enum class StartOutcome {
        // BlueZ holds the subscriber and the characteristic reads Notifying.
        Live,
        // Taken or still being worked on, not Notifying yet: wait, and never
        // answer it with a StopNotify.
        Pending,
        // The characteristic is gone: rediscover.
        Gone,
        // Refused outright: try again later.
        Failed,
    };

    // `replied_ok` is a method return; `timed_out` is this side giving up on
    // the reply; `error` is the D-Bus error text otherwise. `notifying` is the
    // property read right after the call, false when it could not be read.
    inline StartOutcome classify_start(bool replied_ok, bool timed_out, const std::string& error, bool notifying) {
        if (!replied_ok && !timed_out && error.find("UnknownObject") != std::string::npos)
            return StartOutcome::Gone;
        if (replied_ok)
            return notifying ? StartOutcome::Live : StartOutcome::Pending;
        // BlueZ answers InProgress for a subscriber it holds while the device
        // is between two links, and a reply that never came is a subscriber
        // queued behind another write.
        if (timed_out || error.find("InProgress") != std::string::npos ||
            error.find("In Progress") != std::string::npos)
            return StartOutcome::Pending;
        return StartOutcome::Failed;
    }

} // namespace tether::bluetooth::ancs
