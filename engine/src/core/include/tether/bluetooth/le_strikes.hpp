#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <tether/bluetooth/advert.hpp>

// LE links that died before they were of any use, counted the way iOS is
// believed to count them.
//
// An iPhone keeps coming back to an accessory "as long as it is paired and
// uses one of the available services" (Accessory Design Guidelines R32,
// 57.12.4). The console of an iPhone published in 2015 shows the other side
// of that sentence: every LE link that ends before the accessory has used a
// built-in service is logged as "did not use any built-in service, strike
// #1", "#2", "#3", and after the third the iPhone stops reconnecting until
// its owner does something. A link that had subscribed is not counted.
//
// That reading did not survive two real walks out and back (18 November
// 2026, issues #1 and #29): both times the iPhone linked again right after
// three unused links in a row, and what delayed the notifications was this
// computer, not the phone. The advert had been withheld 15, 30 then 60 s
// after each unused link and put back at 1022.5 ms from the second on; the
// iPhone took about 80 s to answer that slow advert indoors, then the third
// unused link cost another minute off air.
//
// So the links that ended before the notification service had answered on
// them are still counted (for the journal, and for le_state: "links are
// failing, trying again"), but by default nothing is withheld any more:
//   - after each such link the advert stays on air, at 20 ms for
//     LE_STRIKE_FAST_SECONDS, then at 152.5 ms, Apple's first interval
//     after the fast phase, for as long as no link has served;
//   - a link on which the service has been live for a while clears the
//     count, and the usual rhythm (advert.hpp) takes over again.
// LANDLINE_LE_STRIKE_PAUSE_SECONDS=15 LANDLINE_LE_STRIKE_LONG_PAUSE_SECONDS=30
// LANDLINE_LE_STRIKE_SLOW=2 LANDLINE_LE_STRIKE_FAST_SECONDS=0 give the rule
// of before back: withheld 15 s, then 30 s doubled up to two minutes, slow
// from the second.
//
// Links that served and then dropped are not strikes, but a run of them is
// a phone at the edge of range coming and going. Pebble's rule for that
// (gap_le_slave_reconnect.c, commit 5dd4e57: "~130 connect/disconnect cycles
// in 70 minutes") is taken as it is: more than three such losses within a
// minute and the advert goes at its slowest interval, with no fast phase,
// until a minute has passed without one.
//
// A link that served after the last strike, if only for a few seconds and
// not long enough to clear the count, gives the usual rhythm back: lost, it
// is an ordinary loss, 20 ms for 180 s (#40). The strikes stay on record for
// the journal and le_state.
//
// An iPhone that links and drops for ever without answering would otherwise
// be given a fresh fast phase after every strike, and the count is never
// forgotten while strikes keep coming. From LE_STRIKE_BRAKE_FROM unused links
// in a row with no link served in between, far beyond anything seen on a
// walk (three), the fast phase is no longer given: 152.5 ms, said once in the
// journal, until a link serves (#41).
//
// A decision on facts handed in; nothing here touches BlueZ.
namespace tether::bluetooth {

