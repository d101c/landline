#pragma once

#include "tether/bluetooth/call_audio_handover.hpp"
#include "tether/bluetooth/objects.hpp"

#include <functional>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// Call control over Bluetooth Hands-Free, in the HF role: this machine is the
// car kit and the iPhone is the audio gateway. Two stacks can own that profile.
// BlueZ's own hands-free profile and PipeWire's, so the phone is reached
// through whichever one has it, and only the bus addressing differs.
namespace tether::bluetooth {

    class BluezMonitor;

    // Empty when the input cannot be dialled.
    std::string normalize_dial_string(const std::string& input);

    // A number fit for the journal: everything hidden but the last two digits.
    std::string mask_number(const std::string& number);

    // One journal line for each call that appeared, was answered, changed state
    // or ended between two readings of the call list (arrays of to_json(Call)).
    // Numbers are masked.
    std::vector<std::string> describe_call_changes(const nlohmann::json& previous, const nlohmann::json& current);

    nlohmann::json to_json(const Call& call);
    nlohmann::json to_json(const std::vector<Call>& calls);
    nlohmann::json to_json(const Telephony& gateway);

    struct TelephonyIds {
        // as in the status payload.
        const char* name;
        const char* bus;
        const char* gateway_iface;
        // null if stack has no call audio
        const char* transport_iface;
        const char* call_iface;
        bool dial_uri;
        bool indicators;
    };

    // What a stack's Dial() takes, built from an already normalized number.
    // BlueZ wants a URI, PipeWire formats the bare number into ATD itself, so
    // handing it a URI would put "ATDtel:+15555550123;" on the RFCOMM link.
    std::string dial_argument(const TelephonyIds& ids, const std::string& number);

    struct TelephonySnapshot {
        Telephony gateway;
        std::vector<Call> calls;
        std::string audio_state;
        // Whether the stack refuses the voice link the phone offers. Unset
        // when the stack carries no call audio or does not say.
        std::optional<bool> reject_sco;
    };

    // Keeps the stack's "refuse the voice link" switch on the user's standing
    // choice. Written when the phone's hands-free link appears, when the
    // choice changes, and when the last call ends -- so a choice made for one
    // call (audio_here, audio_phone) lasts for that call and no longer.
    class CallAudioPolicy {
    public:
        // The standing choice: true brings the sound of calls to this computer.
        void set_here(bool here) {
            if (here != here_)
                due_ = true;
            here_ = here;
        }
        bool here() const { return here_; }

        // One reading of the gateway. Returns what to write to the switch
        // now, if anything: true refuses the voice link.
        std::optional<bool> step(const std::string& gateway_path, std::optional<bool> reject_sco, size_t calls) {
            if (gateway_path.empty()) {
                path_.clear();
                had_calls_ = false;
                return std::nullopt;
            }
            if (gateway_path != path_) {
                path_ = gateway_path;
                due_ = true;
            }
            if (had_calls_ && calls == 0)
                due_ = true;
            had_calls_ = calls > 0;
            // Never under a call in progress: it keeps what was chosen for it.
            if (!due_ || calls > 0 || !reject_sco)
                return std::nullopt;
            due_ = false;
            const bool wanted = !here_;
            return *reject_sco == wanted ? std::nullopt : std::optional<bool>(wanted);
        }

    private:
        bool here_ = false;
        bool due_ = true;
        bool had_calls_ = false;
        std::string path_;
    };

    // A stack that can serve the configured phone's calls.
    class TelephonySource {
    public:
        virtual ~TelephonySource() = default;

        virtual TelephonyIds ids() const = 0;
        // Null while the stack is unreachable.
        virtual GDBusConnection* connection() const = 0;
        // An empty gateway path means this stack is not serving the address.
        virtual TelephonySnapshot snapshot() const = 0;

