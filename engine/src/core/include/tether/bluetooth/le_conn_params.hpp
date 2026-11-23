#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

// The connection parameters of the LE link, asked of the iPhone.
//
// An iPhone opens its LE links with an interval of 22.5 to 30 ms and a
// supervision timeout of 720 ms, and keeps that timeout unless the
// accessory asks for another. The link carrying the notifications died of
// any radio gap of three quarters of a second: the edge of range, or this
// computer's own controller paging the phone for the classic link.
//
// Tried for real on 17 November 2026 (docs/moteur-reconnexion.md, section
// 15): the HCI command LE Connection Update sent by this computer on the LE
// link was taken by the controller and by the iPhone, which applied exactly
// what was asked (60 ms, latency 0, 6 s). Before, links died within one or
// two seconds; after, one held for most of an hour. Two facts of that trial
// shape what is below: a link died 1.05 s after it came up, just before the
// request made one second in could apply, so the request is made as early as
// possible; and on a later link the iPhone renegotiated on its own (45 ms,
// 720 ms kept), so the request is made again on every new link, even one
// that reuses the handle of the last.
//
// The values are Pebble's (PebbleOS, gap_le_connect_params.c, October 2026)
// with no peripheral latency: 45 to 60 ms, latency 0, supervision 6 s.
// Apple's rules for such a request are the ones checked below (Accessory
// Design Guidelines R32 Update 1, 57.6).
//
// Neither BlueZ nor the kernel makes the request for a link the phone
// opened, and nothing on D-Bus, on an L2CAP socket or on the management
// socket lets an ordinary process make it. The one way is the HCI command on
// a raw HCI socket, which the kernel only takes from a process holding
// CAP_NET_RAW (le_conn_update.hpp): in practice the helper
// landline-le-params, which the system assistant installs with that one
// capability. On by default; without the helper, said once and nothing is
// asked.
//
// Everything in this header is a decision on facts handed in.
namespace tether::bluetooth {

    struct LeConnParams {
        int min_interval_ms = 45;
        int max_interval_ms = 60;
        // Connection events the peripheral may skip.
        int latency = 0;
        int timeout_ms = 6000;

        bool operator==(const LeConnParams&) const = default;
    };

    // Seconds into a link before the request. None: the phone's 720 ms is
    // what kills young links, and Pebble went from 5 s to 1 s for that
    // reason (commit dca1f76, July 2026). The pause of the specification
    // (TGAP(conn_pause_peripheral)) is waived by Apple for a stack that
    // never has two requests outstanding.
    inline constexpr int LE_CONN_PARAMS_DELAY_SECONDS = 0;
    inline constexpr int LE_CONN_PARAMS_DELAY_SECONDS_MAX = 600;
    // Requests per link that are refused or go unanswered, and how far
    // apart (Pebble: 3, 5 s). Nothing is ever ended when they run out.
    inline constexpr int LE_CONN_PARAMS_ATTEMPTS = 3;
    inline constexpr int LE_CONN_PARAMS_RETRY_SECONDS = 5;
    // A refusal for a collision with a procedure of the phone's own is not
    // an attempt: asked again 2 s later, ten times at most (Pebble, commit
    // b32ddd4: iOS updating its channel map).
    inline constexpr int LE_CONN_PARAMS_COLLISION_RETRY_SECONDS = 2;
    inline constexpr int LE_CONN_PARAMS_COLLISIONS = 10;
    // The phone renegotiated a shorter supervision timeout on a link that
    // had the one asked for: asked again, at most this often.
    inline constexpr int LE_CONN_PARAMS_REASK_SECONDS = 60;
    // How long the controller is given to report what the phone decided.
    inline constexpr int LE_CONN_PARAMS_VERDICT_MS = 8000;
    // How long a new link is given to be encrypted before the request
    // (Pebble asks nothing of a link that is not). An iPhone that holds the
    // bond encrypts within a second (0.92 s on 17 November).
    inline constexpr int LE_CONN_PARAMS_ENCRYPTION_WAIT_MS = 5000;
    // How long the parameters of a link are watched after a request, for
    // the phone changing them. Past it the request is made once more.
    inline constexpr int LE_CONN_PARAMS_WATCH_SECONDS = 6 * 3600;

