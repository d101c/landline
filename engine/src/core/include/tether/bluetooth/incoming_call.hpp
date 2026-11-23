#pragma once

#include "tether/bluetooth/ancs/protocol.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// An incoming call, from the first ring. The notification of the call comes
// over the LE link at once, with an action that answers on the iPhone and one
// that declines; hands-free, which carries the sound to this computer, may
// only come seconds later, on a classic link opened for it. This follows the
// call between the two and says what can be done about it at each moment.
//
// Pure: fed with the notification, its removal and the call list, it returns
// the bt_incoming_call to publish when something changed.
namespace tether::bluetooth {

    // How long "Answer here" may stay "connecting" before it is said
    // unavailable. The lease is kept: hands-free can still come and serve to
    // move the sound after the call was taken on the iPhone.
    inline constexpr int ANSWER_HERE_TIMEOUT_SECONDS = 15;
    // How long the classic link is kept wanted after the ring is over: time
    // for hands-free to show a call taken on the iPhone, or for the call
    // history to be read after a missed one.
    inline constexpr int CALL_AFTER_RING_SECONDS = 20;
    // A ring nobody ended (the LE link dropped under it) is over after this.
    inline constexpr int RING_MAX_SECONDS = 90;

    // LANDLINE_ANCS_CALLS=0: no bt_incoming_call, no link opened at the ring.
    inline bool ancs_calls_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // A call as the hands-free gateway shows it.
    struct CallSight {
        std::string path;
        // incoming, waiting, active, held, dialing, alerting...
        std::string state;
    };

    inline bool call_is_ringing(const CallSight& call) { return call.state == "incoming" || call.state == "waiting"; }
    inline bool call_is_taken(const CallSight& call) { return call.state == "active" || call.state == "held"; }

    struct IncomingCall {
        uint32_t uid = 0;
        // ringing, ended
        std::string state;
        // The notification's title: the caller as the iPhone shows it.
        std::string caller;
        // From the notification's action flags.
        bool can_answer_on_phone = false;
        bool can_decline = false;
        // connecting, ready, unavailable
        std::string answer_here;
        // The call on the gateway, once it shows it.
        std::string path;
        // Unix seconds of the first ring.
        int64_t since = 0;

        bool operator==(const IncomingCall&) const = default;
    };

    inline nlohmann::json to_json(const IncomingCall& call) {
        nlohmann::json j = {{"command", "bt_incoming_call"},
                            {"uid", call.uid},
                            {"state", call.state},
                            {"caller", call.caller},
                            {"can_answer_on_phone", call.can_answer_on_phone},
                            {"can_decline", call.can_decline},
                            {"answer_here", call.answer_here},
                            {"since", call.since}};
        if (!call.path.empty())
            j["path"] = call.path;
        return j;
    }

    class IncomingCallTracker {
    public:
        using Event = std::optional<IncomingCall>;

        // A notification of category IncomingCall starts or updates the ring;
        // one of category MissedCall ends it. Anything else is ignored.
        // `wall` is the time published as `since`.
        Event on_notification(uint32_t uid,
                              ancs::CategoryId category,
                              const std::string& title,
                              bool has_positive,
                              bool has_negative,
                              int64_t now,
                              int64_t wall = 0) {
            if (category == ancs::CategoryId::MissedCall) {
                if (phase_ == Phase::Ringing || (phase_ == Phase::AfterRing && !ended_sent_))
                    return end(now, phase_ == Phase::Ringing ? now + CALL_AFTER_RING_SECONDS : after_until_);
                return std::nullopt;
            }
            if (category != ancs::CategoryId::IncomingCall)
                return std::nullopt;

            if (phase_ == Phase::Ringing && call_.uid == uid) {
                // The same notification, modified.
                IncomingCall next = call_;
                next.caller = title;
                next.can_answer_on_phone = has_positive;
                next.can_decline = has_negative;
                if (next == call_)
                    return std::nullopt;
                call_ = next;
                return call_;
            }

            phase_ = Phase::Ringing;
            ended_sent_ = false;
            declined_ = false;
            ring_started_at_ = now;
            call_ = IncomingCall{};
            call_.uid = uid;
            call_.state = "ringing";
            call_.caller = title;
            call_.can_answer_on_phone = has_positive;
            call_.can_decline = has_negative;
            call_.since = wall > 0 ? wall : now;
            const CallSight* ringing = ringing_call();
            call_.answer_here = ringing ? "ready" : "connecting";
            call_.path = ringing ? ringing->path : std::string{};
            return call_;
        }

        // The notification was taken off the iPhone: answered there, declined,
        // or the caller gave up.
        Event on_removed(uint32_t uid, int64_t now) {
            if (phase_ != Phase::Ringing || call_.uid != uid)
                return std::nullopt;
            if (taken_call()) {
                // Answered: the call list carries it from here, nothing ends.
                phase_ = Phase::None;
                return std::nullopt;
            }
            if (declined_) {
                // Declined from here: nothing more to wait for.
                Event event = end(now, now);
                phase_ = Phase::None;
                return event;
            }
            if (gateway_present_)
                // Hands-free is there and shows no call: it is over.
                return end(now, now + CALL_AFTER_RING_SECONDS);
            // No hands-free yet: it may arrive and show a call taken on the
            // iPhone. Otherwise the ring is said over when the time is up.
            phase_ = Phase::AfterRing;
            ended_sent_ = false;
            after_until_ = now + CALL_AFTER_RING_SECONDS;
            return std::nullopt;
        }

