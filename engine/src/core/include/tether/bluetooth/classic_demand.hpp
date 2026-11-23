#pragma once

#include <limits>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// The BR/EDR link as a borrowed resource. In the lean link mode the LE link
// is the only one kept; the classic link is opened when something needs it,
// each need holding a named lease, kept a short while after the last lease
// is returned, then closed. A link the iPhone opens itself is adopted, used
// and closed again, with a guard against a close/reopen loop.
//
// Pure: the supervisor feeds the facts once per tick and carries out the
// action returned. Nothing here touches BlueZ.
namespace tether::bluetooth {

    enum class Lease { Sync, Arrival, Send, Reading, Call, Audio, Hourly, Setup, Probe, Phone };
    // Data: an OBEX session opened by obexd is enough, and brings no audio
    // profile. Voice: a real connection, the only one hands-free follows.
    enum class LeaseKind { Data, Voice };
    enum class ClassicState { Idle, Opening, Open, Lingering, Closing, Failed };

    // An opening gives up after this long.
    inline constexpr int CLASSIC_OPEN_TIMEOUT_SECONDS = 20;
    // A voice lease on a link opened for data: hands-free is asked for over
    // the link as it is, and if it has not come after this long the link is
    // closed and opened again by a real connection, once.
    inline constexpr int CLASSIC_VOICE_WAIT_SECONDS = 8;
    // A link with no voice or audio lease is closed after this long whatever
    // data leases follow one another on it.
    inline constexpr int CLASSIC_DATA_MAX_SECONDS = 300;
    // Openings nobody asked for (sync, arrival, hourly), per hour.
    inline constexpr int CLASSIC_AUTO_OPENS_PER_HOUR = 12;
    // After a failed opening; the hour after the last of these.
    inline constexpr int CLASSIC_FAILURE_BACKOFF_SECONDS[] = {30, 120, 600};
    inline constexpr int CLASSIC_FAILURE_BACKOFF_MAX_SECONDS = 3600;
    // The sync made when the LE link returns, at most this often unless the
    // notification session says there is something new.
    inline constexpr int CLASSIC_SYNC_MIN_INTERVAL_SECONDS = 600;
    // A link the iPhone opened is closed after this long with nothing on it.
    inline constexpr int PHONE_LINK_IDLE_SECONDS = 120;
    // #62: but not while the LE link is missing, up to this long. On 23
    // November closing it (to push the iPhone back to LE) lost calls and
    // messages for nothing: the iPhone stayed silent for 19 minutes.
    inline constexpr int PHONE_LINK_WITHOUT_LE_SECONDS = 1800;
    // The loop guard: the iPhone reopening the link within REOPEN_WINDOW of
    // a close made here, REOPEN_COUNT times within REOPEN_PERIOD, means it
    // wants the link. It is then left alone for KEEPS_LINK.
    inline constexpr int PHONE_REOPEN_WINDOW_SECONDS = 60;
    inline constexpr int PHONE_REOPEN_COUNT = 2;
    inline constexpr int PHONE_REOPEN_PERIOD_SECONDS = 900;
    // #43: the LE link gone while the classic link stays up. On the Intel
    // 9560 the iPhone does not answer the advertisement while it has the
    // classic link (16 and 18 November); notifications keep coming over
    // BR/EDR meanwhile, but nothing brings the LE link back. After this long
    // without LE the classic link is closed once, its leases kept: it opens
    // again when the LE link is back.
    inline constexpr int CLASSIC_LE_CYCLE_SECONDS = 30;
    // Not more often than this.
    inline constexpr int CLASSIC_LE_CYCLE_INTERVAL_SECONDS = 600;
    // #43: "Play iPhone audio on this computer" in the lean mode. Turned on,
    // or asked for again, the link is opened and kept this long so that the
    // iPhone lists this computer and can play to it.
    inline constexpr int PHONE_AUDIO_OFFER_SECONDS = 120;
    // While the iPhone's sound plays here the link stays; after it stops,
    // this long more.
    inline constexpr int PHONE_AUDIO_SILENCE_SECONDS = 60;
    inline constexpr int PHONE_KEEPS_LINK_SECONDS = 1800;
    // And in any case, closes of a link the iPhone opened, per hour.
    inline constexpr int PHONE_CLOSES_PER_HOUR = 4;
    // A lease a client holds (reading, setup) lapses without renewal.
    inline constexpr int CLIENT_LEASE_SECONDS = 300;
    // An automatic lease whose work never finished is not held for ever.
    inline constexpr int AUTO_LEASE_SECONDS = 120;
    // A close BlueZ has not carried out is asked for again.
    inline constexpr int CLASSIC_CLOSE_RETRY_SECONDS = 10;
    // On a link opened for a call, the OBEX sessions wait for hands-free, or
    // this long.
    inline constexpr int CLASSIC_DATA_AFTER_VOICE_SECONDS = 10;

