#include "tether/bluetooth/call_audio_handover.hpp"

#include "tether/log.hpp"

#include <algorithm>
#include <cctype>
#include <gio/gio.h>

namespace tether::bluetooth {

    namespace {

        constexpr const char* OFF = "off";
        constexpr const char* USUAL = "audio-gateway";

        std::string lower(std::string text) {
            std::transform(
                text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string string_of(const nlohmann::json& object, const char* key) {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        std::optional<CardProfile> profile_of(const nlohmann::json& entry) {
            if (!entry.is_object())
                return std::nullopt;
            const auto index = entry.find("index");
            if (index == entry.end() || !index->is_number_integer())
                return std::nullopt;
            return CardProfile{index->get<int>(), string_of(entry, "name")};
        }

        // bluez_card.AA_BB_CC_DD_EE_FF, as WirePlumber names the card.
        std::string card_name_for(const std::string& address) {
            std::string name = "bluez_card." + address;
            std::replace(name.begin(), name.end(), ':', '_');
            return lower(name);
        }

    } // namespace

    std::optional<PhoneCard> find_phone_card(const nlohmann::json& dump, const std::string& address) {
        if (!dump.is_array() || address.empty())
            return std::nullopt;
        const std::string wanted = lower(address);
        const std::string wanted_name = card_name_for(address);

        for (const auto& object : dump) {
            if (!object.is_object() || string_of(object, "type") != "PipeWire:Interface:Device")
                continue;
            const auto info = object.find("info");
            if (info == object.end() || !info->is_object())
                continue;
            const auto props = info->find("props");
            if (props == info->end() || !props->is_object())
                continue;
            if (lower(string_of(*props, "api.bluez5.address")) != wanted &&
                lower(string_of(*props, "device.name")) != wanted_name)
                continue;
            const auto id = object.find("id");
            if (id == object.end() || !id->is_number_integer())
                continue;

            PhoneCard card;
            card.id = id->get<int>();
            card.name = string_of(*props, "device.name");
            const auto params = info->find("params");
            if (params != info->end() && params->is_object()) {
                if (const auto active = params->find("Profile");
                    active != params->end() && active->is_array() && !active->empty())
                    if (const auto profile = profile_of(active->front()))
                        card.active = *profile;
                if (const auto all = params->find("EnumProfile"); all != params->end() && all->is_array())
                    for (const auto& entry : *all)
                        if (const auto profile = profile_of(entry))
                            card.profiles.push_back(*profile);
            }
            return card;
        }
        return std::nullopt;
    }

    std::optional<CardProfile> off_profile(const PhoneCard& card) {
        for (const auto& profile : card.profiles)
            if (profile.name == OFF)
                return profile;
        return std::nullopt;
    }

    std::optional<CardProfile> voice_profile(const PhoneCard& card, const std::string& previous) {
        const auto named = [&card](const std::string& name) -> std::optional<CardProfile> {
            if (name.empty() || name == OFF)
                return std::nullopt;
            for (const auto& profile : card.profiles)
                if (profile.name == name)
                    return profile;
            return std::nullopt;
        };
        if (const auto profile = named(previous))
            return profile;
        if (const auto profile = named(USUAL))
            return profile;
        for (const auto& profile : card.profiles)
            if (profile.name != OFF)
                return profile;
        return std::nullopt;
    }

    const char* to_string(HandoverOutcome outcome) {
        switch (outcome) {
            case HandoverOutcome::AlreadyOnPhone:
                return "already_on_phone";
            case HandoverOutcome::Released:
                return "released";
            case HandoverOutcome::ReleasedParked:
                return "released_parked";
            case HandoverOutcome::StillHere:
                return "still_here";
            case HandoverOutcome::NoLever:
                return "no_lever";
        }
        return "no_lever";
    }

    bool restore_voice_profile(CallAudioCard& card, const std::string& address, const std::string& previous) {
        const auto now = card.read(address);
        if (!now)
            return false;
        if (now->active.name != OFF && now->active.index >= 0)
            return true;
        const auto back = voice_profile(*now, previous);
        return back && card.set_profile(*now, *back);
    }

    HandoverResult hand_call_audio_to_phone(CallAudioCard& card,
                                            const std::string& address,
                                            const std::function<bool()>& voice_link_up,
                                            const std::function<void(int)>& sleep_ms,
                                            const HandoverTiming& timing) {
        HandoverResult result;
        if (!voice_link_up()) {
            result.outcome = HandoverOutcome::AlreadyOnPhone;
            return result;
        }

        const auto before = card.read(address);
        if (!before)
            return result;
        const auto off = off_profile(*before);
        if (!off)
            return result;
        const std::string previous = before->active.name == OFF ? std::string{} : before->active.name;
        if (before->active.name != OFF && !card.set_profile(*before, *off))
            return result;

        const int step = std::max(1, timing.step_ms);
        const auto wait_until = [&](bool up, int budget_ms) {
            int waited = 0;
            while (voice_link_up() != up) {
                if (waited >= budget_ms)
                    return -1;
                sleep_ms(step);
                waited += step;
            }
            return waited;
        };

        result.released_after_ms = wait_until(false, timing.release_ms);
        const bool closed = result.released_after_ms >= 0;

        // Back on its profile either way: `off` is a lever, not a state to
        // leave the card in.
        const bool restored = restore_voice_profile(card, address, previous);
        if (!closed) {
            result.outcome = HandoverOutcome::StillHere;
            return result;
        }
        if (!restored) {
            // The link is closed and the card stayed off: say so, so that it
            // is put back later.
            result.outcome = HandoverOutcome::ReleasedParked;
            result.parked_profile = previous;
            return result;
        }

        // The restored profile must not reopen the link by itself.
        if (wait_until(true, timing.settle_ms) < 0) {
            result.outcome = HandoverOutcome::Released;
            return result;
        }
        const auto again = card.read(address);
        if (again && card.set_profile(*again, *off) && wait_until(false, timing.release_ms) >= 0) {
            result.outcome = HandoverOutcome::ReleasedParked;
            result.parked_profile = previous;
            return result;
        }
        restore_voice_profile(card, address, previous);
        result.outcome = HandoverOutcome::StillHere;
        result.released_after_ms = -1;
        return result;
    }

    namespace {

        // Bounded: these tools talk to the PipeWire daemon and must not hold
        // the caller if it is stuck.
        bool run(const std::string& command, std::string* out) {
            gchar* text = nullptr;
            gint status = 0;
            GError* error = nullptr;
            const std::string bounded = "timeout 3 " + command;
            const bool spawned =
                g_spawn_command_line_sync(bounded.c_str(), out ? &text : nullptr, nullptr, &status, &error);
            if (!spawned) {
                debug::log(DEBUG, "call audio: {}: {}", command, error ? error->message : "could not run");
                g_clear_error(&error);
                return false;
            }
            if (out && text)
                *out = text;
            g_free(text);
            return status == 0;
        }

        class PipewireCard : public CallAudioCard {
        public:
            std::optional<PhoneCard> read(const std::string& address) override {
                std::string text;
                if (!run("pw-dump", &text))
                    return std::nullopt;
                const nlohmann::json dump = nlohmann::json::parse(text, nullptr, false);
                if (dump.is_discarded())
                    return std::nullopt;
                return find_phone_card(dump, address);
            }

            bool set_profile(const PhoneCard& card, const CardProfile& profile) override {
                if (card.id < 0 || profile.index < 0)
                    return false;
                const std::string id = std::to_string(card.id);
                const std::string index = std::to_string(profile.index);
                // save:false -- a lever for one call, never the user's choice.
                const std::string commands[] = {
                    "pw-cli set-param " + id + " Profile '{ index: " + index + ", save: false }'",
                    "wpctl set-profile " + id + " " + index,
                };
                const std::string address = address_of(card);
                for (const auto& command : commands) {
                    run(command, nullptr);
                    // Read back: either tool can return before, or without,
                    // the card having changed.
                    for (int attempt = 0; attempt < 4; ++attempt) {
                        const auto now = read(address);
                        if (now && now->active.index == profile.index)
                            return true;
                        g_usleep(80 * 1000);
                    }
                }
                debug::log(WARN, "call audio: {} did not take the profile {}", card.name, profile.name);
                return false;
            }

        private:
            // bluez_card.AA_BB_CC_DD_EE_FF -> AA:BB:CC:DD:EE:FF
            static std::string address_of(const PhoneCard& card) {
                const size_t dot = card.name.find('.');
                std::string address = dot == std::string::npos ? card.name : card.name.substr(dot + 1);
                std::replace(address.begin(), address.end(), '_', ':');
                return address;
            }
        };

    } // namespace

    std::unique_ptr<CallAudioCard> make_pipewire_card() { return std::make_unique<PipewireCard>(); }

} // namespace tether::bluetooth