    // How long the advert stays off air after the first link that died
    // unused: by default not at all (#29). It was 15 s, after the strikes of
    // 2015, about three seconds apart, whose author put the window at "11 or
    // so seconds"; on 18 November the iPhone came back after every pause.
    inline constexpr int LE_STRIKE_PAUSE_SECONDS = 0;
    inline constexpr int LE_STRIKE_PAUSE_SECONDS_MAX = 600;
    // And after the second, doubled for each one that follows: by default
    // not at all either. It was 30 s, and 120 s before that.
    inline constexpr int LE_STRIKE_LONG_PAUSE_SECONDS = 0;
    inline constexpr int LE_STRIKE_LONG_PAUSE_SECONDS_MAX = 3600;
    // After each link that died unused, the advert goes at 20 ms for this
    // long, then at 152.5 ms. Apple's guidelines ask for 20 ms during at
    // least the first 30 s, then one of a list of longer intervals (152.5,
    // 211.25, 318.75, 417.5, 546.25, 760, 852.5, 1022.5, 1285 ms). The
    // shortest of them is kept after that: at 1022.5 ms an iPhone indoors
    // took about 80 s to answer on 18 November, and each longer interval
    // risks the same kind of wait against a phone that only listens in
    // short windows. The 180 s of fast advert that follow an ordinary loss
    // are not given after an unused link: at the edge of range they would
    // offer the phone a failing link every second or two for three minutes,
    // where 30 s covers what Apple asks and a phone just come in the door.
    // 0: the strike leaves the rhythm alone, 20 ms for 180 s after the loss
    // then 152.5 ms, as before #29.
    inline constexpr int LE_STRIKE_FAST_SECONDS = 30;
    inline constexpr int LE_STRIKE_FAST_SECONDS_MAX = 180;
    // Where the doubling stops.
    inline constexpr int LE_STRIKE_PAUSE_CEILING_SECONDS = 120;
    // The notification service live on a link for this long: the link has
    // served, and what was counted is forgotten.
    inline constexpr int LE_SERVED_SECONDS = 10;
    inline constexpr int LE_SERVED_SECONDS_MAX = 600;
    // With no new strike for this long, and no pause running, the count is
    // forgotten as well: the phone left, and its next arrival starts afresh.
    inline constexpr int LE_STRIKE_MEMORY_SECONDS = 900;
    // From this many strikes on, a link lost for good is put down to the
    // phone having given up, and a pause, if any, is the long one.
    inline constexpr int LE_STRIKES_SLOW = 2;
    // From how many strikes on the advert goes at its slowest interval
    // (1022.5 ms): by default never (#29). It was LE_STRIKES_SLOW.
    inline constexpr int LE_STRIKE_SLOW_FROM = 0;
    inline constexpr int LE_STRIKE_SLOW_FROM_MAX = 99;
    // How long after the last strike a lost link is still put down to them.
    inline constexpr int LE_STRIKE_BLAME_SECONDS = 3600;
    // From this many unused links in a row, none served in between, no fast
    // phase any more (#41). Three on 18 November; 0 turns the brake off.
    inline constexpr int LE_STRIKE_BRAKE_FROM = 8;
    inline constexpr int LE_STRIKE_BRAKE_FROM_MAX = 99;
    // More than this many losses of links that had served, within
    // LE_CHURN_WINDOW_SECONDS: the advert slows down for as long again.
    inline constexpr int LE_CHURN_LOSSES = 3;
    inline constexpr int LE_CHURN_WINDOW_SECONDS = 60;

    struct LeStrikesConfig {
        // false: nothing is counted and the rule of le_health.hpp applies.
        bool enabled = true;
        // 0: the first strike is counted and the advert stays on air.
        int first_pause_seconds = LE_STRIKE_PAUSE_SECONDS;
        // 0: no pause from the second strike on either.
        int long_pause_seconds = LE_STRIKE_LONG_PAUSE_SECONDS;
        int served_seconds = LE_SERVED_SECONDS;
        // 0: the rhythm of an ordinary loss.
        int fast_seconds = LE_STRIKE_FAST_SECONDS;
        // 0: never slow because of strikes (churn still is).
        int slow_from = LE_STRIKE_SLOW_FROM;
        // 0: a fast phase after every strike, however many in a row.
        int brake_from = LE_STRIKE_BRAKE_FROM;
    };

