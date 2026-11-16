#include <csignal>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <tether/bluetooth/agent.hpp>
#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/pairing.hpp>

using namespace tether::bluetooth;

// The agent authorizes services during pairing. Anything outside this set would
// grant the phone a profile Tether has no reason to consume.
TEST(AgentPolicy, AuthorizesOnlyMapAndPbap) {
    EXPECT_TRUE(is_authorized_service("0000112e-0000-1000-8000-00805f9b34fb")); // PBAP client
    EXPECT_TRUE(is_authorized_service("0000112f-0000-1000-8000-00805f9b34fb")); // PBAP server
    EXPECT_TRUE(is_authorized_service("00001132-0000-1000-8000-00805f9b34fb")); // MAP server
    EXPECT_TRUE(is_authorized_service("00001133-0000-1000-8000-00805f9b34fb")); // MAP notification
    EXPECT_TRUE(is_authorized_service("00001134-0000-1000-8000-00805f9b34fb")); // MAP client
}

TEST(AgentPolicy, RefusesEverythingElse) {
    // Hands-free / headset: offered by the phone, and refused until call control
    // is deliberately turned on.
    EXPECT_FALSE(is_authorized_service("0000111e-0000-1000-8000-00805f9b34fb"));
    EXPECT_FALSE(is_authorized_service("0000110a-0000-1000-8000-00805f9b34fb"));
    // ANCS is reached over GATT after bonding, never authorized as a profile.
    EXPECT_FALSE(is_authorized_service("7905f431-b5ce-4e99-a40f-4b1e122d00d0"));
    EXPECT_FALSE(is_authorized_service(""));
    EXPECT_FALSE(is_authorized_service("garbage"));
    // A prefix match would be a real hole; require the whole UUID.
    EXPECT_FALSE(is_authorized_service("00001132-0000-1000-8000-00805f9b34f"));
    EXPECT_FALSE(is_authorized_service("00001132-0000-1000-8000-00805f9b34fbb"));
}

TEST(AgentPolicy, ServiceMatchIsCaseInsensitive) {
    EXPECT_TRUE(is_authorized_service("0000112F-0000-1000-8000-00805F9B34FB"));
    EXPECT_TRUE(is_authorized_service("0000111E-0000-1000-8000-00805F9B34FB", true));
}

// Call control needs the phone to reach the HFP channel PipeWire registers, so
// the hands-free UUIDs are authorized -- but only then.
TEST(AgentPolicy, AuthorizesHandsFreeOnlyWithCallsEnabled) {
    for (const char* uuid : {"0000111e-0000-1000-8000-00805f9b34fb",
                             "0000111f-0000-1000-8000-00805f9b34fb",
                             "00001108-0000-1000-8000-00805f9b34fb",
                             "00001112-0000-1000-8000-00805f9b34fb"}) {
        EXPECT_FALSE(is_authorized_service(uuid)) << uuid;
        EXPECT_TRUE(is_authorized_service(uuid, true)) << uuid;
    }
}

// Widening the set for calls must not open anything else.
TEST(AgentPolicy, CallsEnabledStillRefusesEverythingElse) {
    EXPECT_FALSE(is_authorized_service("0000110a-0000-1000-8000-00805f9b34fb", true)); // A2DP source
    EXPECT_FALSE(is_authorized_service("0000110b-0000-1000-8000-00805f9b34fb", true)); // A2DP sink
    EXPECT_FALSE(is_authorized_service("7905f431-b5ce-4e99-a40f-4b1e122d00d0", true)); // ANCS
    EXPECT_FALSE(is_authorized_service("", true));
    EXPECT_FALSE(is_authorized_service("garbage", true));
    EXPECT_FALSE(is_authorized_service("0000111e-0000-1000-8000-00805f9b34f", true));
    EXPECT_FALSE(is_authorized_service("0000111e-0000-1000-8000-00805f9b34fbb", true));
}

TEST(AgentPolicy, FollowsARecreatedDeviceObject) {
    PairingAgent agent(nullptr, "/org/bluez/hci0/dev_old", {});
    EXPECT_TRUE(agent.accepts_device_path("/org/bluez/hci0/dev_old"));
    EXPECT_FALSE(agent.accepts_device_path("/org/bluez/hci0/dev_new"));

    agent.set_device_path("/org/bluez/hci0/dev_new");
    EXPECT_FALSE(agent.accepts_device_path("/org/bluez/hci0/dev_old"));
    EXPECT_TRUE(agent.accepts_device_path("/org/bluez/hci0/dev_new"));
}

