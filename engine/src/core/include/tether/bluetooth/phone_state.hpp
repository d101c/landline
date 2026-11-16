#pragma once

#include <cctype>
#include <optional>
#include <string>

// What the iPhone says about itself besides its links: the battery level and
// the network name. Pure, so the thresholds can be tested without a phone.
namespace tether::bluetooth {

    inline constexpr int BATTERY_LOW_PERCENT = 20;
    inline constexpr int BATTERY_CRITICAL_PERCENT = 10;
    // How far above a threshold the level must climb before that threshold
    // can be announced again. A level hovering at 20-21 % announces once.
    inline constexpr int BATTERY_REARM_MARGIN = 5;

    // A level and where it was read: "le" for the Battery Service BlueZ reads
    // over the LE link (1 % steps), "hfp" for the hands-free indicator (0-5,
    // published as 20 % steps), "" for none.
    struct BatteryReading {
        int percent = -1;
        const char* source = "";

        bool known() const { return percent >= 0; }
    };

    // "ok", "low" (at or under 20 %), "critical" (at or under 10 %), "" when unknown.
    inline const char* battery_state(int percent) {
        if (percent < 0)
            return "";
        if (percent <= BATTERY_CRITICAL_PERCENT)
            return "critical";
        if (percent <= BATTERY_LOW_PERCENT)
            return "low";
        return "ok";
    }

    struct BatteryAlert {
        // BATTERY_LOW_PERCENT or BATTERY_CRITICAL_PERCENT.
        int threshold = 0;
        int level = 0;
        // The first level read was already under the threshold: nothing was
        // seen crossing it. A client may prefer to stay quiet for that one.
        bool initial = false;
    };

    // Announces each threshold once per crossing. A level that drops through
    // both at once announces only the lower one.
    class BatteryWatch {
    public:
        std::optional<BatteryAlert> observe(int percent) {
            // An unknown level (phone away) neither announces nor re-arms.
            if (percent < 0)
                return std::nullopt;

            if (percent >= BATTERY_LOW_PERCENT + BATTERY_REARM_MARGIN)
                announced_ = 0;
            else if (announced_ == BATTERY_CRITICAL_PERCENT &&
                     percent >= BATTERY_CRITICAL_PERCENT + BATTERY_REARM_MARGIN)
                announced_ = BATTERY_LOW_PERCENT;

            const int reached = percent <= BATTERY_CRITICAL_PERCENT ? BATTERY_CRITICAL_PERCENT
                                : percent <= BATTERY_LOW_PERCENT    ? BATTERY_LOW_PERCENT
                                                                    : 0;
            const bool first = !seen_;
            seen_ = true;
            if (reached == 0 || (announced_ != 0 && reached >= announced_))
                return std::nullopt;
            announced_ = reached;
            return BatteryAlert{reached, percent, first};
        }

        // A different phone starts from nothing.
        void reset() { *this = BatteryWatch{}; }

    private:
        // The lowest threshold already announced, 0 for none.
        int announced_ = 0;
        bool seen_ = false;
    };

    // Whether the network name the phone reports over hands-free says calls
    // go over Wi-Fi ("WiFi-Calling", "Wi-Fi Call"). A reading of the name,
    // not an indicator: hands-free has none for it.
    inline bool operator_says_wifi_calling(const std::string& name) {
        std::string folded;
        for (unsigned char c : name)
            if (std::isalnum(c))
                folded.push_back(static_cast<char>(std::tolower(c)));
        return folded.find("wificall") != std::string::npos;
    }

} // namespace tether::bluetooth