    inline LeaseKind lease_kind(Lease lease) {
        switch (lease) {
        case Lease::Call:
        case Lease::Audio:
        case Lease::Setup:
            return LeaseKind::Voice;
        default:
            return LeaseKind::Data;
        }
    }

    // Opened without anyone asking: counted against the hourly ceiling and
    // never retried at once after a failure.
    inline bool lease_automatic(Lease lease) {
        return lease == Lease::Sync || lease == Lease::Arrival || lease == Lease::Hourly || lease == Lease::Probe;
    }

    // How long the link is kept after the lease is returned.
    inline int lease_linger_seconds(Lease lease) {
        switch (lease) {
        case Lease::Sync:
            return 10;
        case Lease::Arrival:
            return 45;
        case Lease::Send:
            return 90;
        case Lease::Reading:
            return 20;
        case Lease::Call:
            return 15;
        case Lease::Audio:
            return 30;
        default:
            return 0;
        }
    }

    // How long a lease lasts when its holder never returns it; -1 for ever.
    inline int lease_default_ttl(Lease lease) {
        if (lease_automatic(lease))
            return AUTO_LEASE_SECONDS;
        if (lease == Lease::Reading || lease == Lease::Setup)
            return CLIENT_LEASE_SECONDS;
        return -1;
    }

    inline const char* to_string(Lease lease) {
        switch (lease) {
        case Lease::Sync:
            return "sync";
        case Lease::Arrival:
            return "arrival";
        case Lease::Send:
            return "send";
        case Lease::Reading:
            return "reading";
        case Lease::Call:
            return "call";
        case Lease::Audio:
            return "audio";
        case Lease::Hourly:
            return "hourly";
        case Lease::Setup:
            return "setup";
        case Lease::Probe:
            return "probe";
        default:
            return "phone";
        }
    }

    inline const char* to_string(ClassicState state) {
        switch (state) {
        case ClassicState::Opening:
            return "opening";
        case ClassicState::Open:
            return "open";
        case ClassicState::Lingering:
            return "lingering";
        case ClassicState::Closing:
            return "closing";
        case ClassicState::Failed:
            return "failed";
        default:
            return "idle";
        }
    }

    // What the supervisor does with the Audio lease, each pass (#43). The
    // choice alone no longer holds the classic link: before 18 November the
    // lease lasted as long as the choice, and the link stayed up for hours.
    // `asked`: the user turned the sound on here, or asked for it again;
    // `playing`: an A2DP stream from the iPhone is playing (transport pending
    // or active), not merely connected.
    enum class AudioLeaseMove { None, Take, Release };
    struct AudioLeasePlan {
        AudioLeaseMove move = AudioLeaseMove::None;
        int ttl_seconds = 0;
    };
    inline AudioLeasePlan plan_audio_lease(bool wanted, bool asked, bool playing, bool held) {
        if (!wanted)
            return {held ? AudioLeaseMove::Release : AudioLeaseMove::None, 0};
        if (playing)
            return {AudioLeaseMove::Take, PHONE_AUDIO_SILENCE_SECONDS};
        if (asked)
            return {AudioLeaseMove::Take, PHONE_AUDIO_OFFER_SECONDS};
        return {};
    }