// iOS renders the comparison with leading zeros. Dropping them shows the user a
// code that does not match their phone, which reads as a failed pairing.
TEST(Passkey, FormatsSixDigitsWithLeadingZeros) {
    EXPECT_EQ(format_passkey(123456), "123456");
    EXPECT_EQ(format_passkey(1234), "001234");
    EXPECT_EQ(format_passkey(0), "000000");
    EXPECT_EQ(format_passkey(7), "000007");
    EXPECT_EQ(format_passkey(999999), "999999");
}

TEST(BluetoothConfig, RoundTrips) {
    Config config;
    config.device_address = "02:00:00:00:00:01";
    config.auth_strategy = AuthStrategy::ExplicitPair;
    config.ancs_enabled = false;
    config.enabled = false;
    config.adapter = "hci1";
    config.calls_enabled = true;
    config.network_enabled = true;
    config.desktop_popups_enabled = true;
    config.pair_dialog_enabled = true;
    config.popup_previews_enabled = false;
    config.clipboard_auto_share = false;
    config.lock_on_away = true;
    config.lock_away_seconds = 45;
    config.lock_command = "hyprlock";
    config.airpods_enabled = true;

    EXPECT_EQ(deserialize_config(serialize_config(config)), config);
}

TEST(BluetoothConfig, DefaultsToConnectFirstAndAncsEnabled) {
    Config config = deserialize_config("{}");
    EXPECT_EQ(config.auth_strategy, AuthStrategy::ConnectFirst);
    EXPECT_TRUE(config.ancs_enabled);
    EXPECT_TRUE(config.device_address.empty());
    // A config written before the toggle existed keeps connecting.
    EXPECT_TRUE(config.enabled);
    // No controller pinned means the first powered one.
    EXPECT_TRUE(config.adapter.empty());
    // Call control needs WirePlumber configured for it, so it is never assumed.
    EXPECT_FALSE(config.calls_enabled);
    // Bluetooth only unless the Wi-Fi side is asked for.
    EXPECT_FALSE(config.network_enabled);
    // Notifications and the pairing code are the subscribed client's to show.
    EXPECT_FALSE(config.desktop_popups_enabled);
    EXPECT_FALSE(config.pair_dialog_enabled);
    EXPECT_TRUE(config.popup_previews_enabled);
    EXPECT_TRUE(config.clipboard_auto_share);
    // AirPods are not managed until asked: the channel takes one client per machine.
    EXPECT_FALSE(config.airpods_enabled);
    // Locking the screen is never assumed; logind performs it once switched on.
    EXPECT_FALSE(config.lock_on_away);
    EXPECT_EQ(config.lock_away_seconds, AWAY_LOCK_GRACE_SECONDS);
    EXPECT_TRUE(config.lock_command.empty());
}

// A zero or negative grace would lock on the first flap.
TEST(BluetoothConfig, ClampsTheAwayGraceToAtLeastASecond) {
    EXPECT_EQ(deserialize_config(R"({"lock_away_seconds": 0})").lock_away_seconds, 1);
    EXPECT_EQ(deserialize_config(R"({"lock_away_seconds": -5})").lock_away_seconds, 1);
}

// Switching Bluetooth off supervises no device, which is what stops the daemon
// re-dialling a link the user disconnected.
TEST(BluetoothConfig, SupervisedAddressIsEmptyWhenDisabled) {
    Config config;
    config.device_address = "02:00:00:00:00:01";
    EXPECT_EQ(supervised_address(config), config.device_address);

    config.enabled = false;
    EXPECT_TRUE(supervised_address(config).empty());
}

// Connect-first is the default for a reason: a Linux-initiated Pair() can yield
// a BR/EDR-only bond that can never carry ANCS. An unknown value must not
// silently select the workaround.
TEST(BluetoothConfig, UnknownStrategyFallsBackToConnectFirst) {
    EXPECT_EQ(auth_strategy_from_string("nonsense"), AuthStrategy::ConnectFirst);
    EXPECT_EQ(auth_strategy_from_string(""), AuthStrategy::ConnectFirst);
    EXPECT_EQ(auth_strategy_from_string("explicit-pair"), AuthStrategy::ExplicitPair);
}