    // HCI statuses of a collision between two link layer procedures.
    inline constexpr int HCI_STATUS_LL_COLLISION = 0x23;
    inline constexpr int HCI_STATUS_DIFFERENT_COLLISION = 0x2a;
    inline bool le_conn_collision(int status) {
        return status == HCI_STATUS_LL_COLLISION || status == HCI_STATUS_DIFFERENT_COLLISION;
    }

    // Why Apple, or the controller, would not take these; empty when they
    // would. An iPhone may still answer with other values of its own.
    inline std::string le_conn_params_fault(const LeConnParams& p) {
        if (p.min_interval_ms < 15)
            return "the minimum interval is under 15 ms";
        if (p.min_interval_ms > 2000)
            return "the minimum interval is over 2 s";
        if (p.min_interval_ms % 15 != 0)
            return "the minimum interval is not a multiple of 15 ms";
        if (!(p.max_interval_ms >= p.min_interval_ms + 15 || (p.min_interval_ms == 15 && p.max_interval_ms == 15)))
            return "the maximum interval is not at least 15 ms above the minimum";
        // The controller's own range, 7.5 ms to 4 s.
        if (p.max_interval_ms > 4000)
            return "the maximum interval is over 4 s";
        if (p.latency < 0 || p.latency > 30)
            return "the latency is not between 0 and 30";
        if (p.timeout_ms < 6000 || p.timeout_ms > 18000)
            return "the supervision timeout is not between 6 and 18 s";
        if (static_cast<int64_t>(p.max_interval_ms) * (p.latency + 1) > 6000)
            return "maximum interval x (latency + 1) is over 6 s";
        if (p.timeout_ms <= static_cast<int64_t>(p.max_interval_ms) * (p.latency + 1) * 3)
            return "the supervision timeout is not over maximum interval x (latency + 1) x 3";
        return {};
    }

    struct LeConnParamsConfig {
        bool enabled = true;
        LeConnParams wanted;
        int delay_seconds = LE_CONN_PARAMS_DELAY_SECONDS;
        // Wait for the link to be encrypted before the request.
        bool wait_encryption = true;
        // Why a setting was put aside for the default; empty when none was.
        std::string note;
    };

    // LANDLINE_LE_CONN_PARAMS=0 turns it off. LANDLINE_LE_CONN_INTERVAL_MS
    // ("45-60"), LANDLINE_LE_CONN_LATENCY, LANDLINE_LE_CONN_TIMEOUT_MS,
    // LANDLINE_LE_CONN_DELAY_SECONDS (0 to 600),
    // LANDLINE_LE_CONN_ENCRYPTED=0 (ask before the link is encrypted). A set
    // Apple's rules refuse is not sent: the defaults are, and `note` says
    // why.
    inline LeConnParamsConfig le_conn_params_config_from(const char* enabled,
                                                         const char* interval_ms,
                                                         const char* latency,
                                                         const char* timeout_ms,
                                                         const char* delay_seconds,
                                                         const char* encrypted = nullptr) {
        const auto number = [](std::string_view text) -> std::optional<int> {
            if (text.empty() || text.size() > 6)
                return std::nullopt;
            int value = 0;
            for (char c : text) {
                if (c < '0' || c > '9')
                    return std::nullopt;
                value = value * 10 + (c - '0');
            }
            return value;
        };
        LeConnParamsConfig config;
        config.enabled = !enabled || std::string_view(enabled) != "0";
        config.wait_encryption = !encrypted || std::string_view(encrypted) != "0";
        LeConnParams asked;
        bool bad = false;
        if (interval_ms && *interval_ms) {
            const std::string_view text(interval_ms);
            const size_t dash = text.find('-');
            const auto min = number(text.substr(0, dash));
            const auto max = dash == std::string_view::npos ? std::nullopt : number(text.substr(dash + 1));
            if (min && max) {
                asked.min_interval_ms = *min;
                asked.max_interval_ms = *max;
            } else {
                bad = true;
            }
        }
        if (latency && *latency) {
            if (const auto value = number(latency))
                asked.latency = *value;
            else
                bad = true;
        }
        if (timeout_ms && *timeout_ms) {
            if (const auto value = number(timeout_ms))
                asked.timeout_ms = *value;
            else
                bad = true;
        }
        const std::string fault = bad ? std::string("a value is not a number") : le_conn_params_fault(asked);
        if (fault.empty())
            config.wanted = asked;
        else
            config.note = fault;
        if (delay_seconds && *delay_seconds) {
            const auto value = number(delay_seconds);
            if (value && *value <= LE_CONN_PARAMS_DELAY_SECONDS_MAX)
                config.delay_seconds = *value;
        }
        return config;
    }

