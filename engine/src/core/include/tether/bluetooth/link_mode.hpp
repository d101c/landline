#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

// The two ways the links to the iPhone are kept.
//
//   lean  the LE link alone is permanent; the classic link is opened when
//         something needs it (classic_demand.hpp). The default.
//   full  both links are kept up all the time, as before.
//
// The mode applied can differ from the one set: lean rests on the LE link
// and on notifications, and falls back to full without them.
namespace tether::bluetooth {

    enum class LinkMode { Lean, Full };

    inline const char* to_string(LinkMode mode) { return mode == LinkMode::Full ? "full" : "lean"; }

    // Anything but "full" is lean: a file written before the setting existed
    // has no value at all.
    inline LinkMode link_mode_from_string(std::string_view value) {
        return value == "full" ? LinkMode::Full : LinkMode::Lean;
    }

    // "lean" or "full" exactly; anything else leaves the mode alone.
    inline std::optional<LinkMode> parse_link_mode(std::string_view value) {
        if (value == "lean")
            return LinkMode::Lean;
        if (value == "full")
            return LinkMode::Full;
        return std::nullopt;
    }

    // LANDLINE_LINK_MODE: tries a mode without touching the file.
    inline std::optional<LinkMode> link_mode_from_env(const char* text) {
        return text ? parse_link_mode(text) : std::nullopt;
    }

    struct EffectiveLinkMode {
        LinkMode mode = LinkMode::Lean;
        // "", "no_le", "notifications_off" or "phone_keeps_link".
        std::string reason;

        bool lean() const { return mode == LinkMode::Lean; }
        bool operator==(const EffectiveLinkMode&) const = default;
    };

    // `le_available`: this machine and this bond can carry the LE half.
    // `ancs_enabled`: notification mirroring is on. `ancs_ready_once`: the
    // iPhone has granted notifications to this bond at least once and has not
    // taken them back. `phone_keeps_link`: the loop guard of ClassicDemand.
    inline EffectiveLinkMode effective_link_mode(
        LinkMode configured, bool le_available, bool ancs_enabled, bool ancs_ready_once, bool phone_keeps_link) {
        if (configured == LinkMode::Full)
            return {LinkMode::Full, ""};
        if (!le_available)
            return {LinkMode::Full, "no_le"};
        // Without notifications nothing says when a message has arrived.
        if (!ancs_enabled || !ancs_ready_once)
            return {LinkMode::Full, "notifications_off"};
        if (phone_keeps_link)
            return {LinkMode::Full, "phone_keeps_link"};
        return {LinkMode::Lean, ""};
    }

    // What the iPhone last answered when a profile was opened.
    enum class ProfileVerdict { Unknown, Granted, Forbidden };

    inline const char* to_string(ProfileVerdict verdict) {
        return verdict == ProfileVerdict::Granted     ? "granted"
               : verdict == ProfileVerdict::Forbidden ? "forbidden"
                                                      : "unknown";
    }

    inline ProfileVerdict profile_verdict_from_string(std::string_view value) {
        return value == "granted"     ? ProfileVerdict::Granted
               : value == "forbidden" ? ProfileVerdict::Forbidden
                                      : ProfileVerdict::Unknown;
    }

    // What outlives a closed link and a restart: the last verdict of each
    // profile with its date, and whether notifications were ever granted.
    struct LinkMemory {
        std::string address;
        bool ancs_ready_once = false;
        ProfileVerdict map = ProfileVerdict::Unknown;
        int64_t map_at = 0;
        ProfileVerdict pbap = ProfileVerdict::Unknown;
        int64_t pbap_at = 0;

        bool operator==(const LinkMemory&) const = default;
    };

    inline std::string serialize_link_memory(const LinkMemory& memory) {
        nlohmann::json j;
        j["address"] = memory.address;
        j["ancs_ready_once"] = memory.ancs_ready_once;
        j["map"] = {{"verdict", to_string(memory.map)}, {"at", memory.map_at}};
        j["pbap"] = {{"verdict", to_string(memory.pbap)}, {"at", memory.pbap_at}};
        return j.dump(2);
    }

    // What was kept for `address`; nothing for another phone or a bad file.
    inline LinkMemory deserialize_link_memory(const std::string& text, const std::string& address) {
        LinkMemory memory;
        memory.address = address;
        try {
            const auto j = nlohmann::json::parse(text);
            if (!j.is_object() || j.value("address", "") != address)
                return memory;
            memory.ancs_ready_once = j.value("ancs_ready_once", false);
            const auto read = [&j](const char* key, ProfileVerdict& verdict, int64_t& at) {
                const auto it = j.find(key);
                if (it == j.end() || !it->is_object())
                    return;
                verdict = profile_verdict_from_string(it->value("verdict", ""));
                at = it->value("at", static_cast<int64_t>(0));
            };
            read("map", memory.map, memory.map_at);
            read("pbap", memory.pbap, memory.pbap_at);
        } catch (const std::exception&) {
        }
        return memory;
    }

    // `messages` and `contacts` of the published status: what the user can
    // expect of the profile.
    //   ready        the session is open
    //   on_demand    granted at the last opening, link closed: opens when needed
    //   forbidden    the iPhone's switch for it is off
    //   unavailable  cannot be had now (released, phone out of reach, error)
    //   unknown      never tried on this bond
    struct ServiceFacts {
        bool open = false;
        bool held = false;
        // The last attempt was refused by the switch.
        bool error_forbidden = false;
        // The last attempt failed for another reason.
        bool error_other = false;
        ProfileVerdict verdict = ProfileVerdict::Unknown;
        bool lean = false;
        bool le_connected = false;
    };

    inline const char* service_state(const ServiceFacts& facts) {
        if (facts.open)
            return "ready";
        if (facts.held)
            return "unavailable";
        if (facts.error_forbidden || facts.verdict == ProfileVerdict::Forbidden)
            return "forbidden";
        if (facts.lean) {
            if (!facts.le_connected)
                return "unavailable";
            return facts.verdict == ProfileVerdict::Granted ? "on_demand" : "unknown";
        }
        return facts.error_other ? "unavailable" : "unknown";
    }

    // `calls_available` of the published status.
    //   ready         hands-free is up: answer, decline, dial, sound here
    //   on_demand     the link opens for a call when one comes or is placed
    //   control_only  answer and decline through the notification, no sound here
    //   off           call control is off, or the iPhone is out of reach
    struct CallsFacts {
        bool calls_enabled = false;
        bool hands_free_ready = false;
        bool lean = false;
        bool le_connected = false;
        bool ancs_ready = false;
        // The classic link could not be opened and is not being tried.
        bool classic_failed = false;
    };

    inline const char* calls_state(const CallsFacts& facts) {
        if (!facts.calls_enabled)
            return "off";
        if (facts.hands_free_ready)
            return "ready";
        if (facts.lean && facts.le_connected && !facts.classic_failed)
            return "on_demand";
        if (facts.le_connected && facts.ancs_ready)
            return "control_only";
        return "off";
    }

} // namespace tether::bluetooth