TEST(BluetoothConfig, SurvivesCorruptFile) {
    Config config = deserialize_config("{not json at all");
    EXPECT_EQ(config, Config{});

    // A valid JSON document of the wrong shape must also yield defaults.
    EXPECT_EQ(deserialize_config("[1,2,3]"), Config{});
    EXPECT_EQ(deserialize_config("\"string\""), Config{});
}

// Connect-first induces authentication only as a side effect of a profile
// connect. A phone that refuses that profile never starts pairing at all, so the
// transaction is retried as an explicit Device1.Pair() -- see issue #49.
TEST(FallbackPolicy, RetriesARefusedConnectFirst) {
    EXPECT_TRUE(should_fall_back(AuthStrategy::ConnectFirst, /*paired=*/false, /*user_rejected=*/false));
}

TEST(FallbackPolicy, DoesNotRetryWhatAlreadyWorked) {
    EXPECT_FALSE(should_fall_back(AuthStrategy::ConnectFirst, /*paired=*/true, /*user_rejected=*/false));
}

// Re-prompting someone who just declined the numeric comparison reads as the
// computer ignoring them.
TEST(FallbackPolicy, DoesNotRetryAUserRejection) {
    EXPECT_FALSE(should_fall_back(AuthStrategy::ConnectFirst, /*paired=*/false, /*user_rejected=*/true));
}

// The extension owns the confirmation: with no dialog asked for, the code goes
// straight to the subscribed client and no window is ever launched.
TEST(PairConfirmation, GoesToTheClientByDefault) {
    int asked = 0;
    const ConfirmOutcome outcome = resolve_confirmation(
        "123456",
        [&asked](const std::string& code) {
            ++asked;
            return code == "123456";
        },
        nullptr);
    EXPECT_EQ(asked, 1);
    EXPECT_TRUE(outcome.accepted);
    EXPECT_FALSE(outcome.unavailable);
    EXPECT_EQ(outcome.answered_by, "client");
}

TEST(PairConfirmation, AClientRefusalIsARefusal) {
    const ConfirmOutcome outcome = resolve_confirmation("123456", [](const std::string&) { return false; }, nullptr);
    EXPECT_FALSE(outcome.accepted);
    EXPECT_FALSE(outcome.unavailable);
}

TEST(PairConfirmation, NobodyToAskIsNotARefusal) {
    const ConfirmOutcome outcome = resolve_confirmation("123456", nullptr, nullptr);
    EXPECT_FALSE(outcome.accepted);
    EXPECT_TRUE(outcome.unavailable);
    EXPECT_TRUE(outcome.answered_by.empty());
}

// The legacy order, kept behind pair_dialog_enabled.
TEST(PairConfirmation, AnAnsweringDialogKeepsTheClientOut) {
    int client_asked = 0;
    const ConfirmOutcome outcome = resolve_confirmation(
        "123456",
        [&client_asked](const std::string&) {
            ++client_asked;
            return true;
        },
        [](const std::string&, bool& unavailable) {
            unavailable = false;
            return false;
        });
    EXPECT_EQ(client_asked, 0);
    EXPECT_FALSE(outcome.accepted);
    EXPECT_EQ(outcome.answered_by, "dialog");
}

TEST(PairConfirmation, ADialogThatCannotShowFallsBackToTheClient) {
    const ConfirmOutcome outcome = resolve_confirmation(
        "123456", [](const std::string&) { return true; }, [](const std::string&, bool& unavailable) {
            unavailable = true;
            return false;
        });
    EXPECT_TRUE(outcome.accepted);
    EXPECT_EQ(outcome.answered_by, "client");
}

// tether-dialog dies in the dynamic loader with 127 when gtk-layer-shell is
// missing. Reading that as a refusal declined every pairing -- see issue #49.
TEST(DialogAnswer, OnlyAcceptRejectAndTimeoutAreAnswers) {
    EXPECT_TRUE(dialog_answered(W_EXITCODE(0, 0)));
    EXPECT_TRUE(dialog_answered(W_EXITCODE(1, 0)));
    EXPECT_TRUE(dialog_answered(W_EXITCODE(2, 0)));
}