    // "hci0" -> 0; nothing for anything else.
    inline std::optional<int> hci_index_from(std::string_view adapter_id) {
        if (adapter_id.size() < 4 || adapter_id.size() > 6 || adapter_id.substr(0, 3) != "hci")
            return std::nullopt;
        int value = 0;
        for (char c : adapter_id.substr(3)) {
            if (c < '0' || c > '9')
                return std::nullopt;
            value = value * 10 + (c - '0');
        }
        return value;
    }

    // --- The HCI side, as bytes -------------------------------------------

    inline constexpr uint16_t HCI_OP_LE_CONN_UPDATE = 0x2013; // OGF 0x08, OCF 0x0013
    inline constexpr uint8_t HCI_PKT_COMMAND = 0x01;
    inline constexpr uint8_t HCI_PKT_EVENT = 0x04;
    inline constexpr uint8_t HCI_EVT_DISCONN_COMPLETE = 0x05;
    inline constexpr uint8_t HCI_EVT_CMD_STATUS = 0x0f;
    inline constexpr uint8_t HCI_EVT_LE_META = 0x3e;
    inline constexpr uint8_t HCI_LE_SUBEVT_CONN_UPDATE_COMPLETE = 0x03;

    // Intervals go in units of 1.25 ms, the timeout in units of 10 ms.
    inline uint16_t le_interval_units(int ms) { return static_cast<uint16_t>(ms * 4 / 5); }
    inline int le_interval_ms(uint16_t units) { return units * 5 / 4; }
    inline uint16_t le_timeout_units(int ms) { return static_cast<uint16_t>(ms / 10); }

    // The packet written to a raw HCI socket: type, opcode, length, then
    // handle, interval min and max, latency, timeout and two event lengths
    // left at zero, all little-endian.
    inline std::array<uint8_t, 18> le_conn_update_command(uint16_t handle, const LeConnParams& p) {
        std::array<uint8_t, 18> out{};
        const auto put = [&out](size_t at, uint16_t value) {
            out[at] = static_cast<uint8_t>(value & 0xff);
            out[at + 1] = static_cast<uint8_t>(value >> 8);
        };
        out[0] = HCI_PKT_COMMAND;
        put(1, HCI_OP_LE_CONN_UPDATE);
        out[3] = 14;
        put(4, handle);
        put(6, le_interval_units(p.min_interval_ms));
        put(8, le_interval_units(p.max_interval_ms));
        put(10, static_cast<uint16_t>(p.latency));
        put(12, le_timeout_units(p.timeout_ms));
        return out;
    }

    struct LeConnUpdateEvent {
        enum class Kind {
            // The controller took the command, or refused it (status).
            CommandStatus,
            // A procedure ended, ours or the phone's: status, and the
            // parameters now in force.
            UpdateComplete,
            // The link ended (status: the reason).
            Disconnected,
        } kind;
        uint8_t status = 0;
        LeConnParams now;
    };

