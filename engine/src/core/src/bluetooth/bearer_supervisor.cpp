#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/bluetooth/objects.hpp"
#include "tether/log.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace tether::bluetooth {

    namespace {

        int grow_backoff(int current, int ceiling = BEARER_BACKOFF_MAX_SECONDS) {
            if (current <= 0)
                return BEARER_BACKOFF_MIN_SECONDS;
            return std::min(current * 2, ceiling);
        }

        // Failures that clear on their own, unlike a refusal from the phone.
        bool is_transient(const std::string& err) {
            static const char* const transient[] = {
                "org.bluez.Error.NotReady",
                "connection-abort",
                "Software caused connection abort",
                "br-connection-canceled",
                "le-connection-abort-by-local",
                "br-connection-refused",
            };
            for (const char* needle : transient) {
                if (err.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        // BlueZ is already working on a connect, or the link is already up.
        // Neither is a failure, and retrying at the minimum backoff is what turns
        // one stalled connect into a permanent InProgress storm.
        bool is_already_running(const std::string& err) {
            return err.find("InProgress") != std::string::npos ||
                   err.find("already in progress") != std::string::npos ||
                   err.find("AlreadyConnected") != std::string::npos ||
                   err.find("already-connected") != std::string::npos;
        }

        // Collapses an error a synchronous caller reported into the same shape an
        // asynchronous one returns, so both take one code path.
        ConnectResult classify(ConnectResult reported, const std::string& err) {
            return reported == ConnectResult::Failed && is_already_running(err) ? ConnectResult::Busy : reported;
        }

        // The phone did not answer the page: out of range or Bluetooth off.
        bool is_absent(const std::string& err) {
            static const char* const absent[] = {
                "page-timeout",
                "page timeout",
                "Host is down",
                "Connection timed out",
            };
            for (const char* needle : absent) {
                if (err.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        int next_backoff(int current, const std::string& err, int ceiling = BEARER_BACKOFF_MAX_SECONDS) {
            if (is_transient(err))
                return BEARER_BACKOFF_MIN_SECONDS;
            if (is_absent(err))
                return grow_backoff(current, std::min(ceiling, ABSENT_CLASSIC_BACKOFF_MAX_SECONDS));
            return grow_backoff(current, ceiling);
        }

        // Marked here, translated where it is read: gettext cannot run at namespace scope.
        constexpr const char* NO_LOCAL_PROFILE_ADVICE =
            N_("Messages and contacts are connected. BlueZ reports the Bluetooth link down because this computer "
               "offers the iPhone no profile to connect to — usual on a machine with no audio stack, or one where "
               "the a2dp_sink and hfp_hf roles are turned off. Nothing is missing and nothing needs changing on "
               "the iPhone.");

        constexpr const char* LE_NEVER_SOLICITED_ADVICE =
            N_("This computer is not putting the notification request on air, so the iPhone is never asked for "
               "the service. Nothing on the iPhone changes this and re-pairing will not either. Check the "
               "controller with landlinectl --bt-status; another one can be selected with landlinectl --bt-adapter.");

        constexpr const char* LE_BOND_PINNED_ADVICE =
            N_("This computer had pinned the bond to BR/EDR, which refuses the LE link the iPhone opens to carry "
               "notifications. That is being undone now; LE should come up within a minute. Nothing on the iPhone "
               "changes this and re-pairing will not either.");

        constexpr const char* LE_PHONE_SILENT_ADVICE =
            N_("The iPhone is not answering on LE. Its Bluetooth is wedged on its own side: turn Bluetooth off "
               "and back on on the iPhone, which clears this even when Share System Notifications is already on. "
               "Messages and contacts are unaffected.");

        constexpr const char* BOND_BROKEN_ADVICE =
            N_("The pairing with the iPhone no longer works: the iPhone keeps refusing to authenticate. On the "
               "iPhone, open Settings > Bluetooth > (i) and choose Forget This Device, then pair again from this "
               "computer. Until then this computer only tries again every ten minutes.");

        constexpr const char* BOND_PARTIAL_ADVICE =
            N_("The pairing with the iPhone is incomplete: it covers BR/EDR only, so notifications cannot work. On "
               "the iPhone, open Settings > Bluetooth > (i) and choose Forget This Device, then pair again from "
               "this computer.");

    } // namespace

    BearerSupervisor::BearerSupervisor(BearerOps& ops, bool ancs_enabled) : ops_(ops), ancs_enabled_(ancs_enabled) {}

    void BearerSupervisor::set_ancs_enabled(bool enabled) { ancs_enabled_ = enabled; }

    void BearerSupervisor::set_calls_enabled(bool enabled) { calls_enabled_ = enabled; }

    void BearerSupervisor::set_hfp_absent_seconds(int seconds) {
        hfp_absent_seconds_ = std::clamp(seconds, HFP_ABSENT_SECONDS_MIN, HFP_ABSENT_SECONDS_MAX);
    }

    void BearerSupervisor::set_hfp_request_enabled(bool enabled) { hfp_request_enabled_ = enabled; }

    int hfp_absent_seconds_from(const char* text, int fallback) {
        if (!text || !*text)
            return fallback;
        int value = 0;
        const char* end = text + std::strlen(text);
        const auto [parsed, ec] = std::from_chars(text, end, value);
        if (ec != std::errc() || parsed != end || value < HFP_ABSENT_SECONDS_MIN || value > HFP_ABSENT_SECONDS_MAX)
            return fallback;
        return value;
    }

    void BearerSupervisor::reset() {
        // hfp_cycle_spent_ deliberately survives
        classic_failures_ = 0;
        telephony_absent_since_ = -1;
        le_down_since_ = -1;
        solicited_since_le_down_ = false;
        classic_connected_since_ = -1;
        bearer_handed_back_ = false;
        next_classic_attempt_ = 0;
        status_.classic_backoff = 0;
        status_.le_dialling = ops_.le_connect_outstanding();
    }

    void BearerSupervisor::bluez_restarted() {
        reset();
        all_down_since_ = -1;
        advert_on_air_since_ = -1;
        hold_expired_at_ = -1;
        last_recycle_at_ = -1;
        le_recycle_spent_ = false;
        unpin_logged_ = false;
        status_.holding_for_le = false;
    }

    LeExpectation BearerSupervisor::le_expectation() const {
        LeExpectation le;
        le.ancs_enabled = ancs_enabled_;
        le.le_available = status_.le_available;
        le.phone_offers_ancs = ops_.phone_offers_ancs();
        le.standing_back = status_.standing_back;
        return le;
    }

    // A bond left preferring BR/EDR turns away the LE link the iPhone opens
    // (measured upstream as an A/B, 25 August 2026). The preference is only
    // needed while Device1.Connect is being answered; once no connect is in
    // flight it goes back to LE, whether the link came up or not.
    void BearerSupervisor::unpin_bearer() {
        if (!le_first_.unpin_after_attempt || !ancs_enabled_ || !status_.le_available)
            return;
        if (ops_.classic_connect_outstanding() || ops_.le_connect_outstanding())
            return;
        if (ops_.preferred_bearer() != "bredr")
            return;
        ops_.set_preferred_bearer("le");
        if (!unpin_logged_)
            debug::log(INFO,
                       "bluetooth: bond was left preferring BR/EDR with no connect in flight; preference handed "
                       "back to LE so the link the iPhone opens is accepted");
        unpin_logged_ = true;
    }

    bool BearerSupervisor::tick(int64_t now, bool obex_up) {
        const BearerStatus previous = status_;
        supervise(now, obex_up);

        // Whatever the links are doing, a bond that has to be made again is
        // the thing to say: every other sentence sends the user elsewhere.
        if (status_.bond.state == BondState::Broken)
            status_.reason = _(BOND_BROKEN_ADVICE);
        else if (status_.bond.needs_repair)
            status_.reason = _(BOND_PARTIAL_ADVICE);
        return !(status_ == previous);
    }

    void BearerSupervisor::assess(int64_t now) {
        const uint64_t failures = ops_.auth_failure_count();
        // What was counted before this supervisor existed belongs to another bond.
        if (!auth_baseline_set_) {
            auth_baseline_set_ = true;
            auth_seen_ = failures;
        }
        const bool was_broken = auth_.broken();
        bool failed_again = false;
        if (failures > auth_seen_) {
            failed_again = true;
            if (auth_.note_auth_failures(now, failures - auth_seen_))
                debug::log(WARN,
                           "bluetooth: the bond with the iPhone is broken: {} authentication failures within {}s. "
                           "Standing back: one {}s window of attempts every {}s from now on. Remove the pairing on "
                           "both sides and pair again.",
                           auth_.recent_failures(),
                           AUTH_FAILURE_WINDOW_SECONDS,
                           BROKEN_BOND_PROBE_SECONDS,
                           BROKEN_BOND_RETRY_SECONDS);
        }
        auth_seen_ = failures;

        if (auth_.observe_le(now, status_.le_connected))
            debug::log(INFO,
                       "bluetooth: the LE link has held {}s; the bond works after all, normal supervision resumes",
                       BOND_RECOVERY_SECONDS);

        if (!auth_.broken()) {
            next_probe_at_ = -1;
            status_.standing_back = false;
        } else if (!was_broken || next_probe_at_ < 0) {
            next_probe_at_ = now + BROKEN_BOND_RETRY_SECONDS;
            status_.standing_back = true;
        } else {
            const bool probing = now >= next_probe_at_ && now < next_probe_at_ + BROKEN_BOND_PROBE_SECONDS;
            // A window that met another refusal has its answer; one that ran
            // out without an LE link that holds has it too.
            if ((probing && failed_again) || now >= next_probe_at_ + BROKEN_BOND_PROBE_SECONDS)
                next_probe_at_ = now + BROKEN_BOND_RETRY_SECONDS;
            const bool opens = now >= next_probe_at_ && status_.standing_back;
            status_.standing_back = now < next_probe_at_;
            if (opens) {
                // The window starts from a clean slate, or it would spend
                // itself waiting out a backoff.
                next_classic_attempt_ = 0;
                status_.classic_backoff = 0;
                ops_.forget_classic_refusal();
            }
        }

        const BondHealth assessed = assess_bond(ops_.bond_facts(), le_expected_);
        status_.bond = combine_bond(assessed, auth_.broken(), auth_.detail());
        if (status_.bond.state == BondState::Partial && status_.bond.needs_repair) {
            if (!partial_logged_) {
                partial_logged_ = true;
                debug::log(WARN,
                           "bluetooth: the bond with the iPhone is partial ({}): notifications cannot work on it. "
                           "Remove the pairing on both sides and pair again.",
                           status_.bond.detail);
            }
        } else {
            partial_logged_ = false;
        }
    }

    void BearerSupervisor::supervise(int64_t now, bool obex_up) {
        const bool le_was_connected = status_.le_connected;
        status_.device_present = ops_.device_present();
        status_.device_paired = status_.device_present && ops_.device_paired();
        if (!status_.device_present || !status_.device_paired) {
            status_.classic_connected = false;
            status_.le_connected = false;
            status_.le_available = false;
            status_.le_dialling = ops_.le_connect_outstanding();
            classic_connected_since_ = -1;
            status_.reason = status_.device_present ? "iPhone is not paired." : "iPhone not known to BlueZ.";
            status_.holding_for_le = false;
            all_down_since_ = -1;
            advert_on_air_since_ = -1;
            // No bond, so nothing left to be broken: a pairing made after this
            // starts from nothing.
            auth_.reset();
            auth_baseline_set_ = false;
            next_probe_at_ = -1;
            partial_logged_ = false;
            status_.bond = BondHealth{};
            status_.standing_back = false;
            return;
        }

        status_.classic_connected = ops_.classic_connected();
        status_.le_available = ops_.le_bearer_available();
        status_.le_connected = status_.le_available && ops_.le_connected();
        if (status_.le_connected) {
            le_down_since_ = -1;
            solicited_since_le_down_ = false;
        } else if (le_down_since_ < 0) {
            le_down_since_ = now;
        }
        const bool on_air = !status_.le_connected && ops_.solicitation_on_air();
        if (on_air)
            solicited_since_le_down_ = true;

        // What le_first.hpp decides on.
        if (!on_air)
            advert_on_air_since_ = -1;
        else if (advert_on_air_since_ < 0)
            advert_on_air_since_ = now;
        const bool was_holding = status_.holding_for_le;
        const int64_t down_since = all_down_since_;
        if (status_.classic_connected || status_.le_connected || obex_up)
            all_down_since_ = -1;
        else if (all_down_since_ < 0)
            all_down_since_ = now;
        if (status_.le_connected) {
            le_recycle_spent_ = false;
            hold_expired_at_ = -1;
            unpin_logged_ = false;
            if (was_holding && down_since >= 0)
                debug::log(INFO,
                           "bluetooth: the LE link came first, {}s after every link was down; BR/EDR follows",
                           now - down_since);
        }
        status_.holding_for_le = false;

        if (status_.le_connected)
            status_.le_dialling = false;

        // The phone opening the LE link proves it is in range and answering
        if (status_.le_connected && !le_was_connected) {
            status_.classic_backoff = 0;
            next_classic_attempt_ = 0;
            classic_failures_ = 0;
            // What an attempt made while the phone was away reported says
            // nothing about a phone that is back. Left in place it would be
            // handed to the next attempt, which then stands back instead of
            // connecting, and obexd brings the link up alone, without hands-free.
            ops_.forget_classic_refusal();
        }

        assess(now);
        if (status_.classic_connected)
            auth_.note_classic_connected();

        // A broken bond between two windows: the links are reported as they
        // are and nothing is asked of the phone. No connect, no dial, no cut
        // for hands-free, each of which only draws another refusal.
        if (status_.standing_back) {
            if (!status_.classic_connected)
                classic_connected_since_ = -1;
            else if (classic_connected_since_ < 0)
                classic_connected_since_ = now;
            status_.le_dialling = ops_.le_connect_outstanding();
            return;
        }

        // No link at all, and an LE link to be expected: the advert is left
        // alone on air for a few seconds before anything is asked over BR/EDR.
        {
            HoldFacts hold;
            hold.le = le_expectation();
            hold.le_connected = status_.le_connected;
            hold.classic_connected = status_.classic_connected;
            hold.obex_up = obex_up;
            hold.now = now;
            hold.all_down_since = all_down_since_;
            status_.holding_for_le = hold_classic_for_le(le_first_, hold);
            if (status_.holding_for_le && !was_holding)
                debug::log(INFO,
                           "bluetooth: no link to the iPhone; leaving BR/EDR alone for up to {}s so that it opens "
                           "the LE link first",
                           le_first_.hold_seconds);
            if (was_holding && !status_.holding_for_le && all_down_since_ >= 0) {
                hold_expired_at_ = now;
                debug::log(INFO,
                           "bluetooth: no LE link after {}s with the solicitation alone on air; connecting over "
                           "BR/EDR",
                           now - all_down_since_);
            }
        }
        if (status_.holding_for_le) {
            classic_connected_since_ = -1;
            bearer_handed_back_ = false;
            hfp_request_spent_ = false;
            unpin_bearer();
            status_.le_dialling = ops_.le_connect_outstanding();
            status_.reason = _("Reconnecting. Waiting a few seconds for the iPhone to open the link that carries "
                               "notifications first...");
            return;
        }

        if (status_.classic_connected) {
            if (classic_connected_since_ < 0)
                classic_connected_since_ = now;
            status_.classic_backoff = 0;
            classic_failures_ = 0;
        } else {
            // A dropped link invalidates the settle window, so no LE attempt is
            // made until BR/EDR is back and has settled. An LE link that is
            // already up is a separate matter and is reported as it is: ANCS
            // rides LE alone, so calling it down here would switch notification
            // mirroring off for a phone that is still delivering notifications.
            classic_connected_since_ = -1;
            bearer_handed_back_ = false;
            hfp_request_spent_ = false;

            const int ceiling = status_.le_connected ? LE_UP_CLASSIC_BACKOFF_MAX_SECONDS : BEARER_BACKOFF_MAX_SECONDS;

            // A connect still waiting for its answer is neither retried nor
            // counted: the answer, not the clock, decides what happens next.
            if (ops_.classic_connect_outstanding()) {
            } else if (now >= next_classic_attempt_) {
                std::string err;
                switch (classify(ops_.connect_classic(err), err)) {
                case ConnectResult::Requested:
                    next_classic_attempt_ = 0;
                    // Answered within the call: BlueZ accepted, so the slate is
                    // clean. A call that is merely issued proves nothing yet,
                    // and clearing the backoff and the refusal count for it
                    // would restart both on every attempt whose refusal only
                    // arrives later. The link coming up clears them, above.
                    if (!ops_.classic_connect_outstanding()) {
                        status_.classic_backoff = 0;
                        classic_failures_ = 0;
                    }
                    break;
                case ConnectResult::Busy:
                    // Not a refusal and not an attempt. Standing back is the
                    // whole remedy.
                    status_.classic_backoff = grow_backoff(status_.classic_backoff, ceiling);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(INFO,
                               "bluetooth: BR/EDR connect already running in BlueZ ({}), standing back {}s",
                               err.empty() ? "no answer yet" : err,
                               status_.classic_backoff);
                    break;
                case ConnectResult::Failed:
                    if (is_key_missing(err) && auth_.note_key_missing()) {
                        debug::log(WARN,
                                   "bluetooth: the bond with the iPhone is broken: the iPhone no longer holds the "
                                   "key ({}). Standing back: one {}s window of attempts every {}s from now on. "
                                   "Remove the pairing on both sides and pair again.",
                                   err,
                                   BROKEN_BOND_PROBE_SECONDS,
                                   BROKEN_BOND_RETRY_SECONDS);
                        next_probe_at_ = now + BROKEN_BOND_RETRY_SECONDS;
                        status_.standing_back = true;
                        status_.bond = combine_bond(status_.bond, true, auth_.detail());
                    }
                    ++classic_failures_;
                    status_.classic_backoff = next_backoff(status_.classic_backoff, err, ceiling);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(
                        WARN, "bluetooth: BR/EDR connect failed ({}), retrying in {}s", err, status_.classic_backoff);
                    break;
                }
            }

            status_.le_dialling = ops_.le_connect_outstanding();
            unpin_bearer();

            // An iPhone that keeps refusing while unlocked, with its Bluetooth
            // permissions already on, is wedged on its own side: nothing here
            // clears it, and saying "connecting" forever hides that. A host with
            // no connectable BR/EDR profile refuses identically, so an OBEX
            // session the phone is serving right now rules the phone out: after
            // this many consecutive refusals the transient decline is over, and
            // what is left is this side having nothing to connect.
            const bool refused = classic_failures_ >= CLASSIC_FAILURES_BEFORE_ADVICE;
            status_.reason = refused && obex_up ? _(NO_LOCAL_PROFILE_ADVICE)
                             : refused && !status_.le_connected
                                 ? _("The iPhone keeps refusing the Bluetooth connection. Turn Bluetooth off and "
                                     "back on on the iPhone — that clears it even when the permissions are already "
                                     "on. Re-pairing is not the fix.")
                             : status_.le_connected
                                 ? _("Notifications are connected. Still reaching the iPhone for messages and "
                                     "contacts...")
                                 : _("Connecting to the iPhone over Bluetooth...");
            return;
        }

        // Classic is up.
        if (calls_enabled_ && !ops_.telephony_present()) {
            if (telephony_absent_since_ < 0)
                telephony_absent_since_ = now;
            // First the cheap way: one request for the profile over the link
            // as it is. A link obexd or the phone brought up carries no
            // hands-free because nobody asked BlueZ for it.
            if (hfp_request_enabled_ && !hfp_request_spent_ && !hfp_cycle_spent_ &&
                HFP_REQUEST_SECONDS < hfp_absent_seconds_ && now - telephony_absent_since_ >= HFP_REQUEST_SECONDS) {
                hfp_request_spent_ = true;
                if (ops_.request_hands_free())
                    debug::log(INFO,
                               "bluetooth: BR/EDR up for {}s without hands-free; asking BlueZ for the profile over "
                               "the live link",
                               now - telephony_absent_since_);
            }
            if (!hfp_cycle_spent_ && now - telephony_absent_since_ >= hfp_absent_seconds_) {
                hfp_cycle_spent_ = true;
                ++hfp_cycles_;
                // Said out loud: without this line the drop that follows reads
                // as a fault of the link rather than a decision made here.
                debug::log(INFO,
                           "bluetooth: BR/EDR up for {}s without hands-free; dropping the BR/EDR link on purpose, "
                           "once, so the reconnect brings call control",
                           now - telephony_absent_since_);
                std::string err;
                if (!ops_.disconnect_classic(err))
                    debug::log(WARN, "bluetooth: BR/EDR cycle for hands-free failed ({})", err);
            }
        } else {
            telephony_absent_since_ = -1;
            hfp_cycle_spent_ = false;
            hfp_request_spent_ = false;
        }

        // LE is only worth attempting once the ACL has settled.
        if (!ancs_enabled_) {
            status_.reason = _("Connected. Notification mirroring is disabled.");
            return;
        }

        if (!status_.le_available) {
            // No populated Bearer.LE1: the bond has no LE half, whatever the cause.
            status_.reason = tr_format(_("Connected, but this pairing has no LE keys, so notifications are "
                                         "unavailable. This is usually the Bluetooth adapter. Run landlinectl --bt-status "
                                         "or see {}"),
                                       ADAPTER_DOC_URL);
            return;
        }

        if (!status_.le_connected) {
            // solicitation owns the radio; the dial gets one shot per daemon and then stands aside.
            const bool settled =
                classic_connected_since_ >= 0 && now - classic_connected_since_ >= BEARER_SETTLE_SECONDS;

            const bool pinned = ops_.preferred_bearer() == "bredr";
            if (pinned && settled && !bearer_handed_back_ && !ops_.le_connect_outstanding()) {
                ops_.set_preferred_bearer("le");
                bearer_handed_back_ = true;
                debug::log(INFO, "bluetooth: bond was pinned to BR/EDR; preference handed back to LE");
            }

            // BR/EDR up, the advert on air, and still no LE link: the phone
            // is here and not answering it. Every link is dropped once, which
            // is the one situation it has been seen to answer from.
            {
                RecycleFacts recycle;
                recycle.le = le_expectation();
                recycle.le_connected = status_.le_connected;
                recycle.classic_connected = status_.classic_connected;
                recycle.connect_outstanding = ops_.le_connect_outstanding() || ops_.classic_connect_outstanding();
                recycle.phone_busy = ops_.phone_busy();
                recycle.now = now;
                recycle.classic_connected_since = classic_connected_since_;
                recycle.advert_on_air_since = advert_on_air_since_;
                recycle.hold_expired_at = hold_expired_at_;
                recycle.last_recycle_at = last_recycle_at_;
                recycle.spent = le_recycle_spent_;
                if (recycle_for_le(le_first_, recycle) == RecycleVerdict::Recycle) {
                    le_recycle_spent_ = true;
                    last_recycle_at_ = now;
                    ++le_recycles_;
                    debug::log(INFO,
                               "bluetooth: BR/EDR up for {}s with the solicitation on air and no LE link; dropping "
                               "every link on purpose, once, so that the iPhone comes back over LE first",
                               now - classic_connected_since_);
                    std::string err;
                    if (!ops_.disconnect_all(err))
                        debug::log(WARN, "bluetooth: could not drop the links for LE ({})", err);
                    status_.le_dialling = ops_.le_connect_outstanding();
                    status_.reason = _("Reconnecting so that the iPhone opens the link that carries notifications...");
                    return;
                }
            }

            if (!le_dial_spent_ && le_dial_due(le_first_, settled, now, advert_on_air_since_) &&
                !ops_.le_connect_outstanding()) {
                std::string err;
                const ConnectResult result = classify(ops_.connect_le(err), err);
                le_dial_spent_ = true;
                if (result == ConnectResult::Failed)
                    debug::log(WARN, "bluetooth: LE connect failed ({}), leaving it to the solicitation", err);
            }

            // read after the attempt, never before
            status_.le_dialling = ops_.le_connect_outstanding();

            const bool le_silent = le_down_since_ >= 0 && now - le_down_since_ >= LE_SILENT_SECONDS;

            status_.reason = le_silent && !solicited_since_le_down_ ? _(LE_NEVER_SOLICITED_ADVICE)
                             : le_silent && pinned                  ? _(LE_BOND_PINNED_ADVICE)
                             : le_silent ? _(LE_PHONE_SILENT_ADVICE)
                                         : _("Connected. Waiting for the iPhone to open the LE link that carries "
                                             "notifications. If it does not, open Settings > Bluetooth > (i) on the "
                                             "iPhone and check Share System Notifications.");
            return;
        }

        status_.reason = _("Connected over BR/EDR and LE.");
        return;
    }

} // namespace tether::bluetooth