        // One method call on the stack's bus. `args` may be floating and is
        // consumed either way. Overridden by tests, which have no bus.
        virtual bool invoke(const std::string& path,
                            const char* iface,
                            const char* method,
                            GVariant* args,
                            std::string& err);
    };

    // The iPhone's audio gateway. Sources are consulted in order and the first with a gateway wins.
    class TelephonyClient {
    public:
        // `card` is the lever that closes a voice link already up (see
        // call_audio_handover.hpp); without one, a call whose sound is here
        // cannot be handed back to the iPhone.
        TelephonyClient(std::vector<std::unique_ptr<TelephonySource>> sources,
                        std::string address,
                        std::unique_ptr<CallAudioCard> card = nullptr);

        TelephonyClient(const TelephonyClient&) = delete;
        TelephonyClient& operator=(const TelephonyClient&) = delete;

        // Whether the phone has HFP connected and the gateway is usable.
        bool available() const;

        // Whether any stack is serving the phone, including one still bringing
        // the service level connection up. Presence, not readiness.
        bool present() const;

        nlohmann::json status() const;
        nlohmann::json calls() const;

        // The call list, read once per supervisor tick: also brings the
        // stack's voice-link switch in line with the standing choice.
        nlohmann::json poll_calls();

        // The standing choice for the sound of calls. Applied by poll_calls().
        void set_call_audio_here(bool here);

        // The number is normalized here, so callers may pass what the user typed.
        bool dial(const std::string& number, std::string& err);

        // action: answer, hangup, hangup_all, swap, hold_and_answer,
        // release_and_answer, release_and_swap, multiparty, audio_here,
        // audio_phone. `path` is the call for answer and hangup, and is ignored
        // otherwise.
        bool call_action(const std::string& path, const std::string& action, std::string& err);

        bool send_tones(const std::string& tones, std::string& err);

        // How the waits of a hand-over are spent. Tests replace it.
        void set_sleep(std::function<void(int)> sleep_ms);
        void set_handover_timing(const HandoverTiming& timing);

    private:
        // The serving source and its snapshot or null for none.
        std::pair<TelephonySource*, TelephonySnapshot> pick() const;

        bool invoke(TelephonySource& source,
                    const std::string& path,
                    const char* iface,
                    const char* method,
                    GVariant* args,
                    std::string& err);

        // Moves the call audio between this computer and the iPhone. Fails when the
        // stack holding Hands-Free cannot carry audio at all, and when the
        // sound of a call in progress did not actually leave this computer.
        bool route_audio(TelephonySource& source, const TelephonySnapshot& snap, bool to_phone, std::string& err);

        // The profile a card left on `off` goes back to; unset when the card
        // is where PipeWire put it.
        std::optional<std::string> parked() const;
        void restore_parked_card();

        std::vector<std::unique_ptr<TelephonySource>> sources_;
        std::string address_;
        std::mutex policy_mutex_;
        CallAudioPolicy policy_;
        std::unique_ptr<CallAudioCard> card_;
        std::function<void(int)> sleep_ms_;
        HandoverTiming timing_;
        mutable std::mutex parked_mutex_;
        std::optional<std::string> parked_;
    };

    // bluez's hfp on the system bus. No call audio.
    std::unique_ptr<TelephonySource> make_bluez_source(BluezMonitor& monitor, std::string address);

    // pipewire's, on the session bus. Carries the call audio as well.
    std::unique_ptr<TelephonySource> make_pipewire_source(std::string address);

    // Every source this machine could have, in preference order.
    std::unique_ptr<TelephonyClient> make_telephony_client(BluezMonitor& monitor, std::string address);

    // Decodes a GetManagedObjects payload from org.pipewire.Telephony into the
    // gateway matching `address`.
    TelephonySnapshot parse_pipewire_telephony(GVariant* objects, const std::string& address);

    std::vector<Call> parse_pipewire_calls(GVariant* objects, const std::string& gateway_path);

} // namespace tether::bluetooth
