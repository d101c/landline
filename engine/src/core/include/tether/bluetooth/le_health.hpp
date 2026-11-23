#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

// How well the LE link is holding, and what follows from it.
//
// 17 November 2026, 18:05: the iPhone came home, its LE link came up and
// died about twenty times in eighty seconds at the edge of range, the
// classic link was opened four seconds into the first link that carried
// notifications, the LE link died 0.72 s later (the supervision timeout iOS
// gives its links, while the controller was paging), and after two more
// short links the iPhone stopped answering the advert altogether. The same
// had happened the evening before.
//
// Three decisions on facts handed in, nothing here touches BlueZ:
//   - the classic link is only opened of the engine's own accord on an LE
//     link that has held for a while;
//   - an LE link lost right after the classic link was opened or closed
//     keeps the engine from opening it again for minutes;
//   - LE links dying in a row take the advert off air for a moment, so that
//     the iPhone is not offered link after link that will not last.
namespace tether::bluetooth {

    // An LE link this old with no drop is one the classic link may be opened
    // on. The links of the two bursts of 17 November lived 1 to 8 s.
    inline constexpr int LE_STABLE_SECONDS = 45;
    inline constexpr int LE_STABLE_SECONDS_MAX = 3600;
    // An LE drop this soon after the classic link was touched is put down to it.
    inline constexpr int CLASSIC_BLAME_SECONDS = 10;
    // No opening nobody asked for, for this long after such a drop; doubled
    // each time it happens again within the hour.
    inline constexpr int CLASSIC_QUIET_SECONDS = 300;
    inline constexpr int CLASSIC_QUIET_SECONDS_MAX = 1800;
    inline constexpr int CLASSIC_BLAME_MEMORY_SECONDS = 3600;
    // This many LE drops within the window is a burst.
    inline constexpr int LE_BURST_DROPS = 3;
    inline constexpr int LE_BURST_WINDOW_SECONDS = 60;
    // The advert is withheld this long after a burst, doubled for each burst
    // that follows before the link has held.
    inline constexpr int LE_BURST_PAUSE_SECONDS = 30;
    inline constexpr int LE_BURST_PAUSE_SECONDS_MAX = 120;
    // A burst this recent before the LE link went down for good is named as
    // the likely cause of the phone's silence.
    inline constexpr int LE_BURST_MEMORY_SECONDS = 120;

    struct LeHealthConfig {
        // 0: the classic link is opened whatever the age of the LE link.
        int stable_seconds = LE_STABLE_SECONDS;
        // 0: an LE drop after an opening or a close changes nothing.
        int quiet_seconds = CLASSIC_QUIET_SECONDS;
        // false: the advert stays on air through a burst.
        bool burst_pause = true;
    };

    // LANDLINE_LE_STABLE_SECONDS (0 to 3600, 0 = off),
    // LANDLINE_CLASSIC_QUIET_SECONDS (0 to 1800, 0 = off),
    // LANDLINE_LE_BURST_PAUSE=0.
    inline LeHealthConfig
        le_health_config_from(const char* stable_seconds, const char* quiet_seconds, const char* burst_pause) {
        const auto number = [](const char* text, int fallback, int max) {
            if (!text || !*text)
                return fallback;
            int value = 0;
            for (const char* c = text; *c; ++c) {
                if (*c < '0' || *c > '9' || value > max)
                    return fallback;
                value = value * 10 + (*c - '0');
            }
            return value <= max ? value : fallback;
        };
        LeHealthConfig config;
        config.stable_seconds = number(stable_seconds, LE_STABLE_SECONDS, LE_STABLE_SECONDS_MAX);
        config.quiet_seconds = number(quiet_seconds, CLASSIC_QUIET_SECONDS, CLASSIC_QUIET_SECONDS_MAX);
        config.burst_pause = !burst_pause || std::string_view(burst_pause) != "0";
        return config;
    }

    enum class ClassicTouch { None, Opened, Closed };

    class LeHealth {
    public:
        // Something to write in the journal, once.
        struct Event {
            enum class Kind { ClassicBlamed, Burst, Steady } kind;
            // ClassicBlamed: what was done to the classic link, how long before
            // the drop, and for how long nothing is opened unasked.
            ClassicTouch touch = ClassicTouch::None;
            int after_seconds = 0;
            int quiet_seconds = 0;
            // Burst: how many drops, and for how long the advert is withheld
            // (0 when it is left on air).
            int drops = 0;
            int pause_seconds = 0;
        };

        void configure(const LeHealthConfig& config) { config_ = config; }
        const LeHealthConfig& config() const { return config_; }

        // The classic link has just been asked for, or asked to close.
        void note_classic(ClassicTouch touch, int64_t now) {
            if (touch == ClassicTouch::None)
                return;
            touch_ = touch;
            touched_at_ = now;
        }

