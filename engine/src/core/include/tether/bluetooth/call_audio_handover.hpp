#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Handing the sound of a call in progress back to the iPhone.
//
// PipeWire's hands-free offers no "release the voice link" call: its transport
// has Activate() and a RejectSCO switch that only gates the next link. The
// voice link (SCO) is closed by PipeWire itself when nothing uses the
// transport any more, and the one lever a client has on that is the profile of
// the phone's card: on `off` its nodes are removed, the transport is released
// and the SCO socket closed. The hands-free control link (RFCOMM) is not part
// of the card profile, so the call itself is untouched; an iPhone whose voice
// link closes during a call carries on with its own microphone and speaker.
namespace tether::bluetooth {

    struct CardProfile {
        int index = -1;
        std::string name;
    };

    // The iPhone's card as PipeWire describes it.
    struct PhoneCard {
        int id = -1;
        std::string name;
        // The active profile; index -1 when PipeWire does not say.
        CardProfile active;
        std::vector<CardProfile> profiles;
    };

    // The card of the phone at `address` in the output of `pw-dump`.
    std::optional<PhoneCard> find_phone_card(const nlohmann::json& dump, const std::string& address);

    // The `off` profile of a card.
    std::optional<CardProfile> off_profile(const PhoneCard& card);

    // The profile to put back after `off`: the one that was active, else the
    // phone's usual one (`audio-gateway`), else the first that is not `off`.
    std::optional<CardProfile> voice_profile(const PhoneCard& card, const std::string& previous);

    // The phone's card, read and switched. A seam, so the sequence below is
    // tested with no PipeWire at all.
    class CallAudioCard {
    public:
        virtual ~CallAudioCard() = default;
        virtual std::optional<PhoneCard> read(const std::string& address) = 0;
        virtual bool set_profile(const PhoneCard& card, const CardProfile& profile) = 0;
    };

    // PipeWire's own tools: `pw-dump` to read, `pw-cli set-param` (then
    // `wpctl set-profile`) to switch. Never saved as the user's choice.
    std::unique_ptr<CallAudioCard> make_pipewire_card();

    enum class HandoverOutcome {
        // No voice link was up: nothing to do.
        AlreadyOnPhone,
        // The voice link closed and the card is back on its profile.
        Released,
        // The voice link closed, but came back as soon as the card's profile
        // was restored, so the card is left on `off`: restore it when the
        // call ends, or when the sound is asked back here.
        ReleasedParked,
        // The voice link is still up. The card is back on its profile.
        StillHere,
        // PipeWire shows no card for the phone, or it has no `off` profile,
        // or the profile could not be switched.
        NoLever,
    };

    const char* to_string(HandoverOutcome outcome);

    // True when the sound of the call really left this computer.
    inline bool released(HandoverOutcome outcome) {
        return outcome == HandoverOutcome::AlreadyOnPhone || outcome == HandoverOutcome::Released ||
               outcome == HandoverOutcome::ReleasedParked;
    }

    struct HandoverTiming {
        // How long the voice link gets to close once the card is off.
        int release_ms = 2000;
        // How long it must stay closed once the profile is back.
        int settle_ms = 500;
        int step_ms = 100;
    };

    struct HandoverResult {
        HandoverOutcome outcome = HandoverOutcome::NoLever;
        // The profile the card was on, to put back when `ReleasedParked`.
        std::string parked_profile;
        // Time the voice link took to close; -1 when it did not.
        int released_after_ms = -1;
    };

    // Closes the voice link of the call in progress. `voice_link_up` reads the
    // stack's transport; `sleep_ms` waits. The card never stays on `off`
    // unless that is the only way the link stays closed.
    HandoverResult hand_call_audio_to_phone(CallAudioCard& card,
                                            const std::string& address,
                                            const std::function<bool()>& voice_link_up,
                                            const std::function<void(int)>& sleep_ms,
                                            const HandoverTiming& timing = {});

    // Puts a card left on `off` back on a profile that carries the voice.
    // True when the card is on such a profile afterwards (or already was).
    bool restore_voice_profile(CallAudioCard& card, const std::string& address, const std::string& previous);

} // namespace tether::bluetooth
