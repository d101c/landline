#pragma once

#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/secret_store.hpp"

#include <map>
#include <string>

namespace tether::bluetooth {

    // How long without input before an idle session releases MAP and PBAP, and its floor.
    inline constexpr int YIELD_IDLE_SECONDS = 300;
    inline constexpr int MIN_YIELD_IDLE_SECONDS = 30;

    // Which pairing transaction to run.
    //
    // ConnectFirst calls Device1.Connect() on the unpaired device and lets iOS
    // initiate authentication. A Linux-initiated Pair() can
    // yield a BR/EDR-only bond, which can never carry ANCS.
    //
    // ExplicitPair calls Device1.Pair() directly. Some controllers cancel the
    // Connect-first transaction, and headless callers cannot show a confirmation
    // prompt, so it stays available as a workaround.
    enum class AuthStrategy { ConnectFirst, ExplicitPair };

    // When taking AirPods out should pause whatever is playing locally.
    enum class PauseMode { Never, OneRemoved, BothRemoved };

    const char* to_string(PauseMode mode);
    PauseMode pause_mode_from_string(const std::string& value);

    // Where the iPhone's media audio plays. Phone keeps it on the iPhone, Here
    // brings it to this computer, Auto leaves it to the iPhone and the audio
    // server -- which, on a stock desktop, means it plays here.
    enum class PhoneAudio { Auto, Here, Phone };

    const char* to_string(PhoneAudio choice);
    PhoneAudio phone_audio_from_string(const std::string& value);

    // Persisted at ~/.config/tether/bluetooth.json.
    struct Config {
        // Address of the selected iPhone, e.g. "02:00:00:00:00:04".
        std::string device_address;
        AuthStrategy auth_strategy = AuthStrategy::ConnectFirst;
        // Whether to mirror notifications. Dual bond turns it on; nothing else writes it but the user
        bool ancs_enabled = true;
        // Whether to request notification contents rather than only the app
        // they came from. The iPhone's own notification-access toggle is the
        // consent gate, so this is on and stays available as an opt-out.
        bool ancs_content_enabled = true;
        // Group replies are off until deliberately enabled
        bool group_messages_enabled = false;
        // Call control over HFP, through whichever stack owns the profile:
        // BlueZ's own hfp, or PipeWire's, which also works with call audio.
        bool calls_enabled = false;
        // When off, supervision runs against no device, so the daemon stops re-dialling
        bool enabled = true;
        // Controller to use, "hci1" or its address. Empty picks the first
        // powered one, which is wrong on any machine with a second controller.
        std::string adapter;
        // How the message journal and contact cache are kept on disk. Encrypted by default.
        Retention retention = Retention::Encrypted;
        // Whether the Wi-Fi side runs: TCP/TLS listener, key pair, mDNS and the
        // LAN-info GATT service. Off leaves only Bluetooth and the local socket.
        bool network_enabled = false;
        // Whether the daemon shows desktop popups itself. Off: the subscribed
        // client is the one that displays notifications.
        bool desktop_popups_enabled = false;
        // Whether a pairing code is confirmed in the daemon's own tether-dialog
        // window. Off: the request goes to the subscribed client.
        bool pair_dialog_enabled = false;
        // Whether popups include the message text. Off shows only the sender.
        bool popup_previews_enabled = true;
        // Whether a local copy is pushed to paired devices. Off shares only on clipboard_send.
        bool clipboard_auto_share = true;
        // Whether threads from senders who are not contacts are hidden. Codes still pop up and autofill.
        bool hide_unknown_senders = false;
        // iPhone apps whose mirrored notifications never pop up, bundle id -> display name.
        std::map<std::string, std::string> muted_apps;
        // Whether Tether manages AirPods at all. Off until asked.
        bool airpods_enabled = false;
        // Whether removing an AirPod pauses local playback.
        PauseMode airpods_pause = PauseMode::Never;
        // Whether an iPhone call hands the AirPods to the phone and takes them back after.
        bool airpods_handoff = false;
        // Whether losing the iPhone to a link supervision timeout locks the session.
        bool lock_on_away = false;
        // How long every bearer must stay down before the session is locked.
        int lock_away_seconds = AWAY_LOCK_GRACE_SECONDS;
        // Whether a locked or idle session releases MAP and PBAP for another computer.
        bool yield_when_inactive = false;
        // Seconds without input before the compositor reports the session idle.
        int yield_idle_seconds = YIELD_IDLE_SECONDS;
        // What performs the lock. Empty uses logind.
        std::string lock_command;
        // Whether the phone coming back after an away lock asks for the login
        // screen to light up. Never an unlock.
        bool wake_on_return = false;
        // Run for that, when set. Empty leaves it to the subscribed client.
        std::string wake_command;
        // Whether contacts list by last name rather than first.
        bool contacts_by_last_name = false;
        // Applied to device_address on every Classic connection. The iPhone
        // takes any computer for a speaker, so the sound stays on the phone
        // until the user asks for it here.
        PhoneAudio phone_audio = PhoneAudio::Phone;
        // Whether a WirePlumber rule for this one phone is written as well, so
        // WirePlumber does not dial the stream itself. Off: nothing is written
        // outside Tether's own configuration.
        bool phone_audio_rule = false;
        // Whether the sound of calls comes to this computer. Off keeps it on
        // the iPhone; answering and hanging up work either way.
        bool call_audio_here = false;

        bool operator==(const Config&) const = default;
    };

    const char* to_string(AuthStrategy strategy);
    AuthStrategy auth_strategy_from_string(const std::string& value);

    std::string config_path();

    Config load_config();
    bool save_config(const Config& config);

    // The address supervision should run against: none while Bluetooth is off.
    std::string supervised_address(const Config& config);

    // exposed for tests without touching fs
    std::string serialize_config(const Config& config);
    Config deserialize_config(const std::string& text);

} // namespace tether::bluetooth
