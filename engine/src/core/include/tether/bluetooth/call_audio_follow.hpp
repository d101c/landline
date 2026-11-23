#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Where the sound of one call was asked to go, followed until it is there.
//
// In the hands-free role the voice link (SCO) is opened by the iPhone, never
// from here: it offers one when a call is set up, and PipeWire accepts or
// refuses it (RejectSCO). The one thing this side can do is ask the iPhone to
// offer it (Activate, which sends AT+BCC), and the iPhone answers ERROR to
// that when it has no call. So "the sound here" is a wish that outlives the
// command: the voice link is accepted before the call exists, then watched
// once the call does, and asked for if it has not come.
//
// Pure: the caller feeds what the stack shows and carries out the step.
namespace tether::bluetooth {

    // Where the sound of a call placed from this computer goes when the
    // client names nothing: here, unless LANDLINE_DIAL_AUDIO_DEFAULT says
    // "setting" (the standing choice, as before) or "phone".
    inline bool dial_audio_here(const std::string& asked, bool standing_here, const char* fallback) {
        if (asked == "here")
            return true;
        if (asked == "phone")
            return false;
        const std::string rule = fallback ? fallback : "";
        if (rule == "setting")
            return standing_here;
        if (rule == "phone")
            return false;
        return true;
    }

    // A call whose sound exists: being placed, ringing at the far end, or
    // connected. One that only rings here has no sound to bring yet.
    inline bool call_carries_sound(const std::string& state) {
        return state == "dialing" || state == "alerting" || state == "active" || state == "held";
    }

    inline bool any_call_carries_sound(const std::vector<std::string>& states) {
        for (const auto& state : states)
            if (call_carries_sound(state))
                return true;
        return false;
    }

    struct CallAudioFollowTiming {
        // How long the iPhone gets to offer the voice link by itself once
        // the call carries sound.
        int first_ms = 3000;
        // Between two requests, and after the last before giving up.
        int retry_ms = 3000;
        int attempts = 3;
    };

    enum class CallAudioStep {
        None,
        // Ask the iPhone for the voice link (Activate).
        Activate,
        // The sound of the call is here.
        Arrived,
        // It did not come, whatever was asked.
        Failed,
    };

    class CallAudioFollowUp {
    public:
        explicit CallAudioFollowUp(CallAudioFollowTiming timing = {}) : timing_(timing) {}

        // The wish for the call being placed, taken, or in progress.
        void expect(bool here) {
            wish_ = here;
            carrying_since_ = -1;
            last_attempt_ = -1;
            attempts_ = 0;
            arrived_ = false;
            failed_ = false;
        }

        // The call ended, or never came.
        void clear() {
            wish_.reset();
            arrived_ = false;
            failed_ = false;
        }

        // true: here, false: on the iPhone, unset: nothing was asked.
        std::optional<bool> wish() const { return wish_; }
        bool arrived() const { return arrived_; }
        bool failed() const { return failed_; }

        // One reading. `carrying`: a call that carries sound exists.
        // `audio_state`: the stack's voice transport ("active" when the
        // sound is here).
        CallAudioStep step(bool carrying, const std::string& audio_state, int64_t now_ms) {
            if (!wish_ || !*wish_)
                return CallAudioStep::None;
            if (!carrying) {
                carrying_since_ = -1;
                return CallAudioStep::None;
            }
            if (audio_state == "active") {
                if (arrived_)
                    return CallAudioStep::None;
                arrived_ = true;
                failed_ = false;
                return CallAudioStep::Arrived;
            }
            // Once it has been here, a sound that leaves was moved on
            // purpose (on the iPhone, or by audio_phone): not fought.
            if (arrived_ || failed_)
                return CallAudioStep::None;
            if (carrying_since_ < 0)
                carrying_since_ = now_ms;
            if (attempts_ < timing_.attempts) {
                const int64_t since = last_attempt_ < 0 ? carrying_since_ : last_attempt_;
                const int wait = last_attempt_ < 0 ? timing_.first_ms : timing_.retry_ms;
                if (now_ms - since < wait)
                    return CallAudioStep::None;
                ++attempts_;
                last_attempt_ = now_ms;
                return CallAudioStep::Activate;
            }
            if (now_ms - last_attempt_ < timing_.retry_ms)
                return CallAudioStep::None;
            failed_ = true;
            return CallAudioStep::Failed;
        }

    private:
        CallAudioFollowTiming timing_;
        std::optional<bool> wish_;
        int64_t carrying_since_ = -1;
        int64_t last_attempt_ = -1;
        int attempts_ = 0;
        bool arrived_ = false;
        bool failed_ = false;
    };

} // namespace tether::bluetooth
