#pragma once

#include "tether/bluetooth/advert.hpp"
#include "tether/bluetooth/bond_health.hpp"
#include "tether/bluetooth/config.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace tether::bluetooth {

    class BluezMonitor;

    struct PairResult {
        bool success = false;
        // Machine-readable outcome: "paired", "already_paired", "unpaired",
        // "stale_bond", "not_found", "rejected", "timeout", "busy", "error".
        // Published a second time as `reason` when the transaction failed.
        std::string status;
        std::string message;
        std::string device_path;
        std::string device_address;
        // True when the resulting bond covers LE as well as BR/EDR.
        bool dual_bond = false;
        // Which transaction actually produced the bond, which is not always the
        // one that was asked for: connect-first falls back to an explicit pair.
        AuthStrategy auth_strategy_used = AuthStrategy::ConnectFirst;
        // The bond as the transaction left it.
        BondHealth bond;
        // unpair only: BlueZ dropped the device, and the selection went with it.
        bool removed = false;
        bool selection_cleared = false;
    };

    // What a bt_pair request asks for beyond the address, and what the caller
    // knows that BlueZ's object tree does not show.
    struct PairOptions {
        // Remove whatever bond exists first, then pair from nothing.
        bool fresh = false;
        // The supervisor has seen this bond refuse to authenticate.
        bool known_broken = false;
        std::string broken_detail;
        // Whether this machine can carry the LE half: see assess_bond().
        bool le_expected = true;
    };

    // What bt_pair does about a bond that is already there.
    enum class ExistingBond {
        Reuse,   // already_paired
        Refuse,  // stale_bond: say so and touch nothing
        Replace, // fresh: remove it and pair again
    };

    inline ExistingBond plan_existing_bond(const BondHealth& health, bool fresh) {
        if (health.state == BondState::None)
            return ExistingBond::Reuse;
        if (fresh)
            return ExistingBond::Replace;
        return bond_reusable(health) ? ExistingBond::Reuse : ExistingBond::Refuse;
    }

    // What bt_unpair reports once BlueZ has answered. `was_selected` is whether
    // the address is the iPhone the daemon supervises: a device BlueZ no longer
    // knows is still worth an unpair then, to drop the selection left behind.
    PairResult finish_unpair(PairResult removal, bool was_selected);

    // Whether two Bluetooth addresses name the same device, whatever their case.
    bool same_address(const std::string& a, const std::string& b);

    // Whether a failed connect-first transaction should be retried as an explicit
    // Device1.Pair(). Connect-first induces authentication only as a side effect of
    // a profile connect, so a phone that refuses the profile never starts pairing
    // at all. Device1.Pair() involves no profile.
    bool should_fall_back(AuthStrategy tried, bool paired, bool confirmation_failed);

    // Whether a finished tether-dialog answered one way or another.
    bool dialog_answered(int wait_status);

    // Whether a D-Bus error names an authentication BlueZ already failed. No bond can appear afterwards.
    bool is_authentication_failure(const std::string& err);

    // Reports progress steps so the CLI and GTK app can show what is happening
    // during a transaction that legitimately takes tens of seconds.
    using ProgressFn = std::function<void(const std::string& step, const std::string& detail)>;

    // Asks the user to confirm a six-digit numeric comparison code. Returns true
    // to accept. Runs on the monitor's GLib thread.
    using ConfirmFn = std::function<bool(const std::string& code)>;

    // Runs the full pairing transaction against `address`, blocking until it
    // resolves. Intended to be called from a worker thread: GDBusConnection is
    // thread-safe for calls, while the agent's callbacks land on the monitor's
    // GLib thread.
    // `calls_enabled` widens the agent's service whitelist to hands-free.
    // Shows the daemon's own confirmation window. Sets `unavailable` when it
    // could not be shown, in which case the return value means nothing.
    using DialogFn = std::function<bool(const std::string& code, bool& unavailable)>;

    struct ConfirmOutcome {
        bool accepted = false;
        // Nobody could be asked.
        bool unavailable = true;
        // "client" or "dialog"; empty when unavailable.
        std::string answered_by;
    };

    // Who confirms a pairing code. Without a dialog the subscribed client is
    // asked directly; with one, the dialog goes first and the client only
    // stands in when it cannot be shown.
    ConfirmOutcome resolve_confirmation(const std::string& code, const ConfirmFn& client, const DialogFn& dialog);

    // `use_dialog` brings back the daemon's own tether-dialog window ahead of
    // the client. Off, the code only goes to `confirm`.
    PairResult pair_device(BluezMonitor& monitor,
                           const std::string& address,
                           AuthStrategy strategy,
                           const ProgressFn& progress,
                           const ConfirmFn& confirm,
                           bool calls_enabled = false,
                           bool use_dialog = false,
                           const PairOptions& options = {});

    // Removes the bond so a clean pairing can be retried.
    PairResult unpair_device(BluezMonitor& monitor, const std::string& address);

    // Runs a BlueZ discovery for `seconds`, calling `on_tick` about once a second
    // so a caller can publish the growing device list.
    bool scan_devices(BluezMonitor& monitor, int seconds, const std::function<void()>& on_tick, std::string& err);

    // Device1.Connect / Device1.Disconnect on a bonded device, by address.
    bool connect_device(BluezMonitor& monitor, const std::string& address, std::string& err);
    bool disconnect_device(BluezMonitor& monitor, const std::string& address, std::string& err);

    // Puts the ANCS solicitation advertisement back on air without re-pairing.
    // This is what makes iOS reveal its "Show Message Notifications" and
    // "Sync Contacts" toggles, and it expires a few minutes after pairing, so
    // there has to be a way back to it short of removing the bond.
    bool solicit_ancs(BluezMonitor& monitor, std::string& err);

    // Whether that advertisement is on air right now
    bool ancs_solicitation_active();

    // Takes it off air, freeing the LE advertising instance
    void stop_ancs_solicitation(BluezMonitor& monitor);

    // bluetoothd was restarted: the advertisement this process believes to be
    // on air is unknown to the new one. Drops that belief, along with any hold
    // and any wait before the next attempt, so that
    // supervise_ancs_solicitation() registers it again on its next call.
    void forget_ancs_solicitation(BluezMonitor& monitor);

    // Holds the solicitation on air for exactly as long as `want`
    // and at the advertising interval asked for: a change of interval registers
    // the advert again.
    void supervise_ancs_solicitation(BluezMonitor& monitor,
                                     bool want,
                                     const AdvertInterval& interval = ADVERT_INTERVAL_STEADY);

    nlohmann::json to_json(const PairResult& result);

    bool confirm_with_dialog(const std::string& device_name, const std::string& code, bool& unavailable);

} // namespace tether::bluetooth
