#pragma once

#include "tether/bluetooth/ancs/notifications.hpp"
#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/bluetooth/classic_demand.hpp"
#include "tether/bluetooth/groups.hpp"
#include "tether/bluetooth/link_mode.hpp"
#include "tether/bluetooth/messages.hpp"
#include "tether/bluetooth/profile_supervisor.hpp"
#include "tether/bluetooth/telephony.hpp"
#include "tether/secret_store.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tether::bluetooth {

    class BluezMonitor;
    class MapSession;

    // How long a message waits for the classic link to be opened for it.
    inline constexpr int SEND_LINK_TIMEOUT_SECONDS = 20;

    // "connecting" while the link is being opened for a message, then
    // "sending": the stages of bt_send_progress.
    using SendProgressFn = std::function<void(const char* stage)>;

    struct ConnectionState;

    // Drives the bearer and profile supervisors against the real BlueZ and obexd,
    // on its own thread, for the daemon's lifetime.
    class ConnectionManager {
    public:
        // Reports a combined status whenever it changes.
        using StatusFn = std::function<void(const nlohmann::json&)>;
        // Called once per newly seen message. `backfill` marks the first listing
        // after a MAP session opens (phone's existing inbox), so they belong in the UI but must not each do desktop
        // notification.
        using MessageFn = std::function<void(const Message& message, bool backfill)>;
        // Called once per mirrored notification, and when one is dismissed on
        // the phone.
        using NotificationFn = std::function<void(const ancs::Notification&)>;
        using WithdrawFn = std::function<void(uint32_t uid)>;
        // Called whenever the call list or the telephony service's availability changes.
        using CallsFn = std::function<void(const nlohmann::json& calls)>;

        ConnectionManager(BluezMonitor& monitor, StatusFn on_change, MessageFn on_message = {});

        // Enables notification mirroring. Without a handler the ANCS client is
        // never started, so a caller that cannot show notifications does not pay
        // for a GATT session it will not use.
        void set_notification_handlers(NotificationFn on_notification, WithdrawFn on_withdraw);

        // Turns notification mirroring on or off in place, on the next supervisor tick.
        void set_ancs_enabled(bool enabled);

        // Whether mirrored notifications carry their title and body, or only
        // which app sent them. Takes effect on the next notification.
        void set_ancs_content_enabled(bool enabled);

        // Whether the session locks when the phone drops out of range, and
        // whether its return then asks for the login screen to light up.
        void set_lock_on_away(bool enabled, int grace_seconds, bool wake_on_return = false);

        // Events besides the status, each a JSON object with its own "command":
        // bt_presence (the phone left or came back) and bt_battery_low.
        using EventFn = std::function<void(const nlohmann::json& event)>;
        void set_event_handler(EventFn on_event);

        // The last bt_presence published; state "unknown" before the first.
        nlohmann::json presence() const;

        // Releases or reclaims a profile on this computer: map, pbap, ancs, or all.
        // Reclaiming retries; it cannot take MAP from another computer holding it.
        // False for an unknown profile.
        bool set_hold(const std::string& profile, bool held);

        // Whether a locked or idle session releases MAP and PBAP for another computer.
        void set_yield_when_inactive(bool enabled);

        // Tries the closed profiles and the notification service now instead of
        // at their next date, and follows a refused profile closely again. For
        // a client whose user is looking at the iPhone's switches. False when
        // no phone is supervised.
        bool retry_profiles();

        // Asks for the inbox to be listed soon instead of at its next date:
        // a Messages notification arrived ("ancs"), one was removed
        // ("ancs_removed"), the notification session came up with some
        // already there ("ancs_backlog"), a message was sent ("sent"). A
        // request already waiting absorbs it. Any thread.
        void request_message_sync(std::string_view reason);

        // The phone's call history has changed: read again when PBAP next allows.
        void mark_call_history_stale();

        // The link mode as set (bt_set_link_mode). What is applied can
        // differ: see link_mode.hpp and the published link_mode_effective.
        void set_link_mode(LinkMode mode);
        // Whether the lean mode is the one applied: the classic link is then
        // closed at rest and opened by leases.
        bool on_demand() const;
        // A lease on the classic link, for the engine's own needs. Recorded in
        // either mode, acted on in the lean one. False when refused.
        bool take_lease(Lease lease, bool urgent = false);
        void release_lease(Lease lease);
        // bt_lease: "reading" or "setup", held for CLIENT_LEASE_SECONDS unless
        // renewed. False for another name, or with no phone supervised.
        bool client_lease(const std::string& name, bool held);
        // Whether the phone's sound is wanted on this computer (the standing
        // choice). True in the lean mode, where ask_phone_audio() opens a link.
        bool set_phone_audio_here(bool here);
        // The user turned the sound on here, or asked for it again: in the
        // lean mode the classic link is opened and offered to the iPhone for
        // PHONE_AUDIO_OFFER_SECONDS, then kept while the sound plays (#43).
        void ask_phone_audio();
        // Waits for the MAP session a Send lease is opening. Null with `err`
        // set when it did not come in time or the iPhone refuses it.
        std::shared_ptr<MapSession> await_messages(int seconds, std::string& err);

        // Invokes a mirrored notification's action.
        bool perform_notification_action(uint32_t uid, ancs::ActionId action);

        // Enables call control.
        void set_call_handler(CallsFn on_calls);
        void set_calls_enabled(bool enabled);
        // Whether the sound of calls comes to this computer by default. A
        // call in progress keeps its route; see CallAudioPolicy.
        void set_call_audio_here(bool here);

        // Publishes the call list now if it changed, instead of on the next supervisor tick.
        void refresh_calls();

        bool dial(const std::string& number, std::string& err);
        // The same on its own thread, opening the classic link and waiting
        // for hands-free when the lean link mode has it closed. `progress`
        // is told each stage ("connecting", "hands_free", "dialing"), `done`
        // the outcome with a reason on failure: no_link, no_hands_free,
        // refused, cancelled, busy, calls_off. One at a time.
        using DialProgressFn = std::function<void(const char* stage)>;
        using DialResultFn =
            std::function<void(bool success, const std::string& reason, const std::string& message)>;
        void dial_async(const std::string& number,
                        const std::string& audio,
                        const std::string& operation_id,
                        DialProgressFn progress,
                        DialResultFn done);
        // Stops the dial in progress, or the one named when an id is given.
        // False when there is none, and without effect once it is dialling.
        bool cancel_dial(const std::string& operation_id);
        bool call_action(const std::string& path, const std::string& action, std::string& err);

        // What bt_call_action carries besides the action and the path.
        struct CallActionOptions {
            // The notification of the call (bt_incoming_call.uid).
            std::optional<uint32_t> uid;
            // "here", "phone", or empty: where the sound of an answered call goes.
            std::string audio;
        };
        // The same, for a call that may still be ringing without hands-free:
        // `answer` on the iPhone and `decline` then go through the action of
        // the call's notification. `via` is "hfp" or "ancs", whichever was
        // used. With no ring being followed this is the call above.
        bool call_action(const std::string& path,
                         const std::string& action,
                         const CallActionOptions& options,
                         std::string& err,
                         std::string& via);
        // The bt_incoming_call of the call that is ringing, or null.
        nlohmann::json incoming_call() const;
        bool call_tones(const std::string& tones, std::string& err);
        nlohmann::json calls() const;
        // Whether the phone has Hands-Free up.
        bool calls_available() const;
        // A call is being placed, is ringing or is in progress, and what can
        // wait for it does (false with LANDLINE_OBEX_DURING_CALLS=1).
        bool call_in_hand() const;

        // The phone's recent calls, newest first: number, name, type
        void set_call_history_handler(CallsFn on_history);
        nlohmann::json call_history() const;

        nlohmann::json notifications(size_t limit = 50) const;
        ~ConnectionManager();

        ConnectionManager(const ConnectionManager&) = delete;
        ConnectionManager& operator=(const ConnectionManager&) = delete;

        // address is the bonded iPhone; empty disables supervision until set.
        bool start(const std::string& address, bool ancs_enabled);
        void stop();

        // `ancs_enabled` is the user's preference. Whether the controller can
        // actually carry ANCS is decided here rather than by the caller.
        void set_device(const std::string& address, bool ancs_enabled);

        nlohmann::json status() const;

    private:
        bool start_locked(const std::string& address, bool ancs_enabled);
        void stop_locked();

        std::unique_ptr<ConnectionState> state_;
    };

    nlohmann::json to_json(const BearerStatus& bearer, const ProfileStatus& profiles);

    // Conversation history for the selected iPhone.
    MessageStore& message_store();
    std::mutex& message_store_mutex();

    // Marks a message as read locally and on the phone (when MAP is up)
    // Returns false only when the handle is unknown.
    bool mark_message_read(const std::string& handle, bool read, std::string& err_out, bool* synced_out = nullptr);

    // Marks a batch read off the calling thread.
    void mark_messages_read_async(std::vector<std::string> handles, bool read);

    // Sends a reply to a one-to-one conversation. Returns false with err_out set
    // when MAP is down, the thread cannot be replied to, or the phone refused.
    // In the lean link mode the classic link is opened for it first, for up
    // to SEND_LINK_TIMEOUT_SECONDS; `progress` is told each stage.
    bool send_message(const std::string& thread_key,
                      const std::string& body,
                      Message& sent_out,
                      std::string& err_out,
                      const SendProgressFn& progress = {});

    // Feeds the group correlator with an Apple Messages notification.
    void observe_message_notification(const std::string& sender,
                                      const std::string& subtitle,
                                      const std::string& body,
                                      int64_t now);

    // Why a group thread can or cannot be replied to, for the UI to show.
    ReplyEligibility group_reply_status(const std::string& thread_key, std::string& reason);

    // Closes the message journal and drops the in-memory history.
    void discard_retained_messages();

    // Follows a retention mode change on disk.
    void move_retained_store(Retention previous, Retention next);

    void set_group_replies_enabled(bool enabled);
    void reload_group_rosters();

    extern ConnectionManager* g_bt_connections;

} // namespace tether::bluetooth
