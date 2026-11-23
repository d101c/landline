#pragma once

#include "tether/bluetooth/le_first.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What 17 November 2026 asked for: the LE link stayed away for hours, the
// computer went to sleep and woke three times meanwhile, and nothing the
// engine published said that notifications were not arriving, or why.
//
// Three things live here, each a decision on facts handed in:
//   - telling that the computer has slept, from two clocks;
//   - telling an unknown LE device that may be the paired phone under an
//     address BlueZ did not resolve;
//   - the state of the notification link as it is published.
namespace tether::bluetooth {

    // --- Sleep -----------------------------------------------------------

    // A suspend shorter than this is not told from scheduling noise.
    inline constexpr int64_t SLEEP_MIN_MS = 3000;

    // The supervising thread runs on a clock that stands still while the
    // computer sleeps (CLOCK_MONOTONIC). CLOCK_BOOTTIME does not. The gap
    // between the two only ever grows, and only across a suspend. No bus, no
    // logind: it also sees a sleep during which no link was up.
    class SleepWatch {
    public:
        // Both in milliseconds, read at the same moment. Returns how long the
        // computer slept since the last call, when it did.
        std::optional<int64_t> observe(int64_t monotonic_ms, int64_t boottime_ms) {
            const int64_t gap = boottime_ms - monotonic_ms;
            if (!primed_) {
                primed_ = true;
                gap_ms_ = gap;
                return std::nullopt;
            }
            const int64_t slept = gap - gap_ms_;
            if (slept < SLEEP_MIN_MS)
                return std::nullopt;
            gap_ms_ = gap;
            return slept;
        }

    private:
        bool primed_ = false;
        int64_t gap_ms_ = 0;
    };

    // TETHER_RESUME_WATCH=0: a wake-up is no longer treated as anything.
    inline bool resume_watch_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // --- An LE device nobody knows ---------------------------------------

    enum class RandomAddressKind { NonResolvable, Resolvable, Static, Reserved };

    // The two top bits of a random address say which kind it is. Meaningless
    // for a public address. Takes "4A:1B:2C:3D:4E:5F" or BlueZ's
    // "/org/bluez/hci0/dev_4A_1B_2C_3D_4E_5F".
    inline std::optional<RandomAddressKind> random_address_kind(std::string_view address) {
        if (const auto at = address.rfind("dev_"); at != std::string_view::npos)
            address.remove_prefix(at + 4);
        if (address.size() < 2)
            return std::nullopt;
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        const int high = nibble(address[0]);
        if (high < 0 || nibble(address[1]) < 0)
            return std::nullopt;
        switch (high >> 2) {
        case 0:
            return RandomAddressKind::NonResolvable;
        case 1:
            return RandomAddressKind::Resolvable;
        case 3:
            return RandomAddressKind::Static;
        default:
            return RandomAddressKind::Reserved;
        }
    }

    // An LE link that ended on a device BlueZ holds no pairing for.
    struct StrangerFacts {
        // Device1.Paired of the device the link was on; false when BlueZ had
        // already removed the object.
        bool paired = false;
        // Device1.AddressType, empty when unknown.
        std::string address_type;
        // Its address, or its object path.
        std::string address;
        // The paired phone, at that moment.
        LeExpectation le;
        bool phone_le_connected = false;
        // The solicitation was registered: this computer was connectable to
        // whoever heard it.
        bool advert_registered = false;
    };

    // Whether that link may have been the paired iPhone, come to answer the
    // advert under a resolvable private address that was not resolved to the
    // pairing. An iPhone always uses such an address on LE. Nothing proves it
    // from here: any device may connect to a connectable advert, and a
    // neighbour's phone looks the same. It is a lead, to be said and counted.
    inline bool may_be_unresolved_phone(const StrangerFacts& facts) {
        if (facts.paired || !facts.le.expected() || facts.phone_le_connected || !facts.advert_registered)
            return false;
        if (!facts.address_type.empty() && facts.address_type != "random")
            return false;
        return random_address_kind(facts.address) == RandomAddressKind::Resolvable;
    }

    // How long such a sighting keeps being named as the likely cause.
    inline constexpr int STRANGER_MEMORY_SECONDS = 900;
    // The advert is registered again after a sighting, but not more often.
    inline constexpr int STRANGER_REARM_SECONDS = 300;

    // TETHER_LE_STRANGER=0: such links are neither reported nor acted on.
    inline bool le_stranger_enabled(const char* env_override) {
        return !env_override || std::string_view(env_override) != "0";
    }

    // --- The published state of the notification link ---------------------

    // How long the LE link may be down before "waiting" becomes "paused".
    // The same threshold as the advice in the link reason.
    inline constexpr int LE_PAUSED_AFTER_SECONDS = 180;