    struct DemandFacts {
        int64_t now = 0;
        bool le_connected = false;
        bool classic_connected = false;
        bool map_open = false;
        bool pbap_open = false;
        // The iPhone's hands-free gateway is there.
        bool hands_free = false;
        bool call_active = false;
        // An A2DP stream from the iPhone is playing here (transport pending
        // or active). A stream connected but idle does not count (#43).
        bool phone_audio_streaming = false;
        // logind has announced the computer's sleep.
        bool suspended_soon = false;
        // Both OBEX profiles were tried on this opening and neither opened.
        bool data_refused = false;
        // Someone else is using the link (NetworkManager's tethering).
        bool held_by_other = false;
    };

    enum class DemandAction {
        None,
        // Let obexd open the sessions: it brings the link, without audio.
        OpenData,
        // A real connection (Bearer.BREDR1.Connect), or hands-free asked for
        // over the link already there.
        OpenVoice,
        // Drop the data link; OpenVoice follows once it is down.
        RecycleForVoice,
        // Remove the sessions, then Bearer.BREDR1.Disconnect.
        Close,
    };

    // A date taken before the computer slept, seen from after it: the
    // thread's clock stands still during a sleep, so without this a night
    // asleep reads as minutes (18 November: woken at 10:14 after seven hours,
    // the last sync looked six minutes old and the mailbox was left unread).
    // A date that falls before the clock's start reads as "long ago", not as
    // "never" (-1): the hourly safety net only starts once something was
    // listed, and must not stop after a sleep longer than the engine's run.
    inline constexpr int64_t SYNC_LONG_AGO = std::numeric_limits<int64_t>::min() / 4;
    inline int64_t dated_across_sleep(int64_t at, int64_t slept_seconds) {
        if (at < 0 || slept_seconds <= 0)
            return at;
        return at - slept_seconds >= 0 ? at - slept_seconds : SYNC_LONG_AGO;
    }

    // Whether the sync made when the LE link returns is due. `backlog` is the
    // notification session announcing Messages notifications already there.
    inline bool sync_due_on_le_return(int64_t now, int64_t last_sync_lease_at, bool backlog) {
        return backlog || last_sync_lease_at < 0 || now - last_sync_lease_at >= CLASSIC_SYNC_MIN_INTERVAL_SECONDS;
    }

    class ClassicDemand {
    public:
        // False when the lease is refused: an automatic one over the hourly
        // ceiling or during the wait after a failure. `ttl_seconds` 0 takes
        // the lease's own default. `urgent` (a call ringing) recycles a data
        // link at once rather than after CLASSIC_VOICE_WAIT_SECONDS.
        bool take(Lease lease, int64_t now, int ttl_seconds = 0, bool urgent = false) {
            if (lease == Lease::Phone)
                return false;
            const bool link_there = state_ == ClassicState::Opening || state_ == ClassicState::Open ||
                                    state_ == ClassicState::Lingering;
            if (lease_automatic(lease)) {
                if (state_ == ClassicState::Failed && now < retry_at_)
                    return false;
                prune(now);
                if (!link_there && static_cast<int>(auto_opens_.size()) >= CLASSIC_AUTO_OPENS_PER_HOUR) {
                    capped_ = true;
                    return false;
                }
            } else if (state_ == ClassicState::Failed) {
                // The user is asking: no waiting out a backoff.
                state_ = ClassicState::Idle;
                retry_at_ = -1;
            }
            const int ttl = ttl_seconds > 0 ? ttl_seconds : lease_default_ttl(lease);
            leases_[lease] = ttl > 0 ? now + ttl : -1;
            if (urgent && lease_kind(lease) == LeaseKind::Voice)
                recycle_now_ = true;
            return true;
        }