    // LANDLINE_LE_STRIKES=0, LANDLINE_LE_STRIKE_PAUSE_SECONDS (0 to 600),
    // LANDLINE_LE_STRIKE_LONG_PAUSE_SECONDS (0 to 3600),
    // LANDLINE_LE_SERVED_SECONDS (1 to 600),
    // LANDLINE_LE_STRIKE_FAST_SECONDS (0 to 180), LANDLINE_LE_STRIKE_SLOW
    // (0 to 99), LANDLINE_LE_STRIKE_BRAKE (0 to 99).
    inline LeStrikesConfig le_strikes_config_from(const char* enabled,
                                                  const char* first_pause,
                                                  const char* long_pause,
                                                  const char* served,
                                                  const char* fast = nullptr,
                                                  const char* slow_from = nullptr,
                                                  const char* brake_from = nullptr) {
        const auto number = [](const char* text, int fallback, int min, int max) {
            if (!text || !*text)
                return fallback;
            int value = 0;
            for (const char* c = text; *c; ++c) {
                if (*c < '0' || *c > '9' || value > max)
                    return fallback;
                value = value * 10 + (*c - '0');
            }
            return value >= min && value <= max ? value : fallback;
        };
        LeStrikesConfig config;
        config.enabled = !enabled || std::string_view(enabled) != "0";
        config.first_pause_seconds = number(first_pause, LE_STRIKE_PAUSE_SECONDS, 0, LE_STRIKE_PAUSE_SECONDS_MAX);
        config.long_pause_seconds =
            number(long_pause, LE_STRIKE_LONG_PAUSE_SECONDS, 0, LE_STRIKE_LONG_PAUSE_SECONDS_MAX);
        config.served_seconds = number(served, LE_SERVED_SECONDS, 1, LE_SERVED_SECONDS_MAX);
        config.fast_seconds = number(fast, LE_STRIKE_FAST_SECONDS, 0, LE_STRIKE_FAST_SECONDS_MAX);
        config.slow_from = number(slow_from, LE_STRIKE_SLOW_FROM, 0, LE_STRIKE_SLOW_FROM_MAX);
        config.brake_from = number(brake_from, LE_STRIKE_BRAKE_FROM, 0, LE_STRIKE_BRAKE_FROM_MAX);
        return config;
    }

    struct LeStrikeFacts {
        int64_t now = 0;
        // Whether a link that ends unused is the phone's to count: the
        // notifications are wanted, the bond is whole and the phone has
        // offered the service on it before. A pairing under way, or a phone
        // whose owner shares no notifications, is not held to this.
        bool armed = false;
        bool le_connected = false;
        // The monitor's count of Disconnected signals on the LE bearer. Only
        // its change matters; it sees the links that came and went between
        // two passes.
        uint64_t drop_count = 0;
        // The notification service has answered on the link: the phone has
        // sent something on the Data Source. On the pass that sees a drop
        // this still describes the link that ended.
        bool service_live = false;
    };

    class LeStrikes {
    public:
        // Something to write in the journal, once.
        struct Event {
            enum class Kind {
                // A link ended before the service had answered on it.
                Strike,
                // A link has served: the count is forgotten.
                Served,
                // Nothing for a long while: the count is forgotten.
                Forgotten,
                // Links that had served keep dropping: the advert slows
                // down (links: the losses in the window).
                Churn,
            } kind;
            // Strike: the count after it, how many links ended unused in this
            // pass, and for how long the advert is withheld (0: left on air).
            // Served, Forgotten: the count that was dropped.
            int strikes = 0;
            int links = 0;
            int pause_seconds = 0;
            // Strike: this one puts the brake on (#41), to be said once.
            bool braked = false;
        };

        void configure(const LeStrikesConfig& config) { config_ = config; }
        const LeStrikesConfig& config() const { return config_; }

        // Once per pass, before the notification client is told of the drop.
        std::optional<Event> observe(const LeStrikeFacts& facts) {
            const int64_t now = facts.now;
            uint64_t fresh = 0;
            if (!primed_) {
                primed_ = true;
            } else if (facts.drop_count > drops_seen_) {
                fresh = facts.drop_count - drops_seen_;
            } else if (le_was_up_ && !facts.le_connected && (last_drop_at_ < 0 || now - last_drop_at_ > 2)) {
                // A stack that sends no signal: the edge is all there is.
                fresh = 1;
            }
            drops_seen_ = facts.drop_count;
            le_was_up_ = facts.le_connected;
            if (!config_.enabled)
                return std::nullopt;

            std::optional<Event> event;
            if (fresh > 0) {
                last_drop_at_ = now;
                // The first link that ended is the one the service flag
                // describes; any other came and went between two passes, too
                // briefly to have been seen serving.
                const bool first_served = served_link_ || facts.service_live;
                const uint64_t unused = first_served ? fresh - 1 : fresh;
                served_link_ = false;
                live_since_ = -1;
                if (unused > 0 && facts.armed) {
                    event = strike(now, static_cast<int>(std::min<uint64_t>(unused, 99)));
                } else if (first_served && facts.armed) {
                    // A link served after the last strike: the strikes no
                    // longer set the rhythm, and this loss is an ordinary one
                    // (#40).
                    served_since_strike_ = true;
                    unused_run_ = 0;
                    event = served_loss(now);
                }
                return event;
            }

            // The notification client is told of a drop in the pass that sees
            // it, after this, and forgets its session: by the next pass the
            // flag is about the link that is up now.
            const bool live = facts.le_connected && facts.service_live;
            if (!live) {
                live_since_ = -1;
            } else {
                served_link_ = true;
                if (live_since_ < 0)
                    live_since_ = now;
                if (strikes_ > 0 && now - live_since_ >= config_.served_seconds) {
                    event = Event{Event::Kind::Served, strikes_};
                    clear();
                    return event;
                }
            }
            if (strikes_ > 0 && now >= pause_until_ && now - last_strike_at_ >= LE_STRIKE_MEMORY_SECONDS) {
                event = Event{Event::Kind::Forgotten, strikes_};
                strikes_ = 0;
                pause_until_ = -1;
                served_since_strike_ = false;
                unused_run_ = 0;
            }
            return event;
        }

