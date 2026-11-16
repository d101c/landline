#include <gtest/gtest.h>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/pending_connect.hpp>

#include <string>

using namespace tether::bluetooth;

// The BR/EDR connect no longer waits for BlueZ: the request is recorded, the
// supervising thread moves on, and the answer is picked up on a later tick.
TEST(PendingConnect, FirstRequestIsIssuedAndTheNextOnesStandBack) {
    PendingConnect connect;
    std::string err;

    EXPECT_EQ(connect.begin(err), ConnectResult::Requested);
    EXPECT_TRUE(connect.pending());
    EXPECT_EQ(connect.begin(err), ConnectResult::Busy);
    EXPECT_EQ(connect.begin(err), ConnectResult::Busy);
    EXPECT_TRUE(err.empty());
}

TEST(PendingConnect, AnAcceptedCallLeavesNothingToReport) {
    PendingConnect connect;
    std::string err;
    ASSERT_EQ(connect.begin(err), ConnectResult::Requested);

    connect.finish();
    EXPECT_FALSE(connect.pending());
    EXPECT_EQ(connect.begin(err), ConnectResult::Requested);
    EXPECT_TRUE(err.empty());
}

TEST(PendingConnect, ARefusalIsReportedOnceThenANewCallCanStart) {
    PendingConnect connect;
    std::string err;
    ASSERT_EQ(connect.begin(err), ConnectResult::Requested);

    connect.finish("org.bluez.Error.Failed: br-connection-refused");
    EXPECT_EQ(connect.begin(err), ConnectResult::Failed);
    EXPECT_EQ(err, "org.bluez.Error.Failed: br-connection-refused");
    EXPECT_FALSE(connect.pending());

    err.clear();
    EXPECT_EQ(connect.begin(err), ConnectResult::Requested);
    EXPECT_TRUE(err.empty());
}

// The link came back another way (the phone, or obexd opening it): what the
// abandoned attempt said must not be charged to the next outage.
TEST(PendingConnect, ALinkThatCameUpDiscardsTheStaleError) {
    PendingConnect connect;
    std::string err;
    ASSERT_EQ(connect.begin(err), ConnectResult::Requested);
    connect.finish("org.bluez.Error.Failed: page-timeout");

    connect.discard_error();
    EXPECT_EQ(connect.begin(err), ConnectResult::Requested);
    EXPECT_TRUE(err.empty());
}

TEST(ClassicReply, SuccessReportsNothing) {
    EXPECT_EQ(classify_classic_reply(true, true, false, ""), ClassicReply{});
    EXPECT_EQ(classify_classic_reply(false, true, false, ""), ClassicReply{});
}

// "Timeout was reached" is GLib giving up, not the iPhone refusing: BlueZ is
// still connecting, and counting it as a refusal ends in advice to toggle
// Bluetooth on a phone that is fine.
TEST(ClassicReply, ADBusTimeoutIsNotARefusal) {
    EXPECT_EQ(classify_classic_reply(true, false, true, "Timeout was reached"), ClassicReply{});
    EXPECT_EQ(classify_classic_reply(false, false, true, "Timeout was reached"), ClassicReply{});
}

TEST(ClassicReply, ABearerRefusalFallsBackToDeviceConnect) {
    const ClassicReply reply = classify_classic_reply(true, false, false, "org.bluez.Error.Failed: br-connection-refused");
    EXPECT_TRUE(reply.fall_back);
}

// Already connecting or already connected: a second connect would only collide.
TEST(ClassicReply, ABearerAlreadyAtWorkDoesNotFallBack) {
    for (const char* message : {"org.bluez.Error.InProgress: Operation already in progress",
                                "org.bluez.Error.AlreadyConnected: Already Connected"}) {
        const ClassicReply reply = classify_classic_reply(true, false, false, message);
        EXPECT_FALSE(reply.fall_back) << message;
        EXPECT_EQ(reply.error, message);
    }
}

TEST(ClassicReply, TheBearerRefusalIsTheOneReportedWhenBothFail) {
    EXPECT_EQ(classify_classic_reply(false, false, false, "device said no", "bearer said no").error, "bearer said no");
    EXPECT_EQ(classify_classic_reply(false, false, false, "device said no").error, "device said no");
}

namespace {

    // A phone whose BR/EDR connect is answered later, the way BlueZ does.
    class AsyncBearer : public BearerOps {
    public:
        PendingConnect connect;
        bool classic = false;
        bool le = true;
        int issued = 0;

