#pragma once

#include <cstdint>
#include <deque>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// Whether the bond with the phone is worth using. Two sources: what BlueZ
// says about the keys it holds, read once and without waiting for a failure,
// and what the link does when it is used. Free of any bus concern.
namespace tether::bluetooth {

    struct Device;

    // Published as "none", "ok", "partial", "broken". Stable: clients switch on them.
    enum class BondState {
        None,    // nothing paired on this side
        Ok,      // both halves, or no way to tell them apart
        Partial, // one half is missing
        Broken,  // the phone keeps refusing to authenticate
    };

    inline const char* to_string(BondState state) {
        switch (state) {
        case BondState::None:
            return "none";
        case BondState::Ok:
            return "ok";
        case BondState::Partial:
            return "partial";
        case BondState::Broken:
            return "broken";
        }
        return "none";
    }

    // Published as bond_detail. Stable too.
    inline constexpr const char* BOND_DETAIL_BREDR_ONLY = "bredr_only";
    inline constexpr const char* BOND_DETAIL_LE_ONLY = "le_only";
    inline constexpr const char* BOND_DETAIL_AUTH_FAILURES = "auth_failures";
    inline constexpr const char* BOND_DETAIL_KEY_MISSING = "key_missing";

    // What bt_pair answers, as status and as reason, when the bond that is
    // already there should not be reused.
    inline constexpr const char* PAIR_STATUS_STALE_BOND = "stale_bond";

    // How many authentication failures, how close together, make a bond broken.
    // The incident this guards against ran at one every eight seconds for
    // twenty-five minutes; a healthy bond produces none.
    inline constexpr int AUTH_FAILURES_BEFORE_BROKEN = 5;
    inline constexpr int AUTH_FAILURE_WINDOW_SECONDS = 120;
    // A refusal that names the missing key is not ambiguous; two rule out a
    // one-off answer from a phone that was busy pairing with something else.
    inline constexpr int KEY_MISSING_BEFORE_BROKEN = 2;
    // An LE link that fails authentication is cut within seconds, so one that
    // has held this long is encrypted and the bond works after all.
    inline constexpr int BOND_RECOVERY_SECONDS = 60;
    // While the bond is broken: one window of normal attempts, this often.
    inline constexpr int BROKEN_BOND_RETRY_SECONDS = 600;
    inline constexpr int BROKEN_BOND_PROBE_SECONDS = 30;

    inline constexpr const char* REASON_AUTHENTICATION = "org.bluez.Reason.Authentication";

    // Whether a refused connect names keys the phone no longer has.
    inline bool is_key_missing(std::string_view err) {
        return err.find("key-missing") != std::string_view::npos || err.find("Key Missing") != std::string_view::npos;
    }

    // The bonding facts BlueZ exposes for one device.
    struct BondFacts {
        // Device1.Paired
        bool paired = false;
        // Bearer.LE1 is populated, so its Bonded means something.
        bool le_known = false;
        bool le_bonded = false;
        // Bearer.BREDR1 carries Paired. It is a bare marker on some builds.
        bool classic_known = false;
        bool classic_bonded = false;
        // A live ANCS subscription. It rides LE alone, so it outranks whatever
        // the bearer properties say about the LE half.
        bool ancs_live = false;

        bool operator==(const BondFacts&) const = default;
    };

    BondFacts bond_facts(const Device& device);

    struct BondHealth {
        BondState state = BondState::None;
        std::string detail;
        // Remove the pairing on both sides and pair again.
        bool needs_repair = false;

        bool operator==(const BondHealth&) const = default;
    };

    // The static half. `le_expected` is whether this machine can carry the LE
    // half at all: where it cannot, a BR/EDR-only bond is all there will ever
    // be, and asking for a new pairing would only produce the same one.
    // An LE-only bond is reported but not condemned: Bearer.BREDR1 is not
    // populated consistently enough across BlueZ builds to act on.
    inline BondHealth assess_bond(const BondFacts& facts, bool le_expected = true) {
        BondHealth health;
        if (!facts.paired)
            return health;
        health.state = BondState::Ok;
        if (facts.le_known && !facts.le_bonded && !facts.ancs_live) {
            health.state = BondState::Partial;
            health.detail = BOND_DETAIL_BREDR_ONLY;
            health.needs_repair = le_expected;
        } else if (facts.le_known && facts.le_bonded && facts.classic_known && !facts.classic_bonded) {
            health.state = BondState::Partial;
            health.detail = BOND_DETAIL_LE_ONLY;
        }
        return health;
    }

