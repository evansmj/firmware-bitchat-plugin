#include <unity.h>
#include "modules/BitChatBridgeModule.h"
#include <cstring>

// ============================================================================
// BitChatDuplicateCache Tests
// ============================================================================

void test_duplicate_cache_first_message(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.payloadLength = 5;
    memcpy(msg.payload, "test", 5);
    
    // First occurrence should not be duplicate
    bool isDupe = cache.isDuplicate(msg);
    TEST_ASSERT_FALSE(isDupe);
}

void test_duplicate_cache_add_and_detect(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.payloadLength = 11;
    memcpy(msg.payload, "Hello World", 11);
    
    // First time - not duplicate
    TEST_ASSERT_FALSE(cache.isDuplicate(msg));
    
    // Add to cache
    cache.addMessage(msg);
    
    // Second time - should be duplicate
    TEST_ASSERT_TRUE(cache.isDuplicate(msg));
}

void test_duplicate_cache_different_messages(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 5;
    memcpy(msg1.payload, "msg1", 5);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;
    msg2.setSenderId32(0x87654321);
    msg2.timestamp = 1234567891;
    msg2.payloadLength = 5;
    memcpy(msg2.payload, "msg2", 5);
    
    // Add first message
    cache.addMessage(msg1);
    
    // First message is duplicate
    TEST_ASSERT_TRUE(cache.isDuplicate(msg1));
    
    // Second message is not duplicate
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2));
    
    // Add second message
    cache.addMessage(msg2);
    
    // Now second is duplicate
    TEST_ASSERT_TRUE(cache.isDuplicate(msg2));
}

void test_duplicate_cache_same_sender_different_timestamp(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 5;
    memcpy(msg1.payload, "test", 5);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;
    msg2.setSenderId32(0x12345678);
    msg2.timestamp = 1234567891;  // Different timestamp
    msg2.payloadLength = 5;
    memcpy(msg2.payload, "test", 5);
    
    cache.addMessage(msg1);
    
    // Same sender but different timestamp - not duplicate
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2));
}

void test_duplicate_cache_same_content_different_sender(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 5;
    memcpy(msg1.payload, "test", 5);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;
    msg2.setSenderId32(0x87654321);  // Different sender
    msg2.timestamp = 1234567890;
    msg2.payloadLength = 5;
    memcpy(msg2.payload, "test", 5);
    
    cache.addMessage(msg1);
    
    // Same content but different sender - not duplicate
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2));
}

void test_duplicate_cache_different_message_types(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_ANNOUNCE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 5;
    memcpy(msg1.payload, "test", 5);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;  // Different type
    msg2.setSenderId32(0x12345678);
    msg2.timestamp = 1234567890;
    msg2.payloadLength = 5;
    memcpy(msg2.payload, "test", 5);
    
    cache.addMessage(msg1);
    
    // Different type - not duplicate
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2));
}

void test_duplicate_cache_overflow(void) {
    BitChatDuplicateCache cache;
    
    // Add more messages than cache size
    for (int i = 0; i < BITCHAT_DUPLICATE_CACHE_SIZE + 10; i++) {
        BitChatMessage msg;
        msg.type = BITCHAT_MSG_MESSAGE;
        msg.setSenderId32(0x10000000 + i);
        msg.timestamp = 1234567890 + i;
        msg.payloadLength = 5;
        memcpy(msg.payload, "test", 5);
        
        cache.addMessage(msg);
    }
    
    // The first message should have been evicted (circular buffer)
    BitChatMessage firstMsg;
    firstMsg.type = BITCHAT_MSG_MESSAGE;
    firstMsg.setSenderId32(0x10000000);
    firstMsg.timestamp = 1234567890;
    firstMsg.payloadLength = 5;
    memcpy(firstMsg.payload, "test", 5);
    
    // Should not be in cache anymore
    TEST_ASSERT_FALSE(cache.isDuplicate(firstMsg));
    
    // But recent messages should still be there
    BitChatMessage recentMsg;
    recentMsg.type = BITCHAT_MSG_MESSAGE;
    recentMsg.setSenderId32(0x10000000 + BITCHAT_DUPLICATE_CACHE_SIZE + 9);
    recentMsg.timestamp = 1234567890 + BITCHAT_DUPLICATE_CACHE_SIZE + 9;
    recentMsg.payloadLength = 5;
    memcpy(recentMsg.payload, "test", 5);
    
    TEST_ASSERT_TRUE(cache.isDuplicate(recentMsg));
}