TEST(DialogAnswer, ADialogThatCouldNotRunAnsweredNothing) {
    EXPECT_FALSE(dialog_answered(W_EXITCODE(3, 0)));   // bad args or no display
    EXPECT_FALSE(dialog_answered(W_EXITCODE(126, 0))); // not executable
    EXPECT_FALSE(dialog_answered(W_EXITCODE(127, 0))); // shared library missing
    EXPECT_FALSE(dialog_answered(W_EXITCODE(0, SIGSEGV)));
    EXPECT_FALSE(dialog_answered(W_EXITCODE(0, SIGABRT)));
}

// BlueZ reporting a failed authentication is terminal: polling for a bond after
// it only buries when the transaction actually broke -- see issue #49.
TEST(AuthenticationFailure, RecognizesBlueZErrorNames) {
    EXPECT_TRUE(is_authentication_failure("GDBus.Error:org.bluez.Error.AuthenticationFailed: Authentication Failed"));
    EXPECT_TRUE(is_authentication_failure("org.bluez.Error.AuthenticationRejected"));
    EXPECT_TRUE(is_authentication_failure("org.bluez.Error.AuthenticationCanceled"));
    EXPECT_TRUE(is_authentication_failure("org.bluez.Error.AuthenticationTimeout"));
}

// A refused profile connect can still be followed by a completed authentication,
// so those errors must keep the full wait.
TEST(AuthenticationFailure, IgnoresConnectAndTransportErrors) {
    EXPECT_FALSE(is_authentication_failure("org.bluez.Error.Failed: br-connection-refused"));
    EXPECT_FALSE(is_authentication_failure("org.bluez.Error.InProgress"));
    EXPECT_FALSE(is_authentication_failure(""));
}

// Explicit pair is the fallback; there is nothing further to fall back to.
TEST(FallbackPolicy, NeverRetriesExplicitPair) {
    EXPECT_FALSE(should_fall_back(AuthStrategy::ExplicitPair, /*paired=*/false, /*user_rejected=*/false));
    EXPECT_FALSE(should_fall_back(AuthStrategy::ExplicitPair, /*paired=*/true, /*user_rejected=*/false));
}

// Which transaction bonded has to survive to the issue report, since it is the
// one thing that distinguishes this failure from every other pairing failure.
TEST(PairResultJson, ReportsTheStrategyThatBonded) {
    PairResult result;
    result.success = true;
    result.status = "paired";
    result.auth_strategy_used = AuthStrategy::ExplicitPair;

    EXPECT_EQ(to_json(result)["auth_strategy_used"], "explicit-pair");

    result.auth_strategy_used = AuthStrategy::ConnectFirst;
    EXPECT_EQ(to_json(result)["auth_strategy_used"], "connect-first");
}

// --- A bond that is already there ------------------------------------------

namespace {

    BondHealth health_of(BondState state, const char* detail, bool needs_repair) {
        return BondHealth{state, detail, needs_repair};
    }

} // namespace

// The incident: bt_pair answered already_paired for a bond made elsewhere with
// BR/EDR keys only, and the pairing "succeeded" without a code.
TEST(ExistingBondPlan, APartialBondIsRefusedNotReused) {
    const auto partial = health_of(BondState::Partial, BOND_DETAIL_BREDR_ONLY, true);
    EXPECT_EQ(plan_existing_bond(partial, false), ExistingBond::Refuse);
}

TEST(ExistingBondPlan, ABrokenBondIsRefused) {
    const auto broken = health_of(BondState::Broken, BOND_DETAIL_AUTH_FAILURES, true);
    EXPECT_EQ(plan_existing_bond(broken, false), ExistingBond::Refuse);
}

TEST(ExistingBondPlan, AHealthyBondIsReused) {
    EXPECT_EQ(plan_existing_bond(health_of(BondState::Ok, "", false), false), ExistingBond::Reuse);
    // BR/EDR only on a machine that cannot carry LE: all there will ever be.
    EXPECT_EQ(plan_existing_bond(health_of(BondState::Partial, BOND_DETAIL_BREDR_ONLY, false), false),
              ExistingBond::Reuse);
}

TEST(ExistingBondPlan, FreshReplacesWhateverIsThere) {
    EXPECT_EQ(plan_existing_bond(health_of(BondState::Ok, "", false), true), ExistingBond::Replace);
    EXPECT_EQ(plan_existing_bond(health_of(BondState::Partial, BOND_DETAIL_BREDR_ONLY, true), true),
              ExistingBond::Replace);
    EXPECT_EQ(plan_existing_bond(health_of(BondState::Broken, BOND_DETAIL_KEY_MISSING, true), true),
              ExistingBond::Replace);
    // Nothing to remove.
    EXPECT_EQ(plan_existing_bond(BondHealth{}, true), ExistingBond::Reuse);
}