        // Links that ended unused since a link last served.
        int strikes() const { return strikes_; }

        // Whether the advert is withheld because of them.
        bool advert_paused(int64_t now) const { return config_.enabled && now < pause_until_; }

        // Seconds left of that, 0 outside it.
        int pause_left(int64_t now) const { return advert_paused(now) ? static_cast<int>(pause_until_ - now) : 0; }

        // Whether the advert, when it is on air, goes at its slowest interval.
        // Never by default (#29).
        bool slow_advert() const {
            return config_.enabled && config_.slow_from > 0 && strikes_ >= config_.slow_from;
        }

        // Links have been ending unused and none has served since.
        bool unsteady() const { return config_.enabled && strikes_ > 0; }

        // Whether the strikes set the rhythm of the advert: they do while
        // they are on record, unless LANDLINE_LE_STRIKE_FAST_SECONDS=0 or a
        // link has served since the last of them (#40).
        bool sets_rhythm() const { return unsteady() && config_.fast_seconds > 0 && !served_since_strike_; }

        // A link has served since the last strike, too briefly to clear the
        // count: the strikes are still on record, the rhythm is the usual one.
        bool served_since_strike() const { return config_.enabled && strikes_ > 0 && served_since_strike_; }

        // So many unused links in a row, none served in between, that no
        // fast phase is given any more (#41).
        bool braked() const {
            return config_.enabled && config_.brake_from > 0 && unused_run_ >= config_.brake_from;
        }

        // Unused links in a row since a link last served, whatever the
        // forgetting of the count.
        int unused_run() const { return unused_run_; }

        // Within the fast phase that follows the last unused link.
        bool fast_after_strike(int64_t now) const {
            return sets_rhythm() && last_strike_at_ >= 0 && now >= last_strike_at_ &&
                   now - last_strike_at_ < config_.fast_seconds;
        }

        // Links that had served have been dropping in a run: the advert goes
        // at its slowest interval, with no fast phase.
        bool churning(int64_t now) const { return config_.enabled && now < churn_until_; }

        // The LE link went down at `le_down_since` after two strikes or
        // more, none of them made up for since: the phone may have given up.
        bool lost_after_strikes(int64_t le_down_since) const {
            return config_.enabled && gave_up_at_ >= 0 && le_down_since >= 0 &&
                   le_down_since - gave_up_at_ <= LE_STRIKE_BLAME_SECONDS;
        }

        // bluetoothd restarted, the computer woke: counts start again.
        void reset(uint64_t drop_count) {
            const LeStrikesConfig config = config_;
            *this = LeStrikes{};
            config_ = config;
            primed_ = true;
            drops_seen_ = drop_count;
        }

    private:
        std::optional<Event> served_loss(int64_t now) {
            // The last LE_CHURN_LOSSES + 1 losses, oldest first.
            for (size_t i = 0; i + 1 < served_losses_.size(); ++i)
                served_losses_[i] = served_losses_[i + 1];
            served_losses_.back() = now;
            ++served_loss_count_;
            const int64_t oldest = served_losses_.front();
            if (served_loss_count_ <= LE_CHURN_LOSSES || now - oldest > LE_CHURN_WINDOW_SECONDS)
                return std::nullopt;
            const bool already = now < churn_until_;
            churn_until_ = now + LE_CHURN_WINDOW_SECONDS;
            if (already)
                return std::nullopt;
            Event event{Event::Kind::Churn, strikes_, LE_CHURN_LOSSES + 1};
            event.pause_seconds = 0;
            return event;
        }

