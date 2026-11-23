#pragma once

#include "tether/bluetooth/ancs/protocol.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>

// When the inbox is listed. It used to be every fifteen seconds for as long
// as a MAP session was open, the one thing this engine asked of the phone
// without end. A Messages notification now says when there is something to
// fetch, and the clock is only a safety net behind it. Pure: the supervisor
// feeds the facts and does what the plan says.
namespace tether::bluetooth {

    // How long after the notification the inbox is listed: the phone files the
    // message a moment later, and a burst is fetched in one listing.
    inline constexpr int MESSAGE_SYNC_DEBOUNCE_SECONDS = 2;
    // A listing that brought nothing new is made again this long after the
    // previous one, twice, then the request is given up.
    inline constexpr int MESSAGE_SYNC_RETRY_SECONDS[] = {3, 10};
    inline constexpr int MESSAGE_SYNC_RETRIES = 2;
    // The listing made without any request, behind working notifications.
    inline constexpr int MESSAGE_BACKGROUND_POLL_SECONDS = 3600;
    // The same when notifications cannot be relied on: the former poll.
    inline constexpr int MESSAGE_FALLBACK_POLL_SECONDS = 15;
    // A request no session came to serve is forgotten after this long.
    inline constexpr int MESSAGE_SYNC_EXPIRY_SECONDS = 60;

    struct MessageSyncFacts {
        int64_t now = 0;
        // The last listing on this session, negative for none.
        int64_t last_poll_at = -1;
        // When a listing was asked for, negative for no request pending.
        int64_t requested_at = -1;
        // Listings already made for that request.
        int request_attempts = 0;
        // The notification session is up, and it carries text.
        bool ancs_ready = false;
        bool ancs_content = false;
        // The MAP session has just opened and was never listed.
        bool session_new = false;
    };

    struct MessageSyncConfig {
        // LANDLINE_MESSAGE_POLL_SECONDS when set: replaces the background
        // interval whatever the notifications do. 0 for unset.
        int background_override = 0;
    };

    enum class MessageSyncPlan { Wait, PollNow };

    // Why a listing is due, for the journal.
    enum class MessageSyncCause { None, Session, Request, Retry, Background };

    inline const char* to_string(MessageSyncCause cause) {
        switch (cause) {
        case MessageSyncCause::Session:
            return "session";
        case MessageSyncCause::Request:
            return "request";
        case MessageSyncCause::Retry:
            return "retry";
        case MessageSyncCause::Background:
            return "background";
        default:
            return "none";
        }
    }

    // LANDLINE_MESSAGE_POLL_SECONDS, 5 to 3600; 0 when unset or unreadable.
    inline int message_poll_override_from(const char* text) {
        if (!text || !*text)
            return 0;
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        return end && !*end && value >= 5 && value <= 3600 ? static_cast<int>(value) : 0;
    }

    inline int message_background_seconds(const MessageSyncFacts& facts, const MessageSyncConfig& config) {
        if (config.background_override > 0)
            return config.background_override;
        return facts.ancs_ready && facts.ancs_content ? MESSAGE_BACKGROUND_POLL_SECONDS
                                                      : MESSAGE_FALLBACK_POLL_SECONDS;
    }

    // Whether a pending request has run out of listings.
    inline bool message_sync_spent(int request_attempts) { return request_attempts > MESSAGE_SYNC_RETRIES; }

    inline MessageSyncCause message_sync_cause(const MessageSyncFacts& facts, const MessageSyncConfig& config = {}) {
        if (facts.session_new)
            return MessageSyncCause::Session;

        if (facts.requested_at >= 0 && !message_sync_spent(facts.request_attempts)) {
            if (facts.request_attempts == 0) {
                if (facts.now - facts.requested_at >= MESSAGE_SYNC_DEBOUNCE_SECONDS)
                    return MessageSyncCause::Request;
            } else if (facts.last_poll_at < 0 ||
                       facts.now - facts.last_poll_at >= MESSAGE_SYNC_RETRY_SECONDS[facts.request_attempts - 1]) {
                return MessageSyncCause::Retry;
            }
        }

        if (facts.last_poll_at < 0 || facts.now - facts.last_poll_at >= message_background_seconds(facts, config))
            return MessageSyncCause::Background;
        return MessageSyncCause::None;
    }

    inline MessageSyncPlan plan_message_sync(const MessageSyncFacts& facts, const MessageSyncConfig& config = {}) {
        return message_sync_cause(facts, config) == MessageSyncCause::None ? MessageSyncPlan::Wait
                                                                           : MessageSyncPlan::PollNow;
    }

    // What one listing does to the request it served. `fresh` is how many
    // messages it brought that were not known; `expect_new` is false for a
    // request that only wants the state brought up to date (a notification
    // removed on the phone), which one listing settles.
    struct MessageSyncRequest {
        int64_t requested_at = -1;
        int attempts = 0;
        bool expect_new = true;
        std::string reason;

        bool pending() const { return requested_at >= 0; }
        void clear() { *this = MessageSyncRequest{}; }

        // A new request. One already waiting for its first listing absorbs it;
        // one being retried starts over, the new message may be another one.
        void ask(int64_t now, const std::string& why, bool expects_new = true) {
            if (pending() && attempts == 0) {
                expect_new = expect_new || expects_new;
                return;
            }
            requested_at = now;
            attempts = 0;
            expect_new = expects_new;
            reason = why;
        }

        // After a listing that succeeded.
        void listed(size_t fresh) {
            if (!pending())
                return;
            ++attempts;
            if (fresh > 0 || !expect_new || message_sync_spent(attempts))
                clear();
        }

        // No session came: the next one lists the folder anyway.
        void expire(int64_t now) {
            if (pending() && attempts == 0 && now - requested_at >= MESSAGE_SYNC_EXPIRY_SECONDS)
                clear();
        }
    };

    // What a notification from the phone asks of the mailbox and of the call
    // history. Fed with every ANCS notification and every removal.
    struct AncsSyncAsk {
        // Why the inbox is to be listed, empty for no listing.
        std::string reason;
        // The call history on the phone has changed.
        bool calls_changed = false;
    };

    class AncsSyncTriggers {
    public:
        AncsSyncAsk on_notification(
            uint32_t uid, const std::string& app_id, ancs::CategoryId category, bool pre_existing, uint64_t session) {
            AncsSyncAsk ask;
            if (category == ancs::CategoryId::IncomingCall || category == ancs::CategoryId::MissedCall)
                ask.calls_changed = !pre_existing;
            if (app_id != ancs::APP_ID_MESSAGES)
                return ask;
            message_uids_.insert(uid);
            if (!pre_existing) {
                ask.reason = "ancs";
            } else if (!backlog_seen_ || session != backlog_session_) {
                // Once per notification session: the backlog says there is
                // something the mailbox has not shown yet.
                backlog_seen_ = true;
                backlog_session_ = session;
                ask.reason = "ancs_backlog";
            }
            return ask;
        }

        // A Messages notification taken off the phone: read there, or deleted.
        AncsSyncAsk on_removed(uint32_t uid) {
            AncsSyncAsk ask;
            if (message_uids_.erase(uid) > 0)
                ask.reason = "ancs_removed";
            return ask;
        }

        void reset() { *this = AncsSyncTriggers{}; }

    private:
        std::set<uint32_t> message_uids_;
        bool backlog_seen_ = false;
        uint64_t backlog_session_ = 0;
    };

} // namespace tether::bluetooth