    struct LeStateFacts {
        bool ancs_enabled = false;
        bool device_paired = false;
        bool le_available = false;
        bool le_connected = false;
        bool classic_connected = false;
        bool obex_up = false;
        // The bond is broken, or covers BR/EDR only.
        bool bond_unusable = false;
        bool holding_for_le = false;
        // The solicitation is registered with BlueZ.
        bool advert_registered = false;
        int64_t now = 0;
        // Since when the LE link has been down, negative while it is up.
        int64_t le_down_since = -1;
        // When the computer last woke up, negative for never.
        int64_t resumed_at = -1;
        // Whether an LE link has been seen since that wake-up.
        bool le_seen_since_resume = true;
        // When an unknown device that may be the phone last connected,
        // negative for never.
        int64_t stranger_at = -1;
        // LE links have been dying in a row (le_health.hpp), and the advert
        // is withheld for a moment because of it.
        bool unsteady = false;
        bool advert_paused = false;
        // The LE link went down for good on the heels of such a burst.
        bool lost_after_burst = false;
        // The classic link has reached the phone since: it is not away.
        bool classic_seen_since_loss = false;
        // The advert is withheld on purpose after an LE link that ended
        // before it was of any use (le_strikes.hpp).
        bool holding_off = false;
    };

    struct LeState {
        // "off", "unavailable", "connected", "waiting" or "paused".
        std::string state;
        // Empty when connected. Otherwise one of: "disabled", "not_paired",
        // "bond", "no_le_bond", "reconnecting", "after_resume", "holding_off",
        // "unsteady", "unresolved_address", "advert_off_air", "phone_backoff",
        // "phone_silent", "phone_away".
        std::string cause;
        // A sentence for the user, in English, empty unless paused or
        // holding off.
        std::string detail;

        bool operator==(const LeState&) const = default;
    };

    inline LeState le_state_for(const LeStateFacts& facts) {
        if (!facts.ancs_enabled)
            return {"off", "disabled", ""};
        if (!facts.device_paired)
            return {"unavailable", "not_paired", ""};
        if (facts.bond_unusable)
            return {"unavailable", "bond", ""};
        if (!facts.le_available)
            return {"unavailable", "no_le_bond", ""};
        if (facts.le_connected)
            return {"connected", "", ""};

        // Said before anything else: the silence is this computer's own
        // doing, for seconds or minutes, and nothing is wrong with the phone.
        if (facts.holding_off)
            return {"waiting",
                    "holding_off",
                    "Notifications are waiting. The link that carries them broke before it could be used, so "
                    "this computer is leaving the iPhone alone for a moment before calling it again; an iPhone "
                    "stops answering an accessory whose links keep failing."};

        const bool after_resume = facts.resumed_at >= 0 && !facts.le_seen_since_resume;
        const bool long_down = facts.le_down_since >= 0 && facts.now - facts.le_down_since >= LE_PAUSED_AFTER_SECONDS;
        if (!long_down || facts.holding_for_le)
            return {"waiting",
                    after_resume ? "after_resume" : facts.unsteady || facts.advert_paused ? "unsteady" : "reconnecting",
                    ""};

        if (facts.stranger_at >= 0 && facts.now - facts.stranger_at < STRANGER_MEMORY_SECONDS)
            return {"paused",
                    "unresolved_address",
                    "Notifications are paused. A device this computer does not recognise answered its call for "
                    "notifications; it may be the iPhone under an address that was not matched to the pairing. "
                    "Turn Bluetooth off and back on on the iPhone."};
        // Before the advert: a burst may be what has it off air right now.
        if (facts.lost_after_burst && (facts.classic_connected || facts.obex_up || facts.classic_seen_since_loss))
            return {"paused",
                    "phone_backoff",
                    "Notifications are paused. The link that carries them broke several times in a row and the "
                    "iPhone, although in reach, has stopped reopening it. It may come back on its own later; turning "
                    "Bluetooth off and back on on the iPhone brings it back at once."};
        if (facts.advert_paused)
            return {"waiting", "unsteady", ""};
        if (!facts.advert_registered)
            return {"paused",
                    "advert_off_air",
                    "Notifications are paused. This computer is not calling the iPhone for them at the moment."};
        if (facts.classic_connected || facts.obex_up)
            return {"paused",
                    after_resume ? "after_resume" : "phone_silent",
                    after_resume
                        ? "Notifications are paused. Since this computer woke up, the iPhone has not reopened the "
                          "link that carries them. Turn Bluetooth off and back on on the iPhone."
                        : "Notifications are paused. The iPhone is connected but has not reopened the link that "
                          "carries them. Turn Bluetooth off and back on on the iPhone."};
        if (facts.lost_after_burst)
            return {"paused",
                    "phone_away",
                    "Notifications are paused. The iPhone is out of reach, or, if it is right here, it has stopped "
                    "reopening the link after it broke several times in a row: turn Bluetooth off and back on on the "
                    "iPhone."};
        return {"paused", "phone_away", "Notifications are paused. The iPhone is out of reach."};
    }

} // namespace tether::bluetooth