        // Once per pass. `drop_count` is the monitor's count of Disconnected
        // signals on the LE bearer: only its change matters, and it sees the
        // links that came and went between two passes.
        std::optional<Event> observe(int64_t now, bool le_connected, bool classic_connected, uint64_t drop_count) {
            std::optional<Event> event;
            if (classic_connected)
                classic_seen_at_ = now;

            uint64_t fresh = 0;
            if (!primed_) {
                primed_ = true;
            } else if (drop_count > drops_seen_) {
                fresh = drop_count - drops_seen_;
            } else if (le_was_up_ && !le_connected && (last_drop_at_ < 0 || now - last_drop_at_ > 2)) {
                // A stack that sends no signal: the edge is all there is. Not
                // when the signal has just been counted and the property follows.
                fresh = 1;
            }
            drops_seen_ = drop_count;

            if (fresh > 0) {
                last_drop_at_ = now;
                up_since_ = -1;
                steady_said_ = false;
                for (uint64_t i = 0; i < std::min<uint64_t>(fresh, LE_BURST_DROPS); ++i)
                    recent_.push_back(now);
                event = blame(now);
                if (!event)
                    event = burst(now);
            }
            while (!recent_.empty() && now - recent_.front() >= LE_BURST_WINDOW_SECONDS)
                recent_.pop_front();

            if (le_connected) {
                if (up_since_ < 0)
                    up_since_ = now;
                // The link has held: what the bursts built up is forgotten.
                if (burst_level_ > 0 && now - up_since_ >= steady_after() && !event) {
                    burst_level_ = 0;
                    pause_until_ = -1;
                    if (!steady_said_)
                        event = Event{Event::Kind::Steady};
                    steady_said_ = true;
                }
            } else {
                up_since_ = -1;
            }
            le_was_up_ = le_connected;
            if (blamed_at_ >= 0 && now - blamed_at_ >= CLASSIC_BLAME_MEMORY_SECONDS) {
                blamed_at_ = -1;
                blames_ = 0;
            }
            return event;
        }

        // The LE link is up and has been, without a drop, for long enough.
        bool stable(int64_t now) const {
            if (config_.stable_seconds <= 0)
                return up_since_ >= 0;
            return up_since_ >= 0 && now - up_since_ >= config_.stable_seconds;
        }

        // Whether the classic link may be opened without anyone having asked.
        bool auto_open_allowed(int64_t now) const { return stable(now) && now >= quiet_until_; }

        // When openings nobody asked for are allowed again after a drop put
        // down to the classic link; -1 outside such a wait.
        int64_t quiet_until(int64_t now) const { return now < quiet_until_ ? quiet_until_ : -1; }

        // Whether the advert is withheld: a burst has just been seen.
        bool advert_paused(int64_t now) const { return config_.burst_pause && now < pause_until_; }

        // LE links are dying in a row and the last word is not in.
        bool unsteady(int64_t now) const { return burst_level_ > 0 || now < pause_until_; }

        // The LE link went down at `le_down_since` on the heels of a burst.
        bool lost_after_burst(int64_t le_down_since) const {
            return last_burst_at_ >= 0 && le_down_since >= 0 && le_down_since - last_burst_at_ <= LE_BURST_MEMORY_SECONDS &&
                   last_burst_at_ - le_down_since <= LE_BURST_MEMORY_SECONDS;
        }

        // The classic link reached the phone at or after `since`: it was there.
        bool classic_seen_since(int64_t since) const { return classic_seen_at_ >= 0 && since >= 0 && classic_seen_at_ >= since; }

        // bluetoothd restarted, the computer woke: counts start again.
        void reset(uint64_t drop_count) {
            const LeHealthConfig config = config_;
            *this = LeHealth{};
            config_ = config;
            primed_ = true;
            drops_seen_ = drop_count;
        }

    private:
        int steady_after() const { return config_.stable_seconds > 0 ? config_.stable_seconds : LE_STABLE_SECONDS; }

        std::optional<Event> blame(int64_t now) {
            if (config_.quiet_seconds <= 0 || touch_ == ClassicTouch::None || touched_at_ < 0 ||
                now - touched_at_ > CLASSIC_BLAME_SECONDS)
                return std::nullopt;
            int quiet = config_.quiet_seconds;
            for (int i = 0; i < blames_ && quiet < CLASSIC_QUIET_SECONDS_MAX; ++i)
                quiet *= 2;
            quiet = std::min(quiet, std::max(CLASSIC_QUIET_SECONDS_MAX, config_.quiet_seconds));
            ++blames_;
            blamed_at_ = now;
            quiet_until_ = std::max(quiet_until_, now + quiet);
            Event event{Event::Kind::ClassicBlamed};
            event.touch = touch_;
            event.after_seconds = static_cast<int>(now - touched_at_);
            event.quiet_seconds = quiet;
            // One drop is blamed once.
            touch_ = ClassicTouch::None;
            return event;
        }

        std::optional<Event> burst(int64_t now) {
            while (!recent_.empty() && now - recent_.front() >= LE_BURST_WINDOW_SECONDS)
                recent_.pop_front();
            if (static_cast<int>(recent_.size()) < LE_BURST_DROPS)
                return std::nullopt;
            Event event{Event::Kind::Burst};
            event.drops = static_cast<int>(recent_.size());
            recent_.clear();
            last_burst_at_ = now;
            int pause = LE_BURST_PAUSE_SECONDS;
            for (int i = 0; i < burst_level_ && pause < LE_BURST_PAUSE_SECONDS_MAX; ++i)
                pause *= 2;
            pause = std::min(pause, LE_BURST_PAUSE_SECONDS_MAX);
            ++burst_level_;
            if (config_.burst_pause) {
                pause_until_ = now + pause;
                event.pause_seconds = pause;
            }
            return event;
        }

        LeHealthConfig config_;
        bool primed_ = false;
        uint64_t drops_seen_ = 0;
        bool le_was_up_ = false;
        int64_t up_since_ = -1;
        int64_t last_drop_at_ = -1;
        std::deque<int64_t> recent_;

        ClassicTouch touch_ = ClassicTouch::None;
        int64_t touched_at_ = -1;
        int blames_ = 0;
        int64_t blamed_at_ = -1;
        int64_t quiet_until_ = -1;

        int burst_level_ = 0;
        int64_t last_burst_at_ = -1;
        int64_t pause_until_ = -1;
        bool steady_said_ = false;

        int64_t classic_seen_at_ = -1;
    };

    inline const char* to_string(ClassicTouch touch) {
        return touch == ClassicTouch::Opened ? "opened" : touch == ClassicTouch::Closed ? "closed" : "left alone";
    }

} // namespace tether::bluetooth