TEST(PairResultJson, AStaleBondIsAFailureWithAStableReason) {
    PairResult result;
    result.status = PAIR_STATUS_STALE_BOND;
    result.bond = health_of(BondState::Partial, BOND_DETAIL_BREDR_ONLY, true);

    const auto json = to_json(result);
    EXPECT_EQ(json["command"], "bt_pair_result");
    EXPECT_EQ(json["success"], false);
    EXPECT_EQ(json["status"], "stale_bond");
    EXPECT_EQ(json["reason"], "stale_bond");
    EXPECT_EQ(json["bond_state"], "partial");
    EXPECT_EQ(json["bond_detail"], "bredr_only");
    EXPECT_EQ(json["needs_repair"], true);
}

TEST(PairResultJson, ReasonIsEmptyOnSuccessAndTheStatusOtherwise) {
    PairResult ok;
    ok.success = true;
    ok.status = "paired";
    ok.bond = health_of(BondState::Ok, "", false);
    EXPECT_EQ(to_json(ok)["reason"], "");
    EXPECT_EQ(to_json(ok)["bond_state"], "ok");
    EXPECT_EQ(to_json(ok)["needs_repair"], false);

    for (const char* status : {"not_found", "rejected", "timeout", "busy", "error"}) {
        PairResult failed;
        failed.status = status;
        EXPECT_EQ(to_json(failed)["reason"], status);
        EXPECT_EQ(to_json(failed)["bond_state"], "none");
    }
}

TEST(Unpair, RemovingTheSelectedIphoneClearsTheSelection) {
    PairResult removal;
    removal.success = true;
    removal.status = "unpaired";
    removal.bond = health_of(BondState::Broken, BOND_DETAIL_AUTH_FAILURES, true);

    const PairResult done = finish_unpair(removal, true);
    EXPECT_TRUE(done.success);
    EXPECT_TRUE(done.removed);
    EXPECT_TRUE(done.selection_cleared);
    EXPECT_EQ(done.bond.state, BondState::None);
}

TEST(Unpair, RemovingAnotherDeviceLeavesTheSelectionAlone) {
    PairResult removal;
    removal.success = true;
    removal.status = "unpaired";

    const PairResult done = finish_unpair(removal, false);
    EXPECT_TRUE(done.removed);
    EXPECT_FALSE(done.selection_cleared);
}

// BlueZ already forgot the phone, but the daemon still selects and supervises it.
TEST(Unpair, ASelectionLeftBehindIsStillCleared) {
    PairResult removal;
    removal.status = "not_found";

    const PairResult done = finish_unpair(removal, true);
    EXPECT_TRUE(done.success);
    EXPECT_EQ(done.status, "unpaired");
    EXPECT_FALSE(done.removed);
    EXPECT_TRUE(done.selection_cleared);
    EXPECT_EQ(to_json(done)["reason"], "");

    const PairResult unknown = finish_unpair(removal, false);
    EXPECT_FALSE(unknown.success);
    EXPECT_EQ(unknown.status, "not_found");
    EXPECT_FALSE(unknown.selection_cleared);
}

// A removal BlueZ refused must not drop the selection: the bond is still there.
TEST(Unpair, AFailedRemovalKeepsTheSelection) {
    PairResult removal;
    removal.status = "error";
    removal.bond = health_of(BondState::Partial, BOND_DETAIL_BREDR_ONLY, true);

    const PairResult done = finish_unpair(removal, true);
    EXPECT_EQ(done.bond.state, BondState::Partial) << "still there, and still reported";
    EXPECT_FALSE(done.success);
    EXPECT_FALSE(done.removed);
    EXPECT_FALSE(done.selection_cleared);
}

TEST(Unpair, AddressesCompareWhateverTheirCase) {
    EXPECT_TRUE(same_address("30:d5:aa:bb:cc:dd", "30:D5:AA:BB:CC:DD"));
    EXPECT_FALSE(same_address("30:D5:AA:BB:CC:DD", "30:D5:AA:BB:CC:DE"));
    EXPECT_FALSE(same_address("", "")) << "no selection matches nothing";
}
