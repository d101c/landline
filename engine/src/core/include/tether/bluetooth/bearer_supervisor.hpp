#pragma once

#include "tether/bluetooth/bond_health.hpp"
#include "tether/bluetooth/le_first.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tether::bluetooth {

    inline constexpr int BEARER_POLL_SECONDS = 5;
    inline constexpr int BEARER_SETTLE_SECONDS = 3;
    inline constexpr int BEARER_BACKOFF_MIN_SECONDS = 5;
    inline constexpr int BEARER_BACKOFF_MAX_SECONDS = 300;

    // Ceiling for the BR/EDR backoff while the phone does not answer pages.
    // Paging an absent phone every 5s can wedge bluetoothd in the kernel's
    // l2cap connect; a minute still finds it soon after it returns without LE.
    inline constexpr int ABSENT_CLASSIC_BACKOFF_MAX_SECONDS = 60;

    // Ceiling for the BR/EDR backoff while LE is connected.
    inline constexpr int LE_UP_CLASSIC_BACKOFF_MAX_SECONDS = 30;

    // How many refused attempts before the status stops saying "connecting"
    inline constexpr int CLASSIC_FAILURES_BEFORE_ADVICE = 6;

    // How long LE may stay down, with the solicitation broadcasting, before the
    // status stops saying "waiting" and names the phone-side Bluetooth cycle.
    // A phone that is answering does so in about a second, so silence this far
    // past that is the iPhone having gone quiet, not a slow connect.
    inline constexpr int LE_SILENT_SECONDS = 180;

    // How long an ANCS session is held across an LE outage before the path is
    // dropped. The bearer flaps faster than a GATT subscription can be rebuilt,
    // so a blip must not reset discovery, while a genuine loss still clears.
    inline constexpr int ANCS_BEARER_GRACE_SECONDS = 30;

    // How long BR/EDR may be up with no hands-free profile before the bearer is
    // cycled once to make BlueZ connect it. Was 20: after a link loss the phone
    // sat without call control for that long on top of everything else. Too
    // short cuts a link whose hands-free was merely late, at the cost of the
    // OBEX sessions on it, so TETHER_HFP_ABSENT_SECONDS overrides it.
    inline constexpr int HFP_ABSENT_SECONDS = 8;
    inline constexpr int HFP_ABSENT_SECONDS_MIN = 3;
    inline constexpr int HFP_ABSENT_SECONDS_MAX = 120;

    // The override when it reads as a whole number of seconds within bounds,
    // else the fallback.
    int hfp_absent_seconds_from(const char* text, int fallback);

    // How long BR/EDR may be up with no hands-free profile before BlueZ is asked
    // for that one profile over the link as it stands. Late enough that a
    // hands-free connection the phone opens itself, about six seconds after the
    // link, is normally there first; early enough to leave the request three
    // seconds before the cycle above, which still follows if nothing comes.
    inline constexpr int HFP_REQUEST_SECONDS = 5;

    // TETHER_HFP_CONNECT_PROFILE=0 goes back to the cycle alone.
    inline bool hfp_request_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // How long an LE link may read connected while the phone offers no ANCS
    // service before the solicitation goes back on air over it. Long enough for
    // GATT discovery on a freshly opened link, which runs to about a minute.
    inline constexpr int ANCS_ABSENT_GRACE_SECONDS = 75;

    // How long every bearer must stay down, after a link supervision timeout,
    // before the phone counts as gone rather than flapping.
    inline constexpr int AWAY_LOCK_GRACE_SECONDS = 30;

    // org.bluez Disconnected reasons.
    inline constexpr const char* REASON_TIMEOUT = "org.bluez.Reason.Timeout";

    // What BlueZ did with a request to bring a bearer up. Accepting a request is
    // not a link: only an observed Connected is that.
    enum class ConnectResult {
        Requested, // BlueZ took the request; a link may or may not follow
        Busy,      // a connect is already running, so do not start another
        Failed,    // refused outright
    };

    // The BlueZ operations the supervisor performs.
    class BearerOps {
    public:
        virtual ~BearerOps() = default;

        virtual bool device_present() const = 0;
        virtual bool device_paired() const = 0;
        virtual bool classic_connected() const = 0;
        virtual bool le_bearer_available() const = 0;
        virtual bool le_connected() const = 0;
        // Device1.PreferredBearer, or empty when BlueZ does not expose it.
        virtual std::string preferred_bearer() const = 0;
        // Writes Device1.PreferredBearer. Never while a connect is in flight:
        // that cancels it as le-connection-abort-by-local.
        virtual void set_preferred_bearer(const std::string& bearer) = 0;

        // May answer later: Requested then only means the call was issued, and a
        // refusal is handed to a later call. See classic_connect_outstanding().
        virtual ConnectResult connect_classic(std::string& err) = 0;
        // Whether a BR/EDR connect is still waiting for BlueZ's answer. Always
        // false for an implementation that answers within connect_classic().
        virtual bool classic_connect_outstanding() const { return false; }
        // Drops a refusal kept from an earlier attempt, so the next
        // connect_classic() issues a call instead of reporting old news.
        virtual void forget_classic_refusal() {}
        // Must not block: BlueZ keeps working on a connect after a synchronous
        // call gives up, and every later attempt then collides with it.
        virtual ConnectResult connect_le(std::string& err) = 0;
        // Whether a connect request is still outstanding
        virtual bool le_connect_outstanding() const = 0;
        // Whether the ANCS solicitation is registered AND BlueZ is advertising.
        virtual bool solicitation_on_air() const = 0;

        // Whether BlueZ exports a telephony object for the device.
        virtual bool telephony_present() const = 0;
        // Asks BlueZ to connect hands-free over the BR/EDR link that is already
        // up. Must not block; false when nothing was asked.
        virtual bool request_hands_free() { return false; }
        // Drops the BR/EDR bearer on its own, leaving LE up. Never Device1.Disconnect().
        virtual bool disconnect_classic(std::string& err) = 0;
        // Drops every link to the phone: Device1.Disconnect(). Only for the
        // recycle of le_first.hpp. Must not block.
        virtual bool disconnect_all(std::string& err) {
            err = "not supported";
            return false;
        }
        // Whether the phone has offered its notification service on this
        // bond: an LE link is then to be expected from it.
        virtual bool phone_offers_ancs() const { return false; }
        // A call is up, or the phone's audio is playing on this machine:
        // not the moment to drop the link on purpose.
        virtual bool phone_busy() const { return false; }

        // What BlueZ says about the keys behind the bond. The default knows
        // nothing beyond Device1.Paired, which reads as a bond in order.
        virtual BondFacts bond_facts() const {
            BondFacts facts;
            facts.paired = device_paired();
            return facts;
        }
        // Disconnected signals with Reason.Authentication seen so far for the
        // device. Only its change matters.
        virtual uint64_t auth_failure_count() const { return 0; }
    };

    struct BearerStatus {
        bool device_present = false;
        bool device_paired = false;
        bool classic_connected = false;
        bool le_connected = false;
        bool le_available = false;
        bool le_dialling = false;
        int classic_backoff = 0;
        std::string reason;
        // bond_state, bond_detail and needs_repair, as published.
        BondHealth bond;
        // The bond is broken and this is not one of the spaced-out windows in
        // which it is tried again: nothing is to be asked of the phone.
        bool standing_back = false;
        // No link is up and BR/EDR is being left alone for a few seconds, so
        // that the phone opens the LE link first (le_first.hpp). No MAP or
        // PBAP session is to be opened meanwhile: obexd would bring BR/EDR up.
        bool holding_for_le = false;

        bool operator==(const BearerStatus&) const = default;
    };

    // Whether the ANCS solicitation belongs on air. A live LE link normally means
    // the phone has been reached and the advert is not needed, but a link that
    // carries no ANCS is, for notifications, no link at all and the advert is
    // the only thing that asks the phone for the service. `ancs_absent_since` is
    // when the link was first seen up with no ANCS offered, or -1 when it is
    // offered or the link is down. Never solicit over a dial still in flight:
    // that is what aborted the connect once per cycle.
    inline bool
        should_solicit_ancs(bool ancs_enabled, const BearerStatus& bearer, int64_t now, int64_t ancs_absent_since) {
        const bool link_dead = ancs_absent_since >= 0 && now - ancs_absent_since >= ANCS_ABSENT_GRACE_SECONDS;
        // Not for a broken bond: the advert is what calls the phone back onto
        // an LE link it then fails to authenticate, every few seconds.
        if (bearer.standing_back)
            return false;
        return ancs_enabled && bearer.le_available && !bearer.le_dialling && (!bearer.le_connected || link_dead);
    }

    // Whether MAP and PBAP sessions are opened. `session_open` is one that
    // already is: it is proof the phone serves this bond, whatever was counted.
    inline bool should_open_profiles(const BearerStatus& bearer, bool session_open) {
        if (bearer.holding_for_le && !session_open)
            return false;
        return !bearer.standing_back || session_open;
    }

    // Whether the ANCS client keeps its device path. Without a populated
    // Bearer.LE1 the LE state is invisible, so the classic link being up is the
    // only sign the phone is back. Discovery decides whether ANCS answers.
    inline bool should_hold_ancs(bool ready, const BearerStatus& bearer, int64_t now, int64_t le_down_since) {
        return ready || bearer.le_connected || (bearer.classic_connected && !bearer.le_available) ||
               now - le_down_since < ANCS_BEARER_GRACE_SECONDS;
    }

    // Whether the phone has been gone long enough, and for the right reason, to
    // lock the session.
    inline bool should_lock_on_away(bool enabled,
                                    const BearerStatus& bearer,
                                    bool obex_up,
                                    const std::string& reason,
                                    int64_t now,
                                    int64_t gone_since,
                                    int grace_seconds) {
        if (!enabled || gone_since < 0 || obex_up)
            return false;
        if (!bearer.device_paired || bearer.classic_connected || bearer.le_connected)
            return false;
        if (reason != REASON_TIMEOUT)
            return false;
        return now - gone_since >= grace_seconds;
    }

    // Keeps the Classic and LE halves of one bond connected.
    //
    // Order is important: BR/EDR first, a settling interval, then LE
    class BearerSupervisor {
    public:
        BearerSupervisor(BearerOps& ops, bool ancs_enabled);

        // Drive once per BEARER_POLL_SECONDS with a monotonic clock. Returns true
        // when the reported status changed, so callers only broadcast on change.
        // `obex_up` is an open MAP or PBAP session, which is proof BR/EDR reaches
        // the phone whatever BlueZ reports about the link.
        bool tick(int64_t now, bool obex_up = false);

        // Clears connection state and backoff after a disconnect or a resume, so
        // recovery starts immediately instead of waiting out a stale backoff.
        void reset();

        void set_ancs_enabled(bool enabled);
        void set_calls_enabled(bool enabled);
        // How long BR/EDR may stay up without hands-free before the one cycle.
        void set_hfp_absent_seconds(int seconds);
        // Whether hands-free is asked for over the live link before the cycle.
        void set_hfp_request_enabled(bool enabled);
        // Whether this machine can carry the LE half of a bond at all. Where it
        // cannot, a BR/EDR-only bond is not one to pair again over.
        void set_le_expected(bool expected) { le_expected_ = expected; }
        // How many times the BR/EDR link was dropped on purpose for hands-free.
        // A caller that sees it move knows the drop that follows is no outage.
        int hfp_cycles() const { return hfp_cycles_; }
        // How many times every link was dropped on purpose so that LE comes
        // back first, and both together.
        int le_recycles() const { return le_recycles_; }
        int planned_drops() const { return hfp_cycles_ + le_recycles_; }
        // The "LE first" measures. Off until set: see le_first.hpp.
        void set_le_first(const LeFirstConfig& config) { le_first_ = config; }
        // bluetoothd was restarted: what was counted against the old one
        // (a recycle spent, a hold that ran out) says nothing about this one.
        void bluez_restarted();
        const BearerStatus& status() const { return status_; }

    private:
        // The links, as tick() has always driven them.
        void supervise(int64_t now, bool obex_up);
        // Reads the bond, counts what the link reported since the last tick,
        // and decides whether this tick stands back.
        void assess(int64_t now);

        BearerOps& ops_;
        bool le_expected_ = true;
        AuthFailureTracker auth_;
        bool auth_baseline_set_ = false;
        uint64_t auth_seen_ = 0;
        // Start of the next window of attempts on a broken bond.
        int64_t next_probe_at_ = -1;
        bool partial_logged_ = false;
        bool ancs_enabled_ = true;
        bool calls_enabled_ = false;
        int hfp_absent_seconds_ = HFP_ABSENT_SECONDS;

        BearerStatus status_;
        int classic_failures_ = 0;
        bool le_dial_spent_ = false;
        bool hfp_cycle_spent_ = false;
        bool hfp_request_enabled_ = true;
        bool hfp_request_spent_ = false;
        int hfp_cycles_ = 0;
        int64_t telephony_absent_since_ = -1;
        int64_t classic_connected_since_ = -1;
        int64_t le_down_since_ = -1;
        // Whether the solicitation has been observed on air since LE went down.
        bool solicited_since_le_down_ = false;
        // Whether the bearer preference has been handed back to LE this Classic session.
        bool bearer_handed_back_ = false;

        // le_first.hpp. None of this is cleared by reset(): a link that drops
        // is exactly what the hold and the recycle are about.
        LeFirstConfig le_first_ = le_first_off();
        LeExpectation le_expectation() const;
        void unpin_bearer();
        int64_t all_down_since_ = -1;
        int64_t advert_on_air_since_ = -1;
        int64_t hold_expired_at_ = -1;
        int64_t last_recycle_at_ = -1;
        bool le_recycle_spent_ = false;
        int le_recycles_ = 0;
        bool unpin_logged_ = false;
        int64_t next_classic_attempt_ = 0;
    };

} // namespace tether::bluetooth