        void release(Lease lease, int64_t now) {
            if (lease == Lease::Phone || leases_.erase(lease) == 0)
                return;
            linger_until_ = std::max(linger_until_, now + lease_linger_seconds(lease));
        }

        bool holds(Lease lease) const { return leases_.count(lease) > 0; }

        ClassicState state() const { return state_; }

        // Phone included, while a link the iPhone opened is being used.
        std::vector<Lease> leases() const {
            std::vector<Lease> out;
            for (const auto& [lease, expires] : leases_)
                out.push_back(lease);
            if (opened_by_ == Opener::Phone && state_ != ClassicState::Idle && state_ != ClassicState::Closing)
                out.push_back(Lease::Phone);
            return out;
        }

        // "engine", "phone", or "" while there is no link.
        const char* opened_by() const {
            if (state_ == ClassicState::Idle || state_ == ClassicState::Failed)
                return "";
            return opened_by_ == Opener::Phone ? "phone" : opened_by_ == Opener::Engine ? "engine" : "";
        }

        // The iPhone has reopened the link right after it was closed, twice:
        // the link is left to it for a while.
        bool phone_keeps_link(int64_t now) const { return keeps_until_ >= 0 && now < keeps_until_; }
        bool phone_keeps_link() const { return phone_keeps_link(now_); }

        // The last automatic lease was refused by the hourly ceiling.
        bool capped() const { return capped_; }

        // The classic link was just closed because the LE link had gone (#43).
        bool cycled_for_le(int64_t now) const { return le_cycled_at_ >= 0 && le_cycled_at_ == now; }

        // Whether a call (placed, ringing, in progress) keeps obexd from
        // opening sessions. On by default: a session opened under a call
        // competes with it for the link, and its reads hold the thread that
        // follows the call.
        void set_data_waits_for_calls(bool waits) { data_waits_for_calls_ = waits; }

        // Whether obexd may open sessions now.
        bool want_data() const {
            if (!has_work())
                return false;
            // A message being sent is the user's own doing: not held back.
            if (data_waits_for_calls_ && (holds(Lease::Call) || call_active_) && !holds(Lease::Send))
                return false;
            if (state_ == ClassicState::Opening)
                return !opening_voice_ && !awaiting_drop_;
            if (state_ != ClassicState::Open)
                return false;
            return !wants_voice() || hands_free_seen_ || now_ - open_since_ >= CLASSIC_DATA_AFTER_VOICE_SECONDS;
        }

        // Whether sessions already open are left open.
        bool keep_data() const {
            return state_ == ClassicState::Open || state_ == ClassicState::Lingering ||
                   (state_ == ClassicState::Opening && !awaiting_drop_);
        }

        // When the wait after a failure ends, -1 outside it.
        int64_t retry_at() const { return state_ == ClassicState::Failed ? retry_at_ : -1; }

        // The state forgotten; the leases, which their holders will return,
        // and what the loop guard counted are kept. For a switch to the full
        // mode and back.
        void reset() {
            state_ = ClassicState::Idle;
            opened_by_ = Opener::None;
            linger_until_ = -1;
            retry_at_ = -1;
            failures_ = 0;
            last_close_at_ = -1;
            awaiting_drop_ = false;
            recycle_now_ = false;
            capped_ = false;
        }

        // The link is up and this side brought it (the full mode did, or an
        // appairing): used, then closed like any other, and not held against
        // the iPhone.
        void adopt_engine_link(int64_t now) {
            state_ = ClassicState::Open;
            opened_by_ = Opener::Engine;
            open_since_ = now;
            link_is_voice_ = true;
            voice_asked_at_ = -1;
            recycled_ = false;
            linger_until_ = std::max(linger_until_, now);
        }

