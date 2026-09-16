#include <Arduino.h>
#include <unity.h>
#include "TestUtil.h"

// Forward declarations for test functions from protocol_handler
void test_parse_announce_message(void);
void test_parse_message_too_short(void);
void test_parse_message_invalid_type(void);
void test_serialize_message(void);
void test_serialize_buffer_too_small(void);
void test_validate_message_valid(void);
void test_validate_message_invalid_type(void);
void test_validate_message_payload_too_large(void);
void test_roundtrip_serialize_parse(void);
void test_parse_all_message_types(void);
void test_parse_empty_payload(void);
void test_parse_max_payload(void);

// Forward declarations for test functions from duplicate_cache
void test_duplicate_cache_first_message(void);
void test_duplicate_cache_add_and_detect(void);
void test_duplicate_cache_different_messages(void);
void test_duplicate_cache_same_sender_different_timestamp(void);
void test_duplicate_cache_same_content_different_sender(void);
void test_duplicate_cache_different_message_types(void);
void test_duplicate_cache_overflow(void);
void test_duplicate_cache_hash_calculation(void);
void test_duplicate_cache_hash_different_payloads(void);
void test_duplicate_cache_empty_payload(void);

// Forward declarations for test functions from fragment_buffer
void test_fragment_buffer_add_single_fragment(void);
void test_fragment_buffer_multiple_fragments(void);
void test_fragment_buffer_get_reassembled(void);
void test_fragment_buffer_out_of_order(void);
void test_fragment_buffer_duplicate_fragment(void);
void test_fragment_buffer_cleanup_expired(void);
void test_fragment_buffer_max_payload(void);
void test_fragment_buffer_invalid_fragment_index(void);
void test_fragment_buffer_multiple_concurrent_assemblies(void);

// Forward declarations for duplicate cache 64-bit timestamp tests
void test_duplicate_cache_high_bits_timestamp(void);
void test_duplicate_cache_near_timestamps_distinct(void);

// Forward declarations for message ring (SPSC queue) tests
void test_ring_empty_on_init(void);
void test_ring_fifo_order(void);
void test_ring_full_drops_new(void);
void test_ring_wraparound(void);

// Forward declarations for time-sync helper tests
void test_should_adopt_rtc(void);
void test_announce_timestamp_prefers_rtc(void);
void test_announce_timestamp_uses_synced_base_when_no_rtc(void);
void test_announce_timestamp_no_double_count(void);
void test_announce_timestamp_millis_wraparound(void);

// Forward declarations for key derivation tests
void test_derive_seed_deterministic(void);
void test_derive_seed_domain_separation(void);
void test_derive_seed_changes_with_secret(void);
void test_ed25519_public_key_valid(void);
void test_noise_public_key_valid_x25519(void);

void setup()
{
    // Wait for hardware (especially important for embedded)
    delay(10);
    delay(2000);

    initializeTestEnvironment();
    
    UNITY_BEGIN();  // Start Unity test framework
    
    // ========================================================================
    // Protocol Handler Tests
    // ========================================================================
    RUN_TEST(test_parse_announce_message);
    RUN_TEST(test_parse_message_too_short);
    RUN_TEST(test_parse_message_invalid_type);
    RUN_TEST(test_serialize_message);
    RUN_TEST(test_serialize_buffer_too_small);
    RUN_TEST(test_validate_message_valid);
    RUN_TEST(test_validate_message_invalid_type);
    RUN_TEST(test_validate_message_payload_too_large);
    RUN_TEST(test_roundtrip_serialize_parse);
    RUN_TEST(test_parse_all_message_types);
    RUN_TEST(test_parse_empty_payload);
    RUN_TEST(test_parse_max_payload);
    
    // ========================================================================
    // Duplicate Cache Tests
    // ========================================================================
    RUN_TEST(test_duplicate_cache_first_message);
    RUN_TEST(test_duplicate_cache_add_and_detect);
    RUN_TEST(test_duplicate_cache_different_messages);
    RUN_TEST(test_duplicate_cache_same_sender_different_timestamp);
    RUN_TEST(test_duplicate_cache_same_content_different_sender);
    RUN_TEST(test_duplicate_cache_different_message_types);
    RUN_TEST(test_duplicate_cache_overflow);
    RUN_TEST(test_duplicate_cache_hash_calculation);
    RUN_TEST(test_duplicate_cache_hash_different_payloads);
    RUN_TEST(test_duplicate_cache_empty_payload);
    RUN_TEST(test_duplicate_cache_high_bits_timestamp);
    RUN_TEST(test_duplicate_cache_near_timestamps_distinct);

    // ========================================================================
    // Fragment Buffer Tests
    // ========================================================================
    RUN_TEST(test_fragment_buffer_add_single_fragment);
    RUN_TEST(test_fragment_buffer_multiple_fragments);
    RUN_TEST(test_fragment_buffer_get_reassembled);
    RUN_TEST(test_fragment_buffer_out_of_order);
    RUN_TEST(test_fragment_buffer_duplicate_fragment);
    RUN_TEST(test_fragment_buffer_cleanup_expired);
    RUN_TEST(test_fragment_buffer_max_payload);
    RUN_TEST(test_fragment_buffer_invalid_fragment_index);
    RUN_TEST(test_fragment_buffer_multiple_concurrent_assemblies);

    // ========================================================================
    // Message Ring (lock-free SPSC queue) Tests
    // ========================================================================
    RUN_TEST(test_ring_empty_on_init);
    RUN_TEST(test_ring_fifo_order);
    RUN_TEST(test_ring_full_drops_new);
    RUN_TEST(test_ring_wraparound);

    // ========================================================================
    // Time-sync Helper Tests
    // ========================================================================
    RUN_TEST(test_should_adopt_rtc);
    RUN_TEST(test_announce_timestamp_prefers_rtc);
    RUN_TEST(test_announce_timestamp_uses_synced_base_when_no_rtc);
    RUN_TEST(test_announce_timestamp_no_double_count);
    RUN_TEST(test_announce_timestamp_millis_wraparound);

    // ========================================================================
    // Key Derivation Tests
    // ========================================================================
    RUN_TEST(test_derive_seed_deterministic);
    RUN_TEST(test_derive_seed_domain_separation);
    RUN_TEST(test_derive_seed_changes_with_secret);
    RUN_TEST(test_ed25519_public_key_valid);
    RUN_TEST(test_noise_public_key_valid_x25519);

    exit(UNITY_END());  // Stop unit testing and exit
}

void loop()
{
    // Not used in testing
    delay(1000);
}