void test_duplicate_cache_hash_calculation(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 11;
    memcpy(msg1.payload, "Hello World", 11);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;
    msg2.setSenderId32(0x12345678);
    msg2.timestamp = 1234567890;
    msg2.payloadLength = 11;
    memcpy(msg2.payload, "Hello World", 11);
    
    // Calculate hashes
    uint32_t hash1 = cache.calculateHash(msg1);
    uint32_t hash2 = cache.calculateHash(msg2);
    
    // Identical messages should have identical hashes
    TEST_ASSERT_EQUAL(hash1, hash2);
}

void test_duplicate_cache_hash_different_payloads(void) {
    BitChatDuplicateCache cache;
    
    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1234567890;
    msg1.payloadLength = 11;
    memcpy(msg1.payload, "Hello World", 11);
    
    BitChatMessage msg2;
    msg2.type = BITCHAT_MSG_MESSAGE;
    msg2.setSenderId32(0x12345678);
    msg2.timestamp = 1234567890;
    msg2.payloadLength = 12;
    memcpy(msg2.payload, "Hello World!", 12);
    
    // Calculate hashes
    uint32_t hash1 = cache.calculateHash(msg1);
    uint32_t hash2 = cache.calculateHash(msg2);
    
    // Different payloads should have different hashes
    TEST_ASSERT_NOT_EQUAL(hash1, hash2);
}

void test_duplicate_cache_empty_payload(void) {
    BitChatDuplicateCache cache;

    BitChatMessage msg;
    msg.type = BITCHAT_MSG_PING;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.payloadLength = 0;

    // Should handle empty payload without crashing
    TEST_ASSERT_FALSE(cache.isDuplicate(msg));
    cache.addMessage(msg);
    TEST_ASSERT_TRUE(cache.isDuplicate(msg));
}

// Two messages whose timestamps differ ONLY in the high 32 bits (same low 32 bits).
// The old cache truncated the timestamp to 32 bits and would wrongly treat these as the
// same message. With the full 64-bit timestamp they must be distinct.
void test_duplicate_cache_high_bits_timestamp(void) {
    BitChatDuplicateCache cache;

    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 0x0000000100000005ULL; // low32 = 0x00000005
    msg1.payloadLength = 4;
    memcpy(msg1.payload, "abc", 4);

    BitChatMessage msg2 = msg1;
    msg2.timestamp = 0x0000000200000005ULL; // same low32, different high32

    cache.addMessage(msg1);
    TEST_ASSERT_TRUE(cache.isDuplicate(msg1));   // exact same message
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2));  // differs in high timestamp bits
}

// With the fuzzy ±5 "near-duplicate" window removed, two messages a few ms apart are
// treated as distinct (exact-match dedup on the full fingerprint).
void test_duplicate_cache_near_timestamps_distinct(void) {
    BitChatDuplicateCache cache;

    BitChatMessage msg1;
    msg1.type = BITCHAT_MSG_MESSAGE;
    msg1.setSenderId32(0x12345678);
    msg1.timestamp = 1700000000000ULL;
    msg1.payloadLength = 5;
    memcpy(msg1.payload, "same", 5);

    BitChatMessage msg2 = msg1;
    msg2.timestamp = 1700000000003ULL; // 3 ms later, same content

    cache.addMessage(msg1);
    TEST_ASSERT_FALSE(cache.isDuplicate(msg2)); // not a duplicate anymore
    cache.addMessage(msg2);
    TEST_ASSERT_TRUE(cache.isDuplicate(msg2));  // exact retransmit is a duplicate
}