        DemandAction step(const DemandFacts& facts) {
            now_ = facts.now;
            call_active_ = facts.call_active;
            prune(facts.now);
            expire(facts.now);
            if (keeps_until_ >= 0 && facts.now >= keeps_until_)
                keeps_until_ = -1;
            if (facts.hands_free)
                hands_free_seen_ = true;
            else if (state_ == ClassicState::Idle || state_ == ClassicState::Opening)
                hands_free_seen_ = false;
            if (facts.le_connected)
                le_lost_since_ = -1;
            else if (le_lost_since_ < 0)
                le_lost_since_ = facts.now;
            if (const DemandAction cycle = cycle_for_le(facts); cycle != DemandAction::None)
                return cycle;

            switch (state_) {
            case ClassicState::Idle:
                return step_idle(facts);
            case ClassicState::Opening:
                return step_opening(facts);
            case ClassicState::Open:
                return step_open(facts);
            case ClassicState::Lingering:
                return step_lingering(facts);
            case ClassicState::Closing:
                return step_closing(facts);
            case ClassicState::Failed:
                return step_failed(facts);
            }
            return DemandAction::None;
        }

    private:
        enum class Opener { None, Engine, Phone };

        static bool link_up(const DemandFacts& f) { return f.classic_connected || f.map_open || f.pbap_open; }
        static bool busy(const DemandFacts& f) {
            return f.call_active || f.phone_audio_streaming || f.held_by_other;
        }

        bool has_work() const { return !leases_.empty(); }

        bool wants_voice() const {
            for (const auto& [lease, expires] : leases_)
                if (lease_kind(lease) == LeaseKind::Voice)
                    return true;
            return false;
        }

        bool all_automatic() const {
            for (const auto& [lease, expires] : leases_)
                if (!lease_automatic(lease))
                    return false;
            return !leases_.empty();
        }

        // #43: the classic link up, the LE link gone for CLASSIC_LE_CYCLE_SECONDS.
        // Closed once, whoever opened it and whatever leases it serves: they
        // are kept, and step_idle() opens the link again for them once the LE
        // link is back. Never under a call, while the iPhone's sound plays here
        // or while NetworkManager uses the link.
        DemandAction cycle_for_le(const DemandFacts& f) {
            // #62: a link the iPhone opened is kept: closing it did not bring
            // the LE link back (23 November) and took calls and messages away.
            if (opened_by_ == Opener::Phone)
                return DemandAction::None;
            if (le_lost_since_ < 0 || f.now - le_lost_since_ < CLASSIC_LE_CYCLE_SECONDS)
                return DemandAction::None;
            if (state_ != ClassicState::Open && state_ != ClassicState::Lingering)
                return DemandAction::None;
            if (!link_up(f) || busy(f) || holds(Lease::Call))
                return DemandAction::None;
            if (le_cycled_at_ >= 0 && f.now - le_cycled_at_ < CLASSIC_LE_CYCLE_INTERVAL_SECONDS)
                return DemandAction::None;
            le_cycled_at_ = f.now;
            drop_automatic();
            state_ = ClassicState::Closing;
            close_asked_at_ = f.now;
            recycle_now_ = false;
            return DemandAction::Close;
        }

        void drop_automatic() {
            for (auto it = leases_.begin(); it != leases_.end();)
                it = lease_automatic(it->first) ? leases_.erase(it) : std::next(it);
        }

        void drop_data() {
            for (auto it = leases_.begin(); it != leases_.end();)
                it = lease_kind(it->first) == LeaseKind::Data ? leases_.erase(it) : std::next(it);
        }

        void drop_all_but_call() {
            for (auto it = leases_.begin(); it != leases_.end();)
                it = it->first != Lease::Call ? leases_.erase(it) : std::next(it);
        }

        static void prune(std::deque<int64_t>& times, int64_t now, int window) {
            while (!times.empty() && now - times.front() >= window)
                times.pop_front();
        }

