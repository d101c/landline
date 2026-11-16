#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tether::bluetooth {

    // Poll fast until the first session opens, then slowly.
    inline constexpr int PROFILE_INITIAL_POLL_SECONDS = 5;
    inline constexpr int PROFILE_STEADY_POLL_SECONDS = 15;

    // A profile the phone refuses with Forbidden is waiting on a switch in the
    // iPhone's Settings, and whoever has just paired is looking at that switch.
    // Over a BR/EDR link that is up the refusal is an immediate OBEX answer, so
    // the refused profile alone is tried again this often, for this long after
    // the refusal was first seen, a session opened, or a client asked. After
    // that the steady poll above is all that is left.
    inline constexpr int PROFILE_FORBIDDEN_POLL_SECONDS = 2;
    inline constexpr int PROFILE_FORBIDDEN_FAST_SECONDS = 300;
    inline constexpr int PROFILE_FORBIDDEN_FAST_SECONDS_MAX = 3600;

    // TETHER_FORBIDDEN_FAST_SECONDS: 0 switches the close retries off, 1 to
    // 3600 sets how long they last.
    inline int forbidden_fast_seconds_from(const char* env_override, int fallback) {
        if (!env_override || !*env_override)
            return fallback;
        int value = 0;
        for (const char* c = env_override; *c; ++c) {
            if (*c < '0' || *c > '9' || value > PROFILE_FORBIDDEN_FAST_SECONDS_MAX)
                return fallback;
            value = value * 10 + (*c - '0');
        }
        return value <= PROFILE_FORBIDDEN_FAST_SECONDS_MAX ? value : fallback;
    }

    // While no link reaches the phone an attempt cannot succeed, and each one
    // holds the supervising thread for the seconds obexd takes to give up. The
    // attempts are spaced out, doubling up to this, and resume at once when a
    // link is back. They do not stop: on a machine where BlueZ never reports
    // the phone connected, an attempt is the only way to find it.
    inline constexpr int PROFILE_ABSENT_POLL_MAX_SECONDS = 120;

    // TETHER_OBEX_ABSENT_BACKOFF=0 keeps the attempts every 15 s regardless.
    inline bool obex_absent_backoff_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // Why an OBEX session could not be opened. Forbidden and Busy look alike from a distance, and reporting one as the
    // other sends the user off re-pairing for nothing.
    enum class ObexError {
        None,
        // OBEX Forbidden / 0x43: the iPhone's permission toggle is off. Not a pairing failure
        Forbidden,
        // Transport-level "Connection refused (111)": another computer probably
        // owns the iPhone's single MAP session. Keep polling; do not re-pair.
        Busy,
        // obexd's live SDP query found no MAS/PSE record. NOT a permissions failure. Recovers on its own once the phone
        // serves them again.
        NoRecord,
        // The phone is not reachable right now.
        Unavailable,
        // org.bluez.obex is not on the session bus and cannot be activated,
        // meaning obexd is not installed. No phone-side setting affects this.
        NoDaemon,
        // This process's connection to the session bus is closed, or there is
        // no session bus: obexd cannot be reached, whatever the phone allows.
        BusClosed,
        Other,
    };

    const char* to_string(ObexError error);

    // Maps an obexd/D-Bus error message onto the cases above. Pure so the
    // classification can be pinned down by tests against real message text.
    ObexError classify_obex_error(const std::string& message);

    // Advice for the user, phrased so it can be shown verbatim. `ertm_disabled`
    // names the local cause of a NoRecord that would otherwise blame the phone.
    std::string obex_error_advice(ObexError error, const std::string& profile, bool ertm_disabled = false);

    // Session operations on obexd.
    class ProfileOps {
    public:
        virtual ~ProfileOps() = default;

        // Returns the new session's object path, or an empty string with err set.
        virtual std::string create_session(const std::string& target, std::string& err) = 0;
        virtual void remove_session(const std::string& path) = 0;
    };

    struct ProfileStatus {
        bool map_open = false;
        bool pbap_open = false;
        ObexError map_error = ObexError::None;
        ObexError pbap_error = ObexError::None;
        // Released on purpose, so another computer can open it.
        bool map_held = false;
        bool pbap_held = false;
        std::string reason;

        bool operator==(const ProfileStatus&) const = default;
    };

    // Keeps exactly one MAP and one PBAP session alive for the daemon's lifetime.
    class ProfileSupervisor {
    public:
        explicit ProfileSupervisor(ProfileOps& ops);

        // Releases both obexd sessions. The iPhone serves one MAP session at a
        // time, so a session abandoned here blocks the next one.
        ~ProfileSupervisor();

        // Drive periodically with a monotonic clock. `device_ready` is a paired
        // device, not a link: obexd opens its own transport. `phone_reachable`
        // is false when BlueZ shows no link to the phone although it could
        // show one; attempts are then spaced out, see PROFILE_ABSENT_POLL_MAX_SECONDS.
        // `classic_up` is the BR/EDR link itself: the close retries of a
        // refused profile are made only over a link that is already there.
        bool tick(int64_t now, bool device_ready, bool phone_reachable = true, bool classic_up = true);

        // How long a refused profile is retried every PROFILE_FORBIDDEN_POLL_SECONDS.
        // 0, the default, leaves every profile on the common poll.
        void set_forbidden_fast_seconds(int seconds) { forbidden_fast_seconds_ = seconds; }

        // Makes the next tick try every closed profile, whatever was scheduled,
        // and starts the close retries of a refused profile over.
        void retry_now(int64_t now);

        // The session bus connection is closed: every session went with it,
        // and nothing can be removed through it.
        void bus_lost();

        // Called from tick() as soon as a session opens, before the other
        // profile is tried: that attempt can wait on obexd for seconds.
        void set_opened_handler(std::function<void()> handler) { on_opened_ = std::move(handler); }

        // Drops both sessions after a disconnect so the next tick reopens them.
        void reset();

        // Drops only the MAP session, for a caller that has found it dead while
        // the link itself is still up. The next tick reopens it.
        void drop_map();

        // A held profile has its session removed and is not reopened until the
        // hold is lifted. `by_inactivity` only changes the reason shown.
        void set_held(bool map, bool pbap, bool by_inactivity);

        const ProfileStatus& status() const { return status_; }
        const std::string& map_session() const { return map_path_; }
        const std::string& pbap_session() const { return pbap_path_; }

    private:
        bool open_profile(
            const std::string& target, std::string& path, ObexError& error, bool& retried, int64_t& logged_at);
        void update_reason(bool reachable);
        // Dates the close retries after an attempt on one profile.
        void note_attempt(int64_t now, bool opened, ObexError error, ObexError other_error, int64_t& next_fast);

        ProfileOps& ops_;
        ProfileStatus status_;
        std::string map_path_;
        std::string pbap_path_;
        bool map_retried_ = false;
        bool pbap_retried_ = false;
        int64_t next_attempt_ = 0;
        bool opened_once_ = false;
        bool attempted_once_ = false;
        // Spacing of the attempts while the phone is unreachable; 0 while it is not.
        int absent_backoff_ = 0;
        int64_t next_absent_attempt_ = 0;
        bool held_by_inactivity_ = false;
        int forbidden_fast_seconds_ = 0;
        // When the close retries began, -1 while no profile has been refused.
        int64_t forbidden_since_ = -1;
        int64_t map_next_fast_ = 0;
        int64_t pbap_next_fast_ = 0;
        bool last_reachable_ = true;
        // The clock of the tick in progress, and when each profile's failure
        // was last written to the journal.
        int64_t tick_now_ = 0;
        int64_t map_logged_at_ = -1;
        int64_t pbap_logged_at_ = -1;
        std::function<void()> on_opened_;
    };

} // namespace tether::bluetooth