        // The negative action was sent for this notification.
        void on_declined(uint32_t uid) {
            if (phase_ == Phase::Ringing && call_.uid == uid)
                declined_ = true;
        }

        // The call list, as read from the gateway, and whether there is one.
        Event on_calls(const std::vector<CallSight>& calls, bool gateway_present, int64_t now) {
            calls_ = calls;
            gateway_present_ = gateway_present;
            if (phase_ == Phase::AfterRing && !ended_sent_ && taken_call()) {
                // Taken on the iPhone, and hands-free now shows it: no "ended".
                phase_ = Phase::None;
                return std::nullopt;
            }
            if (phase_ != Phase::Ringing)
                return std::nullopt;
            const CallSight* ringing = ringing_call();
            if (!ringing)
                return std::nullopt;
            if (call_.answer_here == "ready" && call_.path == ringing->path)
                return std::nullopt;
            call_.answer_here = "ready";
            call_.path = ringing->path;
            (void)now;
            return call_;
        }

        Event step(int64_t now) {
            if (phase_ == Phase::Ringing) {
                if (now - ring_started_at_ >= RING_MAX_SECONDS) {
                    Event event = end(now, now);
                    phase_ = Phase::None;
                    return event;
                }
                if (call_.answer_here == "connecting" && now - ring_started_at_ >= ANSWER_HERE_TIMEOUT_SECONDS) {
                    call_.answer_here = "unavailable";
                    return call_;
                }
                return std::nullopt;
            }
            if (phase_ == Phase::AfterRing && now >= after_until_) {
                Event event = ended_sent_ ? std::nullopt : end(now, now);
                phase_ = Phase::None;
                return event;
            }
            return std::nullopt;
        }

        bool ringing() const { return phase_ == Phase::Ringing; }

        // Whether the classic link is wanted for this call: while it rings,
        // and CALL_AFTER_RING_SECONDS after.
        bool wants_link(int64_t now) const {
            return phase_ == Phase::Ringing || (phase_ == Phase::AfterRing && now < after_until_);
        }

        // Whether anything is being followed, so the call list is worth reading for it.
        bool active() const { return phase_ != Phase::None; }

        // The call that is ringing, for a client that subscribes during the ring.
        std::optional<IncomingCall> current() const {
            return phase_ == Phase::Ringing ? std::optional<IncomingCall>(call_) : std::nullopt;
        }

        void reset() { *this = IncomingCallTracker{}; }

    private:
        enum class Phase { None, Ringing, AfterRing };

        const CallSight* ringing_call() const {
            for (const auto& call : calls_)
                if (call_is_ringing(call))
                    return &call;
            return nullptr;
        }

        bool taken_call() const {
            for (const auto& call : calls_)
                if (call_is_taken(call))
                    return true;
            return false;
        }

        // Publishes "ended" and keeps the link wanted until `until`.
        Event end(int64_t now, int64_t until) {
            call_.state = "ended";
            phase_ = Phase::AfterRing;
            ended_sent_ = true;
            after_until_ = until;
            (void)now;
            return call_;
        }

        Phase phase_ = Phase::None;
        IncomingCall call_;
        int64_t ring_started_at_ = -1;
        int64_t after_until_ = -1;
        bool ended_sent_ = false;
        bool declined_ = false;
        std::vector<CallSight> calls_;
        bool gateway_present_ = false;
    };

    // How a bt_call_action is carried out when a call is ringing.
    enum class CallRoute {
        // Through hands-free, as before.
        Hfp,
        // Through the notification's action, over the LE link.
        Ancs,
        // Cannot be done.
        None,
    };

    struct CallActionPlan {
        CallRoute via = CallRoute::Hfp;
        // The telephony action, for Hfp.
        std::string hfp_action;
        // The notification action, for Ancs.
        ancs::ActionId ancs_action = ancs::ActionId::Positive;
        // Why not, for None.
        std::string error;
    };

    // `action` is answer or decline. `audio` is "here", "phone" or empty.
    // `gateway_sees_call`: hands-free shows the incoming call. `uid_known`:
    // the notification of the call is known, with these action flags.
    inline CallActionPlan plan_call_action(std::string_view action,
                                           std::string_view audio,
                                           bool gateway_sees_call,
                                           bool uid_known,
                                           bool can_positive,
                                           bool can_negative) {
        CallActionPlan plan;
        if (action == "decline") {
            if (gateway_sees_call) {
                plan.hfp_action = "hangup";
                return plan;
            }
            if (uid_known && can_negative) {
                plan.via = CallRoute::Ancs;
                plan.ancs_action = ancs::ActionId::Negative;
                return plan;
            }
            plan.via = CallRoute::None;
            plan.error = "There is no incoming call to decline.";
            return plan;
        }
        // answer
        if (gateway_sees_call) {
            plan.hfp_action = "answer";
            return plan;
        }
        if (audio == "here") {
            plan.via = CallRoute::None;
            plan.error = "This computer is not connected for the call yet.";
            return plan;
        }
        if (uid_known && can_positive) {
            plan.via = CallRoute::Ancs;
            plan.ancs_action = ancs::ActionId::Positive;
            return plan;
        }
        plan.via = CallRoute::None;
        plan.error = "There is no incoming call to answer.";
        return plan;
    }

} // namespace tether::bluetooth