        void prune(int64_t now) {
            prune(auto_opens_, now, 3600);
            prune(phone_reopens_, now, PHONE_REOPEN_PERIOD_SECONDS);
            prune(phone_closes_, now, 3600);
        }

        void expire(int64_t now) {
            for (auto it = leases_.begin(); it != leases_.end();) {
                if (it->second >= 0 && now >= it->second) {
                    linger_until_ = std::max(linger_until_, now + lease_linger_seconds(it->first));
                    it = leases_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void begin_opening(int64_t now, bool voice) {
            state_ = ClassicState::Opening;
            opening_since_ = now;
            opening_voice_ = voice;
            link_is_voice_ = voice;
            opened_by_ = Opener::Engine;
            awaiting_drop_ = false;
            voice_asked_at_ = -1;
        }

        // `keep`: someone else uses the link (NetworkManager's tethering
        // rides it): the half that came up is left, not taken down.
        DemandAction fail(int64_t now, bool keep = false) {
            // A probe nobody answered is an answer (#62): the iPhone is not
            // there. No backoff, or the sync due when it comes back would wait.
            if (holds(Lease::Probe) && leases_.size() == 1) {
                leases_.clear();
                retry_at_ = now;
                state_ = ClassicState::Failed;
                opened_by_ = Opener::None;
                awaiting_drop_ = false;
                recycle_now_ = false;
                return keep ? DemandAction::None : DemandAction::Close;
            }
            const int index = failures_;
            ++failures_;
            const int wait = index < 3 ? CLASSIC_FAILURE_BACKOFF_SECONDS[index] : CLASSIC_FAILURE_BACKOFF_MAX_SECONDS;
            retry_at_ = now + wait;
            drop_automatic();
            state_ = ClassicState::Failed;
            opened_by_ = Opener::None;
            awaiting_drop_ = false;
            recycle_now_ = false;
            // Whatever half came up is taken down, unless it serves another.
            return keep ? DemandAction::None : DemandAction::Close;
        }

        // Closes unless the link is the iPhone's and the guard says no.
        DemandAction close(int64_t now) {
            if (opened_by_ == Opener::Phone) {
                if (keeps_until_ >= 0 || static_cast<int>(phone_closes_.size()) >= PHONE_CLOSES_PER_HOUR)
                    return DemandAction::None;
                phone_closes_.push_back(now);
            }
            state_ = ClassicState::Closing;
            close_asked_at_ = now;
            recycle_now_ = false;
            return DemandAction::Close;
        }

        DemandAction step_idle(const DemandFacts& f) {
            if (link_up(f)) {
                // Nobody here asked for it: the iPhone, or BlueZ on its behalf.
                state_ = ClassicState::Open;
                opened_by_ = Opener::Phone;
                open_since_ = f.now;
                link_is_voice_ = false;
                voice_asked_at_ = -1;
                recycled_ = false;
                // The two minutes run from now unless something uses it.
                phone_idle_since_ = f.now;
                if (last_close_at_ >= 0 && f.now - last_close_at_ < PHONE_REOPEN_WINDOW_SECONDS) {
                    phone_reopens_.push_back(f.now);
                    if (static_cast<int>(phone_reopens_.size()) >= PHONE_REOPEN_COUNT) {
                        keeps_until_ = f.now + PHONE_KEEPS_LINK_SECONDS;
                        phone_reopens_.clear();
                    }
                }
                return DemandAction::None;
            }
            if (!has_work())
                return DemandAction::None;
            // No LE link, no opening: a phone not known to be there is never
            // paged. Except by the probe, whose very purpose that is (#62);
            // the other automatic leases still wait for the LE link.
            if (!f.le_connected) {
                const bool probe = holds(Lease::Probe);
                drop_automatic();
                if (!probe)
                    return DemandAction::None;
                leases_[Lease::Probe] = f.now + lease_default_ttl(Lease::Probe);
            }
            if (f.suspended_soon) {
                leases_.clear();
                return DemandAction::None;
            }
            if (all_automatic()) {
                if (static_cast<int>(auto_opens_.size()) >= CLASSIC_AUTO_OPENS_PER_HOUR) {
                    drop_automatic();
                    capped_ = true;
                    return DemandAction::None;
                }
                auto_opens_.push_back(f.now);
            }
            capped_ = false;
            recycled_ = false;
            const bool voice = wants_voice();
            begin_opening(f.now, voice);
            return voice ? DemandAction::OpenVoice : DemandAction::OpenData;
        }

        DemandAction step_opening(const DemandFacts& f) {
            if (f.suspended_soon && !holds(Lease::Call)) {
                leases_.clear();
                return close(f.now);
            }
            if (awaiting_drop_) {
                if (!f.classic_connected) {
                    awaiting_drop_ = false;
                    opening_since_ = f.now;
                    return DemandAction::OpenVoice;
                }
            } else {
                const bool ready = opening_voice_ ? f.classic_connected : (f.map_open || f.pbap_open);
                if (ready) {
                    state_ = ClassicState::Open;
                    open_since_ = f.now;
                    failures_ = 0;
                    return step_open(f);
                }
                if (!opening_voice_ && wants_voice()) {
                    // A voice lease joined an opening made for data: the
                    // connection is asked for as well, over whatever obexd brings.
                    opening_voice_ = true;
                    voice_asked_at_ = f.now;
                    return DemandAction::OpenVoice;
                }
                if (!opening_voice_ && f.data_refused)
                    return fail(f.now, f.held_by_other);
            }
            if (f.now - opening_since_ >= CLASSIC_OPEN_TIMEOUT_SECONDS)
                return fail(f.now, f.held_by_other);
            return DemandAction::None;
        }

        DemandAction step_open(const DemandFacts& f) {
            if (!link_up(f)) {
                // Lost under the leases, or closed by the phone.
                opened_by_ = Opener::None;
                if (has_work()) {
                    fail(f.now);
                    return DemandAction::None;
                }
                state_ = ClassicState::Idle;
                return DemandAction::None;
            }
            if (f.suspended_soon) {
                drop_all_but_call();
                if (!holds(Lease::Call) && !f.call_active)
                    return close(f.now);
            }

            if (wants_voice() && !f.hands_free && !link_is_voice_) {
                const bool waited = voice_asked_at_ >= 0 && f.now - voice_asked_at_ >= CLASSIC_VOICE_WAIT_SECONDS;
                if (!recycled_ && (recycle_now_ || waited)) {
                    recycled_ = true;
                    recycle_now_ = false;
                    begin_opening(f.now, true);
                    awaiting_drop_ = true;
                    return DemandAction::RecycleForVoice;
                }
                if (voice_asked_at_ < 0) {
                    voice_asked_at_ = f.now;
                    return DemandAction::OpenVoice;
                }
            }
            if (f.hands_free)
                recycle_now_ = false;

            // Data leases following one another do not make a permanent link.
            if (has_work() && opened_by_ == Opener::Engine && !wants_voice() && !busy(f) && !holds(Lease::Send) &&
                f.now - open_since_ >= CLASSIC_DATA_MAX_SECONDS) {
                drop_data();
                return close(f.now);
            }

            if (has_work()) {
                phone_idle_since_ = -1;
                return DemandAction::None;
            }

            if (opened_by_ == Opener::Phone) {
                if (busy(f)) {
                    phone_idle_since_ = -1;
                    return DemandAction::None;
                }
                if (phone_idle_since_ < 0)
                    phone_idle_since_ = f.now;
                const int idle_limit = f.le_connected ? PHONE_LINK_IDLE_SECONDS : PHONE_LINK_WITHOUT_LE_SECONDS;
                if (f.now - phone_idle_since_ < idle_limit || f.now < linger_until_)
                    return DemandAction::None;
                return close(f.now);
            }

            // BR/EDR up keeps the LE link from coming back: without LE the
            // link is only kept while it serves.
            if (!f.le_connected && !busy(f))
                return close(f.now);
            state_ = ClassicState::Lingering;
            if (linger_until_ < f.now)
                linger_until_ = f.now;
            return step_lingering(f);
        }

        DemandAction step_lingering(const DemandFacts& f) {
            if (!link_up(f)) {
                state_ = ClassicState::Idle;
                opened_by_ = Opener::None;
                return DemandAction::None;
            }
            if (has_work()) {
                state_ = ClassicState::Open;
                return step_open(f);
            }
            // Never under a call or the phone's sound.
            if (busy(f))
                return DemandAction::None;
            if (f.suspended_soon || !f.le_connected || f.now >= linger_until_)
                return close(f.now);
            return DemandAction::None;
        }

        DemandAction step_closing(const DemandFacts& f) {
            if (link_up(f) && busy(f)) {
                // Taken up meanwhile (tethering started on the link being
                // closed, a call): no second close. It is someone else's
                // now, closed like a link the iPhone opened once it is idle.
                state_ = ClassicState::Open;
                opened_by_ = Opener::Phone;
                open_since_ = f.now;
                phone_idle_since_ = -1;
                return DemandAction::None;
            }
            if (!link_up(f)) {
                state_ = ClassicState::Idle;
                opened_by_ = Opener::None;
                last_close_at_ = f.now;
                // A lease taken meanwhile is served from Idle, on the next step.
                return DemandAction::None;
            }
            if (f.now - close_asked_at_ >= CLASSIC_CLOSE_RETRY_SECONDS) {
                close_asked_at_ = f.now;
                return DemandAction::Close;
            }
            return DemandAction::None;
        }

        DemandAction step_failed(const DemandFacts& f) {
            if (f.classic_connected) {
                // The opening given up on came through after all.
                state_ = ClassicState::Open;
                opened_by_ = Opener::Engine;
                open_since_ = f.now;
                link_is_voice_ = false;
                voice_asked_at_ = -1;
                return step_open(f);
            }
            if (f.now >= retry_at_) {
                state_ = ClassicState::Idle;
                retry_at_ = -1;
                return step_idle(f);
            }
            return DemandAction::None;
        }

        // Lease -> when it lapses on its own, -1 for never.
        std::map<Lease, int64_t> leases_;
        ClassicState state_ = ClassicState::Idle;
        Opener opened_by_ = Opener::None;
        int64_t now_ = 0;
        bool call_active_ = false;
        bool data_waits_for_calls_ = true;

        int64_t opening_since_ = -1;
        bool opening_voice_ = false;
        // RecycleForVoice was returned and the link has not gone down yet.
        bool awaiting_drop_ = false;
        int64_t open_since_ = -1;
        // Whether the link in place came from a real connection.
        bool link_is_voice_ = false;
        bool hands_free_seen_ = false;
        int64_t voice_asked_at_ = -1;
        bool recycled_ = false;
        bool recycle_now_ = false;
        int64_t linger_until_ = -1;
        int64_t close_asked_at_ = -1;
        // When a link closed here was last seen down.
        int64_t last_close_at_ = -1;

        int failures_ = 0;
        int64_t retry_at_ = -1;
        std::deque<int64_t> auto_opens_;
        bool capped_ = false;

        int64_t phone_idle_since_ = -1;
        std::deque<int64_t> phone_reopens_;
        std::deque<int64_t> phone_closes_;
        int64_t keeps_until_ = -1;
        // Since when the LE link is down, -1 while it is up.
        int64_t le_lost_since_ = -1;
        int64_t le_cycled_at_ = -1;
    };

} // namespace tether::bluetooth