    // What one packet read from the raw socket says about the link `handle`
    // or a request made on it; nothing when it is about something else.
    inline std::optional<LeConnUpdateEvent> parse_le_conn_update_event(const uint8_t* data, size_t length, uint16_t handle) {
        if (!data || length < 3 || data[0] != HCI_PKT_EVENT)
            return std::nullopt;
        const uint8_t code = data[1];
        const size_t plen = data[2];
        if (length < 3 + plen)
            return std::nullopt;
        const uint8_t* p = data + 3;
        const auto u16 = [](const uint8_t* at) { return static_cast<uint16_t>(at[0] | (at[1] << 8)); };
        if (code == HCI_EVT_CMD_STATUS && plen >= 4) {
            if (u16(p + 2) != HCI_OP_LE_CONN_UPDATE)
                return std::nullopt;
            return LeConnUpdateEvent{LeConnUpdateEvent::Kind::CommandStatus, p[0], {}};
        }
        if (code == HCI_EVT_LE_META && plen >= 10 && p[0] == HCI_LE_SUBEVT_CONN_UPDATE_COMPLETE) {
            if ((u16(p + 2) & 0x0fff) != handle)
                return std::nullopt;
            LeConnUpdateEvent event{LeConnUpdateEvent::Kind::UpdateComplete, p[1], {}};
            const int interval = le_interval_ms(u16(p + 4));
            event.now.min_interval_ms = interval;
            event.now.max_interval_ms = interval;
            event.now.latency = u16(p + 6);
            event.now.timeout_ms = u16(p + 8) * 10;
            return event;
        }
        if (code == HCI_EVT_DISCONN_COMPLETE && plen >= 4) {
            // A failed Disconnect command (status not 0) ends nothing.
            if (p[0] != 0 || (u16(p + 1) & 0x0fff) != handle)
                return std::nullopt;
            return LeConnUpdateEvent{LeConnUpdateEvent::Kind::Disconnected, p[3], {}};
        }
        return std::nullopt;
    }

    // --- What a request came to -------------------------------------------

    enum class LeConnUpdateOutcome {
        // The phone took parameters; `got` says which.
        Applied,
        // The controller or the phone said no (status).
        Refused,
        // This process may not send the command and no helper is installed.
        NoRight,
        // No LE link to the phone was found on the controller.
        NoLink,
        // The link was not encrypted in time; nothing was sent.
        NotReady,
        // The controller took the command and reported nothing in time.
        NoVerdict,
        // Anything else: a socket, the helper.
        Failed,
        // While the link is watched after a request: its parameters changed
        // (`got`), on the phone's initiative or another's.
        Changed,
        // While the link is watched: it ended (status: the HCI reason).
        Ended,
    };

    inline const char* to_string(LeConnUpdateOutcome outcome) {
        switch (outcome) {
        case LeConnUpdateOutcome::Applied:
            return "applied";
        case LeConnUpdateOutcome::Refused:
            return "refused";
        case LeConnUpdateOutcome::NoRight:
            return "no_right";
        case LeConnUpdateOutcome::NoLink:
            return "no_link";
        case LeConnUpdateOutcome::NotReady:
            return "not_ready";
        case LeConnUpdateOutcome::NoVerdict:
            return "no_verdict";
        case LeConnUpdateOutcome::Failed:
            return "failed";
        case LeConnUpdateOutcome::Changed:
            return "changed";
        case LeConnUpdateOutcome::Ended:
            return "ended";
        }
        return "failed";
    }

    struct LeConnUpdateResult {
        LeConnUpdateOutcome outcome = LeConnUpdateOutcome::Failed;
        // Applied, Changed: the parameters in force (one interval, so
        // min == max).
        LeConnParams got;
        // The HCI status of a refusal, the reason of an end; -1 when there
        // is none.
        int status = -1;
        int handle = -1;
        std::string detail;

        bool operator==(const LeConnUpdateResult&) const = default;
    };

    // The lines the helper prints, and their reading. Keys and values hold
    // no spaces; `detail` comes last and runs to the end of the line.
    inline std::string format_le_conn_update_result(const LeConnUpdateResult& r) {
        std::string out = "outcome=";
        out += to_string(r.outcome);
        out += " status=" + std::to_string(r.status) + " handle=" + std::to_string(r.handle);
        if (r.outcome == LeConnUpdateOutcome::Applied || r.outcome == LeConnUpdateOutcome::Changed)
            out += " interval_ms=" + std::to_string(r.got.max_interval_ms) + " latency=" + std::to_string(r.got.latency) +
                   " timeout_ms=" + std::to_string(r.got.timeout_ms);
        if (!r.detail.empty())
            out += " detail=" + r.detail;
        return out;
    }