        bool device_present() const override { return true; }
        bool device_paired() const override { return true; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return true; }
        bool le_connected() const override { return le; }
        std::string preferred_bearer() const override { return "last-seen"; }
        void set_preferred_bearer(const std::string&) override {}
        ConnectResult connect_classic(std::string& err) override {
            const ConnectResult result = connect.begin(err);
            if (result == ConnectResult::Requested)
                ++issued;
            return result;
        }
        bool classic_connect_outstanding() const override { return connect.pending(); }
        ConnectResult connect_le(std::string&) override { return ConnectResult::Requested; }
        bool le_connect_outstanding() const override { return false; }
        bool solicitation_on_air() const override { return false; }
        bool telephony_present() const override { return true; }
        bool disconnect_classic(std::string&) override { return true; }
    };

} // namespace

// The measured outage: BlueZ sat on the connect for 30 s. The supervisor must
// keep ticking through it, issue the connect exactly once, and count no refusal.
TEST(PendingConnect, TheSupervisorKeepsTickingWhileBlueZIsSilent) {
    AsyncBearer ops;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now < 30; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.issued, 1);
    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected) << "LE must stay reported while BR/EDR is still being reached";
    EXPECT_EQ(sup.status().reason.find("keeps refusing"), std::string::npos);

    // The D-Bus call expires without an answer: nothing to report, and the
    // link, which came back through another path, is taken as it is.
    ops.connect.finish(classify_classic_reply(true, false, true, "Timeout was reached").error);
    ops.classic = true;
    sup.tick(30);
    EXPECT_TRUE(sup.status().classic_connected);
    EXPECT_EQ(ops.issued, 1);
}

TEST(PendingConnect, ARefusalReachesTheSupervisorOnALaterTick) {
    AsyncBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    ASSERT_EQ(ops.issued, 1);
    ops.connect.finish("org.bluez.Error.Failed: br-connection-refused");

    sup.tick(1);
    EXPECT_EQ(ops.issued, 1);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);

    for (int64_t now = 2; now <= 1 + BEARER_BACKOFF_MIN_SECONDS; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.issued, 2) << "a refused connect must be retried once the backoff has run";
}

// An absent phone must not be paged every few seconds: each refusal arrives a
// tick late, and the backoff has to keep growing across the attempts all the
// same, up to the ceiling for a phone that does not answer.
TEST(PendingConnect, BackoffKeepsGrowingAcrossLateRefusals) {
    AsyncBearer ops;
    ops.le = false;
    BearerSupervisor sup(ops, true);

    int previous_backoff = 0;
    for (int64_t now = 0; now < 600; ++now) {
        sup.tick(now);
        if (ops.connect.pending())
            ops.connect.finish("org.bluez.Error.Failed: br-connection-page-timeout");
        EXPECT_GE(sup.status().classic_backoff, previous_backoff) << "backoff restarted at t=" << now;
        previous_backoff = sup.status().classic_backoff;
    }
    EXPECT_EQ(sup.status().classic_backoff, ABSENT_CLASSIC_BACKOFF_MAX_SECONDS);
    EXPECT_LE(ops.issued, 16);
}

// Six refusals in a row is what turns "connecting" into advice. They only
// count if an attempt that was merely issued does not wipe the tally.
TEST(PendingConnect, LateRefusalsStillAddUpToTheAdvice) {
    AsyncBearer ops;
    ops.le = false;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now < 600; ++now) {
        sup.tick(now);
        if (ops.connect.pending())
            ops.connect.finish("org.bluez.Error.Failed: br-connection-refused");
    }
    EXPECT_GE(ops.issued, CLASSIC_FAILURES_BEFORE_ADVICE);
    EXPECT_NE(sup.status().reason.find("keeps refusing"), std::string::npos) << sup.status().reason;
}

// While BlueZ has not answered, the wait is not a failure and not a reason to
// stand further back: the backoff is left alone.
TEST(PendingConnect, AnOutstandingConnectDoesNotGrowTheBackoff) {
    AsyncBearer ops;
    BearerSupervisor sup(ops, true);

    for (int64_t now = 0; now < 40; ++now)
        sup.tick(now);
    EXPECT_EQ(ops.issued, 1);
    EXPECT_EQ(sup.status().classic_backoff, 0);
}
