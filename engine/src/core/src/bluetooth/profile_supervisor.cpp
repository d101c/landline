#include "tether/bluetooth/profile_supervisor.hpp"
#include "tether/bluetooth/objects.hpp"
#include "tether/log.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        std::string lowered(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        bool contains(const std::string& haystack, const char* needle) {
            return haystack.find(needle) != std::string::npos;
        }

    } // namespace

    const char* to_string(ObexError error) {
        switch (error) {
        case ObexError::None:
            return "none";
        case ObexError::Forbidden:
            return "forbidden";
        case ObexError::Busy:
            return "busy";
        case ObexError::NoRecord:
            return "no_record";
        case ObexError::Unavailable:
            return "unavailable";
        case ObexError::NoDaemon:
            return "no_daemon";
        case ObexError::BusClosed:
            return "bus_closed";
        default:
            return "other";
        }
    }

    ObexError classify_obex_error(const std::string& message) {
        if (message.empty())
            return ObexError::None;

        const std::string text = lowered(message);

        // This process's own bus connection: GDBus answers every call on a
        // closed connection with G_IO_ERROR_CLOSED, "The connection is closed".
        if (contains(text, "the connection is closed") || contains(text, "session bus unavailable"))
            return ObexError::BusClosed;

        // Bus-level, not phone-level: obexd is absent so the name has no owner
        // and no .service file to start one.
        if (contains(text, "not activatable") || contains(text, "serviceunknown") ||
            contains(text, "was not provided by any .service files"))
            return ObexError::NoDaemon;

        // permission toggle is off on the phone
        if (contains(text, "forbidden") || contains(text, "0x43"))
            return ObexError::Forbidden;

        if (contains(text, "connection refused") || contains(text, "(111)"))
            return ObexError::Busy;

        if (contains(text, "unable to find service record") || contains(text, "no such service"))
            return ObexError::NoRecord;

        if (contains(text, "host is down") || contains(text, "no route to host") || contains(text, "not connected") ||
            contains(text, "page timeout"))
            return ObexError::Unavailable;

        return ObexError::Other;
    }

    std::string obex_error_advice(ObexError error, const std::string& profile, bool ertm_disabled) {
        const bool map = profile == "map";
        // TRANSLATORS: the feature name Tether shows for this profile.
        const std::string label = map ? _("Messages") : _("Contacts");
        // TRANSLATORS: these two are toggle names in the iPhone's own Settings app.
        // Use Apple's wording for the target language, not a literal translation.
        const std::string toggle = map ? _("Show Message Notifications") : _("Sync Contacts");

        switch (error) {
        case ObexError::Forbidden:
            // TRANSLATORS: {} is an iPhone Settings toggle name.
            return tr_format(_("Enable \"{}\" for this computer in the iPhone's Settings > Bluetooth > (i). "
                               "This is a permission, not a pairing problem."),
                             toggle);
        case ObexError::Busy:
            // TRANSLATORS: {0} is Messages or Contacts, {1} is the profile name (map or pbap).
            return tr_format(_("{0} is unavailable because another computer is using the iPhone's {1} session. "
                               "Disconnect it there, then this will connect on its own."),
                             label,
                             profile);
        case ObexError::NoRecord:
            // obexd reports its own failed ERTM connect as a missing record too
            if (ertm_disabled)
                // TRANSLATORS: {} is Messages or Contacts.
                return tr_format(_("{} cannot connect because the kernel has L2CAP ERTM disabled "
                                   "(bluetooth.disable_ertm). Run landlinectl --bt-setup; nothing is wrong on the iPhone."),
                                 label);
            // obexd reports a missing record whenever its SDP fetch is refused, so the real cause is usually one layer
            // down.
            // TRANSLATORS: {0} is Messages or Contacts, {1} is an iPhone Settings toggle name.
            return tr_format(_("{0} could not be fetched from the iPhone. Usually another computer holds the "
                               "phone's Bluetooth session, or \"{1}\" is off in Settings > Bluetooth > (i). "
                               "Check both; re-pairing is not the fix."),
                             label,
                             toggle);
        case ObexError::Unavailable:
            return tr_format(_("{} is unreachable; waiting for the iPhone to come back."), label);
        case ObexError::NoDaemon:
            return tr_format(_("{} needs BlueZ's OBEX daemon, which is not installed on this computer. Install it "
                               "(bluez-obex on Arch, bluez-obexd on Debian and Ubuntu). Nothing needs changing on "
                               "the iPhone."),
                             label);
        case ObexError::BusClosed:
            // TRANSLATORS: {} is Messages or Contacts.
            return tr_format(_("{} is unavailable because the engine lost this computer's session bus. It "
                               "restarts on its own; nothing needs changing on the iPhone."),
                             label);
        case ObexError::Other:
            return tr_format(_("{} could not be opened."), label);
        default:
            return {};
        }
    }

    ProfileSupervisor::ProfileSupervisor(ProfileOps& ops) : ops_(ops) {}

    ProfileSupervisor::~ProfileSupervisor() { reset(); }

    void ProfileSupervisor::reset() {
        if (!map_path_.empty())
            ops_.remove_session(map_path_);
        if (!pbap_path_.empty())
            ops_.remove_session(pbap_path_);
        map_path_.clear();
        pbap_path_.clear();
        map_retried_ = false;
        pbap_retried_ = false;
        next_attempt_ = 0;
        status_.map_open = false;
        status_.pbap_open = false;
    }

    void ProfileSupervisor::retry_now(int64_t now) {
        next_attempt_ = 0;
        map_next_fast_ = 0;
        pbap_next_fast_ = 0;
        if (absent_backoff_ != 0)
            next_absent_attempt_ = now;
        // Whoever asked is watching: a profile still refused is followed closely again.
        if (forbidden_fast_seconds_ > 0)
            forbidden_since_ = now;
    }

    void ProfileSupervisor::bus_lost() {
        map_path_.clear();
        pbap_path_.clear();
        map_retried_ = false;
        pbap_retried_ = false;
        status_.map_open = false;
        status_.pbap_open = false;
        if (!status_.map_held)
            status_.map_error = ObexError::BusClosed;
        if (!status_.pbap_held)
            status_.pbap_error = ObexError::BusClosed;
        update_reason(last_reachable_);
    }

    void ProfileSupervisor::note_attempt(
        int64_t now, bool opened, ObexError error, ObexError other_error, int64_t& next_fast) {
        next_fast = now + PROFILE_FORBIDDEN_POLL_SECONDS;
        if (forbidden_fast_seconds_ <= 0)
            return;
        if (opened) {
            // A switch has just been turned on: the other one is being looked at.
            if (other_error == ObexError::Forbidden)
                forbidden_since_ = now;
        } else if (error == ObexError::Forbidden && forbidden_since_ < 0) {
            forbidden_since_ = now;
        }
    }

    void ProfileSupervisor::drop_map() {
        if (!map_path_.empty())
            ops_.remove_session(map_path_);
        map_path_.clear();
        map_retried_ = false;
        status_.map_open = false;
        // Reopen on the next tick rather than waiting out the steady poll.
        next_attempt_ = 0;
    }

    void ProfileSupervisor::set_held(bool map, bool pbap, bool by_inactivity) {
        held_by_inactivity_ = by_inactivity;
        if (map != status_.map_held) {
            status_.map_held = map;
            if (map) {
                if (!map_path_.empty())
                    ops_.remove_session(map_path_);
                map_path_.clear();
                map_retried_ = false;
                status_.map_open = false;
                status_.map_error = ObexError::None;
            }
            next_attempt_ = 0;
        }
        if (pbap != status_.pbap_held) {
            status_.pbap_held = pbap;
            if (pbap) {
                if (!pbap_path_.empty())
                    ops_.remove_session(pbap_path_);
                pbap_path_.clear();
                pbap_retried_ = false;
                status_.pbap_open = false;
                status_.pbap_error = ObexError::None;
            }
            next_attempt_ = 0;
        }
    }

    bool ProfileSupervisor::open_profile(
        const std::string& target, std::string& path, ObexError& error, bool& retried, int64_t& logged_at) {
        std::string err;
        // create_session() clears `path` on failure, so the session that may need
        // dropping has to be captured before the call.
        const std::string previous = path;
        path = ops_.create_session(target, err);
        if (!path.empty()) {
            error = ObexError::None;
            retried = false;
            return true;
        }

        const ObexError before = error;
        error = classify_obex_error(err);
        // The close retries of a refused profile would otherwise write this
        // line every two seconds for minutes: the same refusal is written no
        // more often than the steady poll would have.
        const bool repeated = error == ObexError::Forbidden && before == error && logged_at >= 0 &&
                              tick_now_ - logged_at < PROFILE_STEADY_POLL_SECONDS;
        if (!repeated)
            logged_at = tick_now_;
        debug::log(repeated ? TRACE : WARN, "bluetooth: {} session failed ({}): {}", target, to_string(error), err);

        // A stale session from a previous run can present as Forbidden. Drop only
        // this profile's session and try once more, rather than restarting obexd.
        if (error == ObexError::Forbidden && !retried) {
            retried = true;
            ops_.remove_session(previous);
            path = ops_.create_session(target, err);
            if (!path.empty()) {
                error = ObexError::None;
                return true;
            }
            error = classify_obex_error(err);
        }
        return false;
    }

    bool ProfileSupervisor::tick(int64_t now, bool device_ready, bool phone_reachable, bool classic_up) {
        ProfileStatus previous = status_;
        tick_now_ = now;

        if (!device_ready) {
            status_.reason = _("Waiting for the iPhone.");
            return !(status_ == previous);
        }

        const bool map_done = status_.map_open || status_.map_held;
        const bool pbap_done = status_.pbap_open || status_.pbap_held;

        // An open session is a link, whatever BlueZ reports.
        const bool reachable = phone_reachable || status_.map_open || status_.pbap_open;
        bool due = false;
        if (reachable) {
            // Back: open now rather than at the end of a wait that was sized
            // for an absent phone.
            if (absent_backoff_ != 0) {
                absent_backoff_ = 0;
                next_attempt_ = 0;
            }
            due = now >= next_attempt_;
        } else {
            if (absent_backoff_ == 0) {
                absent_backoff_ = PROFILE_STEADY_POLL_SECONDS;
                // The very first attempt of a run is made regardless: the
                // phone may be in range with no link up yet. One that follows
                // a loss waits, it would only fail.
                next_absent_attempt_ = attempted_once_ ? now + absent_backoff_ : now;
            }
            due = now >= next_absent_attempt_;
        }

        // The close retries: a refused profile, over a link that is up, for a
        // while. Each profile has its own date, so neither waits for the other.
        const bool fast = forbidden_fast_seconds_ > 0 && classic_up && reachable && forbidden_since_ >= 0 &&
                          now - forbidden_since_ < forbidden_fast_seconds_;
        const bool try_map =
            !map_done && (due || (fast && status_.map_error == ObexError::Forbidden && now >= map_next_fast_));
        const bool try_pbap =
            !pbap_done && (due || (fast && status_.pbap_error == ObexError::Forbidden && now >= pbap_next_fast_));
        last_reachable_ = reachable;

        if (try_map || try_pbap) {
            attempted_once_ = true;
            if (try_map) {
                status_.map_open = open_profile("map", map_path_, status_.map_error, map_retried_, map_logged_at_);
                note_attempt(now, status_.map_open, status_.map_error, status_.pbap_error, map_next_fast_);
                if (status_.map_open && on_opened_) {
                    update_reason(reachable);
                    on_opened_();
                }
            }
            if (try_pbap) {
                status_.pbap_open = open_profile("pbap", pbap_path_, status_.pbap_error, pbap_retried_, pbap_logged_at_);
                note_attempt(now, status_.pbap_open, status_.pbap_error, status_.map_error, pbap_next_fast_);
                if (status_.pbap_open && on_opened_) {
                    update_reason(reachable);
                    on_opened_();
                }
            }
            if (status_.map_open || status_.pbap_open)
                opened_once_ = true;
            if (due) {
                next_attempt_ = now + (opened_once_ ? PROFILE_STEADY_POLL_SECONDS : PROFILE_INITIAL_POLL_SECONDS);
                if (!reachable && !status_.map_open && !status_.pbap_open) {
                    next_absent_attempt_ = now + absent_backoff_;
                    absent_backoff_ = std::min(absent_backoff_ * 2, PROFILE_ABSENT_POLL_MAX_SECONDS);
                }
            }
        }

        update_reason(reachable);
        return !(status_ == previous);
    }

    void ProfileSupervisor::update_reason(bool reachable) {
        const bool bus_closed =
            (!status_.map_open && !status_.map_held && status_.map_error == ObexError::BusClosed) ||
            (!status_.pbap_open && !status_.pbap_held && status_.pbap_error == ObexError::BusClosed);

        if (status_.map_held && status_.pbap_held) {
            status_.reason = held_by_inactivity_
                                 ? tr_format(_("{} released while this computer is locked or idle."),
                                             _("Messages and contacts"))
                                 : tr_format(_("{} released on this computer."), _("Messages and contacts"));
        } else if (bus_closed) {
            // Ahead of everything that is not the user's own choice: without
            // the bus nothing can be said about the phone.
            status_.reason = obex_error_advice(ObexError::BusClosed, status_.map_held ? "pbap" : "map");
        } else if (status_.map_held || status_.pbap_held) {
            const std::string names = status_.map_held && status_.pbap_held ? _("Messages and contacts")
                                      : status_.map_held                    ? _("Messages")
                                                                            : _("Contacts");
            // TRANSLATORS: {} is Messages, Contacts, or Messages and contacts.
            status_.reason = held_by_inactivity_
                                 ? tr_format(_("{} released while this computer is locked or idle."), names)
                                 : tr_format(_("{} released on this computer."), names);
        } else if (status_.map_open && status_.pbap_open) {
            status_.reason = _("Messages and contacts are connected.");
        } else if (!reachable) {
            status_.reason = obex_error_advice(ObexError::Unavailable, "map");
        } else if (!status_.map_open && status_.map_error != ObexError::None) {
            // MAP is the feature people notice, so its problem leads.
            status_.reason = obex_error_advice(status_.map_error, "map", l2cap_ertm_disabled());
        } else if (!status_.pbap_open && status_.pbap_error != ObexError::None) {
            status_.reason = obex_error_advice(status_.pbap_error, "pbap", l2cap_ertm_disabled());
        } else {
            status_.reason = _("Opening messages and contacts...");
        }
    }

} // namespace tether::bluetooth
