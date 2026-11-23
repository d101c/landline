#pragma once

#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/objects.hpp"

#include <chrono>
#include <functional>
#include <gio/gio.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

// Moves the iPhone's audio between this machine and the phone by opening or closing its A2DP stream.
namespace tether::bluetooth {

    // The iPhone opens the stream itself about a second after the link comes
    // up; within this long, a stream found open in Phone mode is closed.
    inline constexpr auto PHONE_AUDIO_WINDOW = std::chrono::seconds(30);
    // How many times per connection. More than once because WirePlumber may
    // dial the stream again after a close; bounded so that a route the user
    // picks on the iPhone is not fought for ever.
    inline constexpr int PHONE_AUDIO_MAX_CLOSES = 3;
    // Long enough for iOS to open the stream first, so Here does not race it.
    inline constexpr auto PHONE_AUDIO_HERE_DELAY = std::chrono::seconds(5);

    // What the follower does about the phone's stream on one look at it.
    enum class StreamMove {
        // Nothing yet; look again.
        Wait,
        // Close the stream: the sound goes back to the iPhone.
        Close,
        // Open it: the sound comes here.
        Open,
        // Nothing more for this connection.
        Done,
    };

    // `closing` is a close already asked for whose stream is still up;
    // `closes` how many were asked for on this connection.
    StreamMove plan_phone_audio(PhoneAudio choice,
                                bool stream_open,
                                bool closing,
                                int closes,
                                std::chrono::steady_clock::duration link_age);

    // A bt_set_phone_audio or bt_set_call_audio command, read.
    struct AudioRequest {
        // False when the command names no usable route.
        bool valid = false;
        PhoneAudio route = PhoneAudio::Phone;
        // Whether the choice is remembered. False moves the sound now, for
        // this connection or this call only.
        bool persist = true;
        // bt_set_phone_audio only: whether the WirePlumber rule is wanted.
        std::optional<bool> rule;
    };

    // "route" (here, phone, and auto when `allow_auto`), or the older
    // "enabled" (true = here). "persist" defaults to true; a one-off "auto"
    // means nothing and is refused.
    AudioRequest parse_audio_request(const nlohmann::json& command, bool allow_auto);

    // A WirePlumber 0.5 fragment that turns off auto-connect for one device.
    std::string wireplumber_rule(const std::string& address);

    // Where that fragment goes: wireplumber.conf.d/zz-tether-phone-audio.conf
    // under the user's configuration directory. Empty when there is none.
    std::string phone_audio_rule_path();
    bool phone_audio_rule_installed();

    // Writes the rule for `address`, or removes it when empty, and restarts WirePlumber
    // when the file changed. False when the file could not be written.
    bool install_wireplumber_rule(const std::string& address);

    // Brings the file in line with the configuration: present only while the
    // rule is asked for, a phone is selected and a route is remembered.
    bool sync_wireplumber_rule(const Config& config);

    // ConnectProfile or DisconnectProfile on the phone's A2DP source. Blocks for up to 30s.
    bool set_phone_audio_route(GDBusConnection* conn, const std::string& device_path, bool here, std::string& err);

    // "here", "phone", or "unavailable" when there is no Classic link or no sink role.
    std::string phone_audio_route(const BluezObjects& objects, const std::string& address);

    // Applies the remembered choice on each Classic connection, without fighting a
    // later change made on the iPhone or asked for by a client.
    class PhoneAudioFollower {
    public:
        // Told whenever the route wanted becomes "here" or stops being it:
        // in the lean link mode this is what holds the classic link open
        // for the sound (the Audio lease). Returns whether a link will be
        // opened for it. Called with the follower's lock released.
        using LeaseFn = std::function<bool(bool here)>;
        void set_lease_handler(LeaseFn handler);

        // True while the stream must be left as it is: a call is being
        // placed, is ringing or is in progress, and closing the stream then
        // puts one more exchange on a link that is setting up the voice. The
        // close is made once it turns false. Called from a worker thread.
        using HoldFn = std::function<bool()>;
        void set_hold(HoldFn hold);

        void configure(const std::string& address, PhoneAudio choice);
        // Main loop, on every snapshot change.
        void update(const BluezObjects& objects, GDBusConnection* conn);
        // Moves the audio now, to the remembered choice or to `here` when
        // given, and leaves the stream alone for the rest of the connection.
        // False when the phone is not connected or there is nothing to apply.
        // With no link, a one-off "here" is kept for the link the lease
        // handler says it is opening, and is then true.
        bool apply_now(GDBusConnection* conn, std::optional<bool> here = std::nullopt);

        // Whether the sound is wanted here: the remembered choice, or the
        // one-off route asked for this connection.
        bool wants_here() const;

    private:
        // `settle` waits for a stream the iPhone just opened to finish opening first.
        void act(GDBusConnection* conn, bool here, bool settle = false);

        // Tells the lease handler when wants_here() has changed.
        bool notify_lease();
        PhoneAudio effective_choice() const;

        mutable std::mutex mutex_;
        LeaseFn lease_handler_;
        HoldFn hold_;
        // What the handler was last told, unset before the first time.
        std::optional<bool> lease_told_;
        // A route asked for this connection only, forgotten when it ends.
        std::optional<bool> once_;
        std::string address_;
        PhoneAudio choice_ = PhoneAudio::Auto;
        std::string device_path_;
        std::optional<std::chrono::steady_clock::time_point> link_since_;
        bool acted_ = false;
        // A close was asked for and the stream has not gone yet.
        bool closing_ = false;
        int closes_ = 0;
        // Bumped on every link change, so a delayed action from an old connection is dropped.
        unsigned generation_ = 0;
    };

    extern PhoneAudioFollower* g_phone_audio;

} // namespace tether::bluetooth
