#pragma once

#include <cstdint>
#include <string>

// Placing a call when the classic link may be closed. The link is opened
// for it, hands-free is waited for, then the number is dialled; each stage is
// published, and the wait can be cancelled until the dialling itself.
//
// Pure: the caller feeds what the links are and does the dialling.
namespace tether::bluetooth {

    // How long the classic link may take to come up.
    inline constexpr int DIAL_LINK_TIMEOUT_SECONDS = 20;
    // How long hands-free may take once the link is up.
    inline constexpr int DIAL_HANDS_FREE_TIMEOUT_SECONDS = 15;
    // How long the link is kept wanted after a dial the phone accepted, for
    // the call to show; the call itself keeps it after that. A call that
    // has not shown by then is reported (stage "no_call"), not passed over.
    inline constexpr int DIAL_CALL_APPEARS_SECONDS = 20;

    enum class DialStage { Connecting, HandsFree, Dialing, Done, Failed, Cancelled };

    // The `stage` of bt_call_progress, for the three stages that have one.
    inline const char* to_string(DialStage stage) {
        switch (stage) {
        case DialStage::Connecting:
            return "connecting";
        case DialStage::HandsFree:
            return "hands_free";
        case DialStage::Dialing:
            return "dialing";
        case DialStage::Done:
            return "done";
        case DialStage::Cancelled:
            return "cancelled";
        default:
            return "failed";
        }
    }

    // Stages of bt_call_progress published after the dial was accepted.
    // The phone took the number and no call has shown.
    inline constexpr const char* DIAL_STAGE_NO_CALL = "no_call";
    // The sound of the call is on this computer, as asked.
    inline constexpr const char* CALL_STAGE_AUDIO_HERE = "audio_here";
    // It was wanted here and did not come.
    inline constexpr const char* CALL_STAGE_AUDIO_FAILED = "audio_failed";

    // Whether something about a call wants the supervisor's whole attention:
    // one being placed, one ringing, one in progress. The long OBEX reads
    // wait, no OBEX session is opened, and the calls are read more often.
    inline bool call_busy(bool dialing, bool ringing, bool call_listed) { return dialing || ringing || call_listed; }

    // LANDLINE_OBEX_DURING_CALLS=1: the address book, the call history and
    // the mailbox are read during a call as they used to be.
    inline bool obex_waits_for_calls(const char* value) { return !value || std::string(value) != "1"; }

    inline bool dial_stage_published(DialStage stage) {
        return stage == DialStage::Connecting || stage == DialStage::HandsFree || stage == DialStage::Dialing;
    }

    struct DialFacts {
        bool calls_enabled = true;
        // The BR/EDR link is up.
        bool link_up = false;
        // The hands-free gateway answers.
        bool hands_free_ready = false;
    };

    // Why a dial is turned away before it starts: "calls_off", "busy", or
    // null when it may start. One operation at a time.
    inline const char* dial_refusal(bool calls_enabled, bool another_running) {
        if (!calls_enabled)
            return "calls_off";
        if (another_running)
            return "busy";
        return nullptr;
    }

    class DialOperation {
    public:
        explicit DialOperation(int64_t now) : started_at_(now) {}

        struct Step {
            DialStage stage = DialStage::Connecting;
            // The stage is new: publish it.
            bool changed = false;
        };

        // One look at the links. On Dialing the caller dials and reports
        // with dialed().
        Step step(const DialFacts& facts, int64_t now) {
            const DialStage before = stage_;
            const bool first = !stepped_;
            stepped_ = true;
            if (terminal() || stage_ == DialStage::Dialing)
                return {stage_, false};

            if (!facts.calls_enabled) {
                fail("calls_off");
            } else if (cancel_requested_) {
                stage_ = DialStage::Cancelled;
                reason_ = "cancelled";
            } else if (facts.hands_free_ready) {
                stage_ = DialStage::Dialing;
            } else if (stage_ == DialStage::Connecting) {
                if (facts.link_up) {
                    stage_ = DialStage::HandsFree;
                    hands_free_since_ = now;
                } else if (now - started_at_ >= DIAL_LINK_TIMEOUT_SECONDS) {
                    fail("no_link");
                }
            } else if (stage_ == DialStage::HandsFree) {
                if (now - hands_free_since_ >= DIAL_HANDS_FREE_TIMEOUT_SECONDS)
                    fail("no_hands_free");
            }
            return {stage_, first || stage_ != before};
        }

        // Without effect once the dialling has begun: hanging up is then a
        // call action.
        bool cancel() {
            if (terminal() || stage_ == DialStage::Dialing)
                return false;
            cancel_requested_ = true;
            return true;
        }

        // What the phone answered to the dial.
        void dialed(bool accepted) {
            if (stage_ != DialStage::Dialing)
                return;
            if (accepted)
                stage_ = DialStage::Done;
            else
                fail("refused");
        }

        DialStage stage() const { return stage_; }
        bool terminal() const {
            return stage_ == DialStage::Done || stage_ == DialStage::Failed || stage_ == DialStage::Cancelled;
        }
        // no_link, no_hands_free, refused, cancelled, calls_off; empty otherwise.
        const std::string& reason() const { return reason_; }
        // Whether the classic link is still wanted for the operation itself.
        bool wants_link() const { return !terminal(); }

    private:
        void fail(const char* reason) {
            stage_ = DialStage::Failed;
            reason_ = reason;
        }

        DialStage stage_ = DialStage::Connecting;
        std::string reason_;
        int64_t started_at_ = 0;
        int64_t hands_free_since_ = -1;
        bool cancel_requested_ = false;
        bool stepped_ = false;
    };

} // namespace tether::bluetooth
