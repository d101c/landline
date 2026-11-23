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
        case ObexError::DaemonDown:
            return "daemon_down";
        default:
            return "other";
        }
    }

    const char* to_string(ObexDaemonState state) {
        switch (state) {
        case ObexDaemonState::Alive:
            return "alive";
        case ObexDaemonState::Absent:
            return "absent";
        case ObexDaemonState::Silent:
            return "silent";
        default:
            return "unknown";
        }
    }

    ObexDaemonState classify_obex_ping(const std::string& message) {
        if (message.empty())
            return ObexDaemonState::Alive;
        const std::string text = lowered(message);
        // dbus-broker: "NameHasNoOwner: Destination does not exist"; dbus-daemon
        // says the same error name with other words.
        if (contains(text, "namehasnoowner") || contains(text, "serviceunknown") ||
            contains(text, "destination does not exist"))
            return ObexDaemonState::Absent;
        // GLib's own timeout, or the bus giving up on the owner.
        if (contains(text, "timeout was reached") || contains(text, "error.noreply") ||
            contains(text, "error.timeout"))
            return ObexDaemonState::Silent;
        return ObexDaemonState::Unknown;
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

        // GLib gave up waiting (CONNECT_TIMEOUT_MS): obexd did not answer at all.
        // The phone's own failures come back from obexd as errors well before.
        if (contains(text, "timeout was reached"))
            return ObexError::DaemonDown;

        // obexd exited while the call was in flight (stopped, crashed): the bus
        // answers for it. Seen on 18 November when obex.service was stopped.
        if (contains(text, "remote peer disconnected") || contains(text, "dbus.error.noreply"))
            return ObexError::DaemonDown;

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
        case ObexError::DaemonDown:
            // TRANSLATORS: {} is Messages or Contacts.
            return tr_format(_("{} is waiting for this computer's OBEX service, which is starting again. "
                               "Nothing needs changing on the iPhone."),
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

    void ProfileSupervisor::close_sessions() {
        if (!map_path_.empty())
            ops_.remove_session(map_path_);
        if (!pbap_path_.empty())
            ops_.remove_session(pbap_path_);
        map_path_.clear();
        pbap_path_.clear();
        map_retried_ = false;
        pbap_retried_ = false;
        status_.map_open = false;
        status_.pbap_open = false;
    }

    void ProfileSupervisor::set_on_demand(bool on_demand) {
        if (on_demand == on_demand_)
            return;
        on_demand_ = on_demand;
        want_data_ = false;
        demand_attempted_ = false;
        // Back to the full mode: open at the next tick, not at a stale date.
        next_attempt_ = 0;
        absent_backoff_ = 0;
    }

    void ProfileSupervisor::set_demand(bool want_data, bool keep_open) {
        if (!on_demand_)
            return;
        if (!keep_open && !want_data && (status_.map_open || status_.pbap_open || !map_path_.empty() ||
                                         !pbap_path_.empty()))
            close_sessions();
        if (want_data && !want_data_) {
            // Wanted from now: tried at the next tick, whatever was scheduled.
            next_attempt_ = 0;
            absent_backoff_ = 0;
            map_next_fast_ = 0;
            pbap_next_fast_ = 0;
            demand_attempted_ = false;
        }
        want_data_ = want_data;
    }

    bool ProfileSupervisor::demand_refused() const {
        if (!on_demand_ || !want_data_ || !demand_attempted_ || status_.map_open || status_.pbap_open)
            return false;
        // obexd being started or restarted is not the phone saying no: the
        // opening waits for it (18 November, 01:13: the hourly sync gave up
        // one second after asking systemd for obexd).
        const auto out = [](bool held, ObexError error) {
            return held || (error != ObexError::None && error != ObexError::DaemonDown);
        };
        const bool map_out = out(status_.map_held, status_.map_error);
        const bool pbap_out = out(status_.pbap_held, status_.pbap_error);
        return map_out && pbap_out;
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

    void ProfileSupervisor::forget_sessions() {
        map_path_.clear();
        pbap_path_.clear();
        map_retried_ = false;
        pbap_retried_ = false;
        status_.map_open = false;
        status_.pbap_open = false;
    }

    bool ProfileSupervisor::recover_daemon(int64_t now, ObexDaemonState state) {
        if (recovery_at_ >= 0 && now - recovery_at_ < OBEX_DAEMON_RECOVERY_SECONDS)
            return recovery_requested_;
        recovery_at_ = now;
        std::string err;
        if (state == ObexDaemonState::Absent) {
            recovery_requested_ = ops_.start_daemon(err);
            if (recovery_requested_)
                debug::log(INFO, "bluetooth: obexd is not on the session bus; asked systemd to start {}", OBEX_UNIT);
            else
                debug::log(WARN, "bluetooth: obexd is not on the session bus and systemd could not start {}: {}",
                           OBEX_UNIT, err);
            return recovery_requested_;
        }
        recovery_requested_ = ops_.restart_daemon(err);
        if (recovery_requested_) {
            debug::log(WARN, "bluetooth: obexd holds its name but does not answer; asked systemd to restart {}",
                       OBEX_UNIT);
            // Whatever it held goes with it.
            forget_sessions();
        } else {
            debug::log(WARN, "bluetooth: obexd does not answer and systemd could not restart {}: {}", OBEX_UNIT, err);
        }
        return recovery_requested_;
    }

    bool ProfileSupervisor::daemon_ready(int64_t now) {
        const ObexDaemonState state = ops_.probe_daemon();
        switch (state) {
        case ObexDaemonState::Absent:
            // No systemd to ask: CreateSession goes through D-Bus activation
            // as it always did, and its error names a missing obexd.
            return !recover_daemon(now, state);
        case ObexDaemonState::Silent:
            // Never worth a CreateSession: it would wait out the full timeout.
            recover_daemon(now, state);
            debug::log(DEBUG, "bluetooth: obexd did not answer a ping; no session asked of it");
            return false;
        default:
            return true;
        }
    }

    bool ProfileSupervisor::open_profile(
        const std::string& target, std::string& path, ObexError& error, bool& retried, int64_t& logged_at) {
        std::string err;
        // obexd already found down in this tick: a second CreateSession would
        // only wait out the same timeout.
        if (daemon_down_this_tick_) {
            path.clear();
            error = ObexError::DaemonDown;
            return false;
        }
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

        // Unanswered until the timeout: obexd was looked at before the call,
        // so it froze or went away meanwhile. Looked at again; brought back
        // when the look confirms it.
        if (error == ObexError::DaemonDown) {
            daemon_down_this_tick_ = true;
            const ObexDaemonState state = ops_.probe_daemon();
            if (state == ObexDaemonState::Absent || state == ObexDaemonState::Silent)
                recover_daemon(tick_now_, state);
            return false;
        }

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
            if (error == ObexError::DaemonDown)
                daemon_down_this_tick_ = true;
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

        // The lean link mode, and nothing needs a session: none is opened.
        // What the phone last answered is still what there is to say.
        if (on_demand_ && !want_data_) {
            if (status_.map_open && status_.pbap_open)
                status_.reason = _("Messages and contacts are connected.");
            else if (!status_.map_open && !status_.map_held && status_.map_error == ObexError::Forbidden)
                status_.reason = obex_error_advice(ObexError::Forbidden, "map");
            else if (!status_.pbap_open && !status_.pbap_held && status_.pbap_error == ObexError::Forbidden)
                status_.reason = obex_error_advice(ObexError::Forbidden, "pbap");
            else
                status_.reason = _("Messages and contacts connect when needed.");
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
            demand_attempted_ = true;
            daemon_down_this_tick_ = !daemon_ready(now);
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
            if (daemon_down_this_tick_ && reachable) {
                // obexd is coming back within seconds: looked at again soon,
                // inside the classic opening's own window. Without the phone,
                // the backoff below applies as for any failure (review of 18
                // November: otherwise a ping, or a 30 s CreateSession, every pass).
                next_attempt_ = now + OBEX_DAEMON_RETRY_SECONDS;
            } else if (due) {
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