    inline LeConnUpdateResult parse_le_conn_update_result(std::string_view line) {
        LeConnUpdateResult result;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);
        bool outcome_seen = false;
        while (!line.empty()) {
            const size_t eq = line.find('=');
            if (eq == std::string_view::npos)
                break;
            const std::string_view key = line.substr(0, eq);
            line.remove_prefix(eq + 1);
            if (key == "detail") {
                result.detail = std::string(line);
                break;
            }
            const size_t space = line.find(' ');
            const std::string value(line.substr(0, space));
            line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
            if (key == "outcome") {
                for (LeConnUpdateOutcome o : {LeConnUpdateOutcome::Applied,
                                              LeConnUpdateOutcome::Refused,
                                              LeConnUpdateOutcome::NoRight,
                                              LeConnUpdateOutcome::NoLink,
                                              LeConnUpdateOutcome::NotReady,
                                              LeConnUpdateOutcome::NoVerdict,
                                              LeConnUpdateOutcome::Failed,
                                              LeConnUpdateOutcome::Changed,
                                              LeConnUpdateOutcome::Ended})
                    if (value == to_string(o)) {
                        result.outcome = o;
                        outcome_seen = true;
                    }
            } else if (key == "status") {
                result.status = std::atoi(value.c_str());
            } else if (key == "handle") {
                result.handle = std::atoi(value.c_str());
            } else if (key == "interval_ms") {
                result.got.min_interval_ms = result.got.max_interval_ms = std::atoi(value.c_str());
            } else if (key == "latency") {
                result.got.latency = std::atoi(value.c_str());
            } else if (key == "timeout_ms") {
                result.got.timeout_ms = std::atoi(value.c_str());
            }
        }
        if (!outcome_seen) {
            result = LeConnUpdateResult{};
            result.detail = "the helper printed nothing that could be read";
        }
        return result;
    }

    // --- When to ask --------------------------------------------------------

    struct LeConnParamsFacts {
        int64_t now = 0;
        bool le_connected = false;
        // The monitor's count of LE links seen coming up: another value is
        // another link, whatever its handle.
        uint64_t link = 0;
        // A request is on its way.
        bool busy = false;
    };

    class LeConnParamsPlan {
    public:
        void configure(const LeConnParamsConfig& config) { config_ = config; }
        const LeConnParamsConfig& config() const { return config_; }

        // Once per pass. True: make the request now, then report().
        bool due(const LeConnParamsFacts& facts) {
            if (facts.link != link_ || !facts.le_connected || up_since_ < 0) {
                // Another link, or none: whatever was obtained went with the
                // old one.
                if (facts.link != link_ || !facts.le_connected) {
                    attempts_ = 0;
                    collisions_ = 0;
                    settled_ = false;
                    gave_up_ = false;
                    next_at_ = -1;
                    last_ask_at_ = -1;
                    in_force_.reset();
                }
                link_ = facts.link;
                connected_ = facts.le_connected;
                up_since_ = facts.le_connected ? facts.now : -1;
                if (!facts.le_connected)
                    return false;
            }
            connected_ = true;
            if (!config_.enabled || stopped_ || settled_ || gave_up_ || facts.busy)
                return false;
            if (facts.now - up_since_ < config_.delay_seconds)
                return false;
            if (next_at_ >= 0 && facts.now < next_at_)
                return false;
            ++attempts_;
            last_ask_at_ = facts.now;
            next_at_ = facts.now + LE_CONN_PARAMS_RETRY_SECONDS;
            return true;
        }

        // What the last request came to. True when nothing more will be
        // asked on this link (or at all): the caller says so, once.
        bool report(int64_t now, const LeConnUpdateResult& result) {
            switch (result.outcome) {
            case LeConnUpdateOutcome::Applied:
                in_force_ = result.got;
                // An iPhone may answer with a set of its own. What matters
                // is the supervision timeout: anything short of the one
                // asked for is asked again.
                settled_ = result.got.timeout_ms >= config_.wanted.timeout_ms;
                // The request went through: no collision is pending any more.
                collisions_ = 0;
                if (settled_)
                    return true;
                break;
            case LeConnUpdateOutcome::NoRight:
                // Not a fault and not going to change while this runs.
                stopped_ = true;
                return true;
            case LeConnUpdateOutcome::Refused:
                if (le_conn_collision(result.status)) {
                    // The phone was busy with a procedure of its own: not an
                    // attempt, asked again soon.
                    if (++collisions_ > LE_CONN_PARAMS_COLLISIONS) {
                        gave_up_ = true;
                        return true;
                    }
                    if (attempts_ > 0)
                        --attempts_;
                    next_at_ = now + LE_CONN_PARAMS_COLLISION_RETRY_SECONDS;
                    return false;
                }
                break;
            case LeConnUpdateOutcome::NoLink:
            case LeConnUpdateOutcome::NotReady:
            case LeConnUpdateOutcome::NoVerdict:
            case LeConnUpdateOutcome::Failed:
                break;
            case LeConnUpdateOutcome::Changed:
                changed(now, result.got);
                return false;
            case LeConnUpdateOutcome::Ended:
                return false;
            }
            // An answer that is not a collision ends a run of them.
            collisions_ = 0;
            next_at_ = now + LE_CONN_PARAMS_RETRY_SECONDS;
            gave_up_ = attempts_ >= LE_CONN_PARAMS_ATTEMPTS;
            return gave_up_;
        }

        // The parameters of the link changed after a request: the phone
        // renegotiated. A supervision timeout short of the one wanted is
        // asked for again, at most once a minute, with a fresh count.
        void changed(int64_t now, const LeConnParams& now_in_force) {
            in_force_ = now_in_force;
            if (now_in_force.timeout_ms >= config_.wanted.timeout_ms) {
                settled_ = true;
                return;
            }
            if (!config_.enabled || stopped_)
                return;
            // The iPhone's own procedure that a request just collided with
            // ends here: the retry already set for two seconds stands, not
            // the minute that spaces out re-asking a link that renegotiates
            // (18 November, 03:08: a minute on 720 ms right after a wake).
            const bool collision_retry = collisions_ > 0 && !gave_up_ && next_at_ > now;
            const int64_t pending = next_at_;
            settled_ = false;
            gave_up_ = false;
            attempts_ = 0;
            next_at_ = last_ask_at_ < 0 ? now : std::max(now, last_ask_at_ + LE_CONN_PARAMS_REASK_SECONDS);
            if (collision_retry)
                next_at_ = std::min(next_at_, std::max(now, pending));
            else
                collisions_ = 0;
        }

        // The watch on a settled link ran out (LE_CONN_PARAMS_WATCH_SECONDS):
        // asked once more, which watches it again.
        void watch_ended(int64_t now) {
            if (!settled_ || !connected_)
                return;
            settled_ = false;
            attempts_ = 0;
            collisions_ = 0;
            next_at_ = now;
        }

        // The parameters asked for are in force on the link that is up.
        bool settled() const { return settled_; }
        // No right to ask: nothing is tried again by this process.
        bool stopped() const { return stopped_; }
        // Nothing more is asked on this link.
        bool gave_up() const { return gave_up_; }
        int attempts() const { return attempts_; }
        int collisions() const { return collisions_; }
        // What the controller last reported in force on this link.
        const std::optional<LeConnParams>& in_force() const { return in_force_; }

        // What is published: "off", "no_right", "waiting" (no LE link),
        // "pending", "applied" or "refused".
        const char* state() const {
            if (!config_.enabled)
                return "off";
            if (stopped_)
                return "no_right";
            if (!connected_)
                return "waiting";
            if (settled_)
                return "applied";
            if (gave_up_)
                return "refused";
            return "pending";
        }

    private:
        LeConnParamsConfig config_;
        uint64_t link_ = 0;
        bool connected_ = false;
        int64_t up_since_ = -1;
        int attempts_ = 0;
        int collisions_ = 0;
        int64_t next_at_ = -1;
        int64_t last_ask_at_ = -1;
        bool settled_ = false;
        bool gave_up_ = false;
        bool stopped_ = false;
        std::optional<LeConnParams> in_force_;
    };

} // namespace tether::bluetooth
