#include <gtest/gtest.h>
#include <tether/bluetooth/advert.hpp>

#include <string>

using namespace tether::bluetooth;

// Apple's guidance for an accessory that wants the iPhone back: 20 ms for at
// least the first 30 s, then 152.5 ms. The advert used to carry no interval at
// all, which leaves the kernel's 1.28 s.
TEST(AdvertInterval, IsFastForThreeMinutesAfterTheLinkWentDown) {
    EXPECT_EQ(advert_interval_for(100, 100), ADVERT_INTERVAL_FAST);
    // 16 November trial: Bluetooth back on the phone 55 s after the loss.
    EXPECT_EQ(advert_interval_for(155, 100), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(279, 100), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(280, 100), ADVERT_INTERVAL_STEADY);
}

// Real absences last minutes or hours: the steady interval never gives way to
// anything slower.
TEST(AdvertInterval, StaysAtTheSteadyIntervalHoweverLongThePhoneIsAway) {
    for (int64_t away : {300, 3600, 8 * 3600, 3 * 86400})
        EXPECT_EQ(advert_interval_for(100 + away, 100), ADVERT_INTERVAL_STEADY) << away;
}

TEST(AdvertInterval, FastWindowCanBeSetFromTheEnvironment) {
    EXPECT_EQ(advert_fast_seconds_from(nullptr, 180), 180);
    EXPECT_EQ(advert_fast_seconds_from("", 180), 180);
    EXPECT_EQ(advert_fast_seconds_from("30", 180), 30);
    EXPECT_EQ(advert_fast_seconds_from("0", 180), 0);
    EXPECT_EQ(advert_fast_seconds_from("3600", 180), 3600);
    EXPECT_EQ(advert_fast_seconds_from("3601", 180), 180);
    EXPECT_EQ(advert_fast_seconds_from("1e3", 180), 180);
    EXPECT_EQ(advert_fast_seconds_from("-1", 180), 180);

    EXPECT_EQ(advert_interval_for(129, 100, 30), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(130, 100, 30), ADVERT_INTERVAL_STEADY);
    EXPECT_EQ(advert_interval_for(100, 100, 0), ADVERT_INTERVAL_STEADY);
}

// BR/EDR reaching the phone while LE is still down says the phone is here.
TEST(AdvertInterval, IsFastAgainWhenThePhoneIsSeenWithoutLe) {
    const int64_t lost = 100, seen = 5000;
    EXPECT_EQ(advert_interval_for(seen - 1, lost, ADVERT_FAST_SECONDS, -1), ADVERT_INTERVAL_STEADY);
    EXPECT_EQ(advert_interval_for(seen, lost, ADVERT_FAST_SECONDS, seen), ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(seen + ADVERT_SEEN_FAST_SECONDS - 1, lost, ADVERT_FAST_SECONDS, seen),
              ADVERT_INTERVAL_FAST);
    EXPECT_EQ(advert_interval_for(seen + ADVERT_SEEN_FAST_SECONDS, lost, ADVERT_FAST_SECONDS, seen),
              ADVERT_INTERVAL_STEADY);
}

// BlueZ retires the advert after 180 s. Waiting for its Release left the phone
// unsolicited until the supervising thread came round again.
TEST(AdvertRenewal, IsDueBeforeBluezRetiresTheAdvert) {
    EXPECT_FALSE(advert_renewal_due(0));
    EXPECT_FALSE(advert_renewal_due(ANCS_ADVERT_TIMEOUT_SECONDS - ADVERT_RENEW_MARGIN_SECONDS - 1));
    EXPECT_TRUE(advert_renewal_due(ANCS_ADVERT_TIMEOUT_SECONDS - ADVERT_RENEW_MARGIN_SECONDS));
    EXPECT_TRUE(advert_renewal_due(ANCS_ADVERT_TIMEOUT_SECONDS));
    EXPECT_GE(ADVERT_RENEW_MARGIN_SECONDS, 10) << "must outlast a tick held up by an OBEX call";
    EXPECT_LT(ADVERT_RENEW_MARGIN_SECONDS, ANCS_ADVERT_TIMEOUT_SECONDS / 2);
}

TEST(AdvertInterval, IsSteadyWhenNoLossIsOnRecord) {
    EXPECT_EQ(advert_interval_for(100, -1), ADVERT_INTERVAL_STEADY);
    // A clock that reads before the loss must not open a fast window.
    EXPECT_EQ(advert_interval_for(50, 100), ADVERT_INTERVAL_STEADY);
}

TEST(AdvertInterval, MatchesAppleGuidance) {
    EXPECT_GE(ADVERT_FAST_SECONDS, 30) << "Apple: at least 30 s";
    EXPECT_EQ(ADVERT_INTERVAL_FAST.min_ms, 20u);
    EXPECT_EQ(ADVERT_INTERVAL_FAST.max_ms, 20u);

    // BlueZ takes whole milliseconds and truncates to 0.625 ms slots.
    const auto slots = [](uint32_t ms) { return static_cast<uint32_t>(ms / 0.625); };
    EXPECT_EQ(slots(ADVERT_INTERVAL_FAST.min_ms) * 0.625, 20.0);
    EXPECT_EQ(slots(ADVERT_INTERVAL_STEADY.max_ms) * 0.625, 152.5);
    EXPECT_LE(slots(ADVERT_INTERVAL_STEADY.min_ms), slots(ADVERT_INTERVAL_STEADY.max_ms));
    EXPECT_GE(slots(ADVERT_INTERVAL_STEADY.min_ms) * 0.625, 151.0);
}

TEST(AdvertInterval, AnUnsetOrInvertedIntervalIsNoInterval) {
    EXPECT_FALSE(AdvertInterval{}.set());
    EXPECT_FALSE((AdvertInterval{20, 0}).set());
    EXPECT_FALSE((AdvertInterval{200, 100}).set());
    EXPECT_TRUE(ADVERT_INTERVAL_FAST.set());
    EXPECT_TRUE(ADVERT_INTERVAL_STEADY.set());
}

// BlueZ reads whatever the object declares. With no interval the object is
// exactly the one it was before, so the switch-off really is the old advert.
TEST(AdvertInterval, PropertiesAreDeclaredOnlyWhenAnIntervalIsSet) {
    const std::string plain = advert_introspection_xml({});
    EXPECT_EQ(plain.find("MinInterval"), std::string::npos);
    EXPECT_EQ(plain.find("MaxInterval"), std::string::npos);
    EXPECT_NE(plain.find("SolicitUUIDs"), std::string::npos);

    const std::string tuned = advert_introspection_xml(ADVERT_INTERVAL_FAST);
    EXPECT_NE(tuned.find("<property name='MinInterval' type='u' access='read'/>"), std::string::npos);
    EXPECT_NE(tuned.find("<property name='MaxInterval' type='u' access='read'/>"), std::string::npos);

    for (const std::string& xml : {plain, tuned}) {
        GError* error = nullptr;
        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(xml.c_str(), &error);
        ASSERT_NE(info, nullptr) << (error ? error->message : "");
        ASSERT_NE(info->interfaces, nullptr);
        EXPECT_STREQ(info->interfaces[0]->name, "org.bluez.LEAdvertisement1");
        g_dbus_node_info_unref(info);
    }
}

TEST(AdvertInterval, CanBeSwitchedOffFromTheEnvironment) {
    EXPECT_TRUE(advert_intervals_enabled(nullptr));
    EXPECT_TRUE(advert_intervals_enabled(""));
    EXPECT_TRUE(advert_intervals_enabled("1"));
    EXPECT_FALSE(advert_intervals_enabled("0"));
}