    // The observed half: authentication failures as the link reports them.
    // Latches broken, and only an LE link that holds lets go of it.
    class AuthFailureTracker {
    public:
        // `count` Disconnected signals with Reason.Authentication since the
        // last call. True when this is what broke the bond.
        bool note_auth_failures(int64_t now, uint64_t count = 1) {
            le_up_since_ = -1;
            for (uint64_t i = 0; i < count && i < static_cast<uint64_t>(AUTH_FAILURES_BEFORE_BROKEN); ++i)
                failures_.push_back(now);
            prune(now);
            if (broken_ || static_cast<int>(failures_.size()) < AUTH_FAILURES_BEFORE_BROKEN)
                return false;
            broken_ = true;
            detail_ = BOND_DETAIL_AUTH_FAILURES;
            return true;
        }

        // A connect refused for want of a key. True when this is what broke the bond.
        bool note_key_missing() {
            ++key_missing_;
            if (broken_ || key_missing_ < KEY_MISSING_BEFORE_BROKEN)
                return false;
            broken_ = true;
            detail_ = BOND_DETAIL_KEY_MISSING;
            return true;
        }

        // A BR/EDR link that came up answers the question key-missing asked.
        void note_classic_connected() {
            if (!broken_)
                key_missing_ = 0;
        }

        // Once per tick. True when the bond has just been found working again.
        bool observe_le(int64_t now, bool le_connected) {
            prune(now);
            if (!le_connected) {
                le_up_since_ = -1;
                return false;
            }
            if (le_up_since_ < 0)
                le_up_since_ = now;
            if (!broken_ || now - le_up_since_ < BOND_RECOVERY_SECONDS)
                return false;
            reset();
            le_up_since_ = now;
            return true;
        }

        void reset() {
            failures_.clear();
            key_missing_ = 0;
            broken_ = false;
            detail_.clear();
            le_up_since_ = -1;
        }

        bool broken() const { return broken_; }
        const std::string& detail() const { return detail_; }
        int recent_failures() const { return static_cast<int>(failures_.size()); }

    private:
        void prune(int64_t now) {
            while (!failures_.empty() && now - failures_.front() >= AUTH_FAILURE_WINDOW_SECONDS)
                failures_.pop_front();
        }

        std::deque<int64_t> failures_;
        int key_missing_ = 0;
        bool broken_ = false;
        std::string detail_;
        int64_t le_up_since_ = -1;
    };

    // What is seen in use outranks what the keys say.
    inline BondHealth combine_bond(const BondHealth& assessed, bool broken, const std::string& broken_detail) {
        if (!broken || assessed.state == BondState::None)
            return assessed;
        return BondHealth{BondState::Broken, broken_detail, true};
    }

    // Whether bt_pair may answer already_paired for a bond in this state.
    inline bool bond_reusable(const BondHealth& health) { return !health.needs_repair; }

    // bond_state, bond_detail, needs_repair: the same three keys wherever the
    // bond is reported.
    inline void put_bond(nlohmann::json& payload, const BondHealth& health) {
        payload["bond_state"] = to_string(health.state);
        payload["bond_detail"] = health.detail;
        payload["needs_repair"] = health.needs_repair;
    }

    // The bond as a stored bt_connection_changed payload last reported it.
    inline BondHealth bond_from_json(const nlohmann::json& payload) {
        BondHealth health;
        if (!payload.is_object())
            return health;
        const std::string state = payload.value("bond_state", std::string("none"));
        health.state = state == "ok"        ? BondState::Ok
                       : state == "partial" ? BondState::Partial
                       : state == "broken"  ? BondState::Broken
                                            : BondState::None;
        health.detail = payload.value("bond_detail", std::string());
        health.needs_repair = payload.value("needs_repair", false);
        return health;
    }

} // namespace tether::bluetooth