        int pause_for(int strikes) const {
            if (strikes < LE_STRIKES_SLOW)
                return config_.first_pause_seconds;
            int pause = config_.long_pause_seconds;
            const int ceiling = std::max(LE_STRIKE_PAUSE_CEILING_SECONDS, config_.long_pause_seconds);
            for (int i = LE_STRIKES_SLOW; i < strikes && pause < ceiling; ++i)
                pause *= 2;
            return std::min(pause, ceiling);
        }

        Event strike(int64_t now, int links) {
            strikes_ = std::min(strikes_ + links, 99);
            last_strike_at_ = now;
            served_since_strike_ = false;
            const bool was_braked = braked();
            unused_run_ = std::min(unused_run_ + links, 999);
            if (strikes_ >= LE_STRIKES_SLOW)
                gave_up_at_ = now;
            Event event{Event::Kind::Strike, strikes_, links};
            event.braked = !was_braked && braked();
            event.pause_seconds = pause_for(strikes_);
            if (event.pause_seconds > 0)
                pause_until_ = std::max(pause_until_, now + event.pause_seconds);
            return event;
        }

        void clear() {
            strikes_ = 0;
            pause_until_ = -1;
            gave_up_at_ = -1;
            served_since_strike_ = false;
            unused_run_ = 0;
        }

        LeStrikesConfig config_;
        bool primed_ = false;
        uint64_t drops_seen_ = 0;
        bool le_was_up_ = false;
        int64_t last_drop_at_ = -1;

        // The service has been seen live on the link that is up now.
        bool served_link_ = false;
        int64_t live_since_ = -1;

        std::array<int64_t, LE_CHURN_LOSSES + 1> served_losses_{};
        int served_loss_count_ = 0;
        int64_t churn_until_ = -1;

        int strikes_ = 0;
        int64_t last_strike_at_ = -1;
        int64_t pause_until_ = -1;
        int64_t gave_up_at_ = -1;
        // A link has served since the last strike (#40).
        bool served_since_strike_ = false;
        // Unused links in a row, none served in between (#41).
        int unused_run_ = 0;
    };

    // The interval of the advert, strikes and churn taken into account; the
    // arguments after `now` are those of advert_interval_for().
    //   - links that served keep dropping (Pebble), or LANDLINE_LE_STRIKE_SLOW
    //     reached: the slowest interval, 1022.5 ms;
    //   - LE_STRIKE_BRAKE_FROM unused links in a row or more, none served in
    //     between: 152.5 ms, no fast phase of any kind (#41);
    //   - links ending unused, none served since: 20 ms for the fast phase
    //     after the last of them, then 152.5 ms, or 20 ms while BR/EDR has
    //     just reached the phone without LE. The fast windows that follow
    //     any loss of the LE link, or of every link, are not given: each
    //     unused link is such a loss, and they would keep it at 20 ms;
    //   - otherwise, a link having served since the last strike included
    //     (#40), the usual rhythm.
    // A `fast_seconds` of 0 (no fast advert at all) holds here as well.
    inline AdvertInterval advert_interval_with_strikes(const LeStrikes& strikes,
                                                       int64_t now,
                                                       int64_t le_down_since,
                                                       int fast_seconds = ADVERT_FAST_SECONDS,
                                                       int64_t seen_without_le_since = -1,
                                                       int64_t rearmed_at = -1) {
        if (strikes.slow_advert() || strikes.churning(now))
            return ADVERT_INTERVAL_SLOW;
        if (strikes.braked())
            return ADVERT_INTERVAL_STEADY;
        if (strikes.sets_rhythm()) {
            if (fast_seconds > 0 && strikes.fast_after_strike(now))
                return ADVERT_INTERVAL_FAST;
            return advert_interval_for(now, -1, fast_seconds, seen_without_le_since, -1);
        }
        return advert_interval_for(now, le_down_since, fast_seconds, seen_without_le_since, rearmed_at);
    }

} // namespace tether::bluetooth
