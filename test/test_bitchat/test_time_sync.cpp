#include <unity.h>
#include "modules/BitChatBridgeModule.h"

// ============================================================================
// Time-sync pure-helper tests (shouldAdoptRtc / computeAnnounceTimestampMs)
// ============================================================================

void test_should_adopt_rtc(void) {
    // RTCQualityNone must NOT be trusted; anything Device or better is fine.
    TEST_ASSERT_FALSE(BitChatBridgeModule::shouldAdoptRtc(RTCQualityNone));
    TEST_ASSERT_TRUE(BitChatBridgeModule::shouldAdoptRtc(RTCQualityDevice));
    TEST_ASSERT_TRUE(BitChatBridgeModule::shouldAdoptRtc(RTCQualityFromNet));
    TEST_ASSERT_TRUE(BitChatBridgeModule::shouldAdoptRtc(RTCQualityNTP));
    TEST_ASSERT_TRUE(BitChatBridgeModule::shouldAdoptRtc(RTCQualityGPS));
}

void test_announce_timestamp_prefers_rtc(void) {
    // When the RTC is valid it is authoritative, regardless of any synced base.
    uint64_t ts = BitChatBridgeModule::computeAnnounceTimestampMs(
        /*rtcValid=*/true, /*rtcTimeMs=*/1700000000000ULL,
        /*haveSyncedBase=*/true, /*syncedBaseMs=*/1500000000000ULL,
        /*syncedAtMillis=*/1000, /*nowMillis=*/6000);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ULL, ts);
}

void test_announce_timestamp_uses_synced_base_when_no_rtc(void) {
    // No valid RTC: advance the peer-learned base by elapsed millis().
    uint64_t ts = BitChatBridgeModule::computeAnnounceTimestampMs(
        /*rtcValid=*/false, /*rtcTimeMs=*/0,
        /*haveSyncedBase=*/true, /*syncedBaseMs=*/1700000000000ULL,
        /*syncedAtMillis=*/1000, /*nowMillis=*/6000);
    // elapsed = 6000 - 1000 = 5000 ms
    TEST_ASSERT_EQUAL_UINT64(1700000005000ULL, ts);
}

void test_announce_timestamp_no_double_count(void) {
    // Regression for the old offset scheme: once the RTC becomes valid the timestamp must
    // equal the RTC time exactly, NOT rtc + a previously-learned offset.
    uint64_t rtcNow = 1700000010000ULL;
    uint64_t ts = BitChatBridgeModule::computeAnnounceTimestampMs(
        /*rtcValid=*/true, /*rtcTimeMs=*/rtcNow,
        /*haveSyncedBase=*/true, /*syncedBaseMs=*/1700000000000ULL,
        /*syncedAtMillis=*/1000, /*nowMillis=*/11000);
    TEST_ASSERT_EQUAL_UINT64(rtcNow, ts); // no addition of the synced base
}

void test_announce_timestamp_millis_wraparound(void) {
    // millis() wrapped past 2^32 since the base was captured; uint32 subtraction still
    // yields the correct elapsed interval.
    uint32_t syncedAt = 0xFFFFFF00u;      // near uint32 max
    uint32_t now = 0x000000FFu;           // wrapped around
    uint32_t expectedElapsed = now - syncedAt; // 0x1FF == 511
    uint64_t base = 1700000000000ULL;
    uint64_t ts = BitChatBridgeModule::computeAnnounceTimestampMs(
        false, 0, true, base, syncedAt, now);
    TEST_ASSERT_EQUAL_UINT64(base + expectedElapsed, ts);
}
