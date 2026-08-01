#include <unity.h>
#include "modules/BitChatBridgeModule.h"
#include <cstring>
#include <ctime>

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

// ============================================================================
// BitChatProtocolHandler Tests
// ============================================================================

void test_parse_announce_message(void) {
    // Create a valid ANNOUNCE message buffer
    uint8_t buffer[64];
    size_t offset = 0;
    
    // Type
    buffer[offset++] = 0x01;  // ANNOUNCE
    
    // Sender ID (big-endian)
    buffer[offset++] = 0x12;
    buffer[offset++] = 0x34;
    buffer[offset++] = 0x56;
    buffer[offset++] = 0x78;
    
    // Timestamp (big-endian)
    uint32_t timestamp = 1234567890;
    buffer[offset++] = (timestamp >> 24) & 0xFF;
    buffer[offset++] = (timestamp >> 16) & 0xFF;
    buffer[offset++] = (timestamp >> 8) & 0xFF;
    buffer[offset++] = timestamp & 0xFF;
    
    // TTL
    buffer[offset++] = 60;
    
    // Payload length
    uint8_t payloadLen = 11;
    buffer[offset++] = payloadLen;
    
    // Payload
    memcpy(&buffer[offset], "Hello World", payloadLen);
    offset += payloadLen;
    
    // Parse message
    BitChatMessage msg;
    bool result = BitChatProtocolHandler::parseMessage(buffer, offset, msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(BITCHAT_MSG_ANNOUNCE, msg.type);
    TEST_ASSERT_EQUAL(0x12345678, msg.getSenderId32());
    TEST_ASSERT_EQUAL(1234567890, msg.timestamp);
    TEST_ASSERT_EQUAL(60, msg.ttl);
    TEST_ASSERT_EQUAL(11, msg.payloadLength);
    TEST_ASSERT_EQUAL_MEMORY("Hello World", msg.payload, 11);
}

void test_parse_message_too_short(void) {
    // Buffer too short (less than header size)
    uint8_t buffer[5] = {0x01, 0x12, 0x34, 0x56, 0x78};
    
    BitChatMessage msg;
    bool result = BitChatProtocolHandler::parseMessage(buffer, 5, msg);
    
    TEST_ASSERT_FALSE(result);
}

void test_parse_message_invalid_type(void) {
    // Create message with invalid type
    uint8_t buffer[BITCHAT_HEADER_SIZE + 10];
    buffer[0] = 0x99;  // Invalid type
    
    // Fill rest with valid data
    for (int i = 1; i < BITCHAT_HEADER_SIZE; i++) {
        buffer[i] = 0;
    }
    buffer[11] = 5;  // payload length
    
    BitChatMessage msg;
    bool result = BitChatProtocolHandler::parseMessage(buffer, BITCHAT_HEADER_SIZE + 5, msg);
    
    // Should parse but validation will fail
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_FALSE(BitChatProtocolHandler::validateMessage(msg));
}

void test_serialize_message(void) {
    // Create a message
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.ttl = 60;
    msg.payloadLength = 11;
    memcpy(msg.payload, "Hello World", 11);
    
    // Serialize
    uint8_t buffer[256];
    size_t size = BitChatProtocolHandler::serializeMessage(msg, buffer, 256);
    
    TEST_ASSERT_EQUAL(BITCHAT_HEADER_SIZE + 11, size);
    
    // Verify header
    TEST_ASSERT_EQUAL(0x02, buffer[0]);  // MESSAGE type
    TEST_ASSERT_EQUAL(0x12, buffer[1]);  // Sender ID MSB
    TEST_ASSERT_EQUAL(0x34, buffer[2]);
    TEST_ASSERT_EQUAL(0x56, buffer[3]);
    TEST_ASSERT_EQUAL(0x78, buffer[4]);  // Sender ID LSB
    
    // Verify payload length
    TEST_ASSERT_EQUAL(11, buffer[11]);
    
    // Verify payload
    TEST_ASSERT_EQUAL_MEMORY("Hello World", &buffer[12], 11);
}

void test_serialize_buffer_too_small(void) {
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.ttl = 60;
    msg.payloadLength = 100;
    
    // Buffer too small
    uint8_t buffer[50];
    size_t size = BitChatProtocolHandler::serializeMessage(msg, buffer, 50);
    
    TEST_ASSERT_EQUAL(0, size);  // Should fail
}

void test_validate_message_valid(void) {
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.ttl = 60;
    msg.payloadLength = 10;
    
    bool valid = BitChatProtocolHandler::validateMessage(msg);
    TEST_ASSERT_TRUE(valid);
}

void test_validate_message_invalid_type(void) {
    BitChatMessage msg;
    msg.type = 0x99;  // Invalid type
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.ttl = 60;
    msg.payloadLength = 10;
    
    bool valid = BitChatProtocolHandler::validateMessage(msg);
    TEST_ASSERT_FALSE(valid);
}

void test_validate_message_payload_too_large(void) {
    BitChatMessage msg;
    msg.type = BITCHAT_MSG_MESSAGE;
    msg.setSenderId32(0x12345678);
    msg.timestamp = 1234567890;
    msg.ttl = 60;
    msg.payloadLength = BITCHAT_MAX_PAYLOAD_SIZE + 1;  // Too large
    
    bool valid = BitChatProtocolHandler::validateMessage(msg);
    TEST_ASSERT_FALSE(valid);
}

void test_roundtrip_serialize_parse(void) {
    // Create original message
    BitChatMessage original;
    original.type = BITCHAT_MSG_ANNOUNCE;
    original.setSenderId32(0xABCDEF12);
    original.timestamp = 1700000000;
    original.ttl = 120;
    original.payloadLength = 20;
    memcpy(original.payload, "Meshtastic: TestNode", 20);
    
    // Serialize
    uint8_t buffer[256];
    size_t size = BitChatProtocolHandler::serializeMessage(original, buffer, 256);
    TEST_ASSERT_TRUE(size > 0);
    
    // Parse
    BitChatMessage parsed;
    bool result = BitChatProtocolHandler::parseMessage(buffer, size, parsed);
    TEST_ASSERT_TRUE(result);
    
    // Verify roundtrip
    TEST_ASSERT_EQUAL(original.type, parsed.type);
    TEST_ASSERT_EQUAL(original.getSenderId32(), parsed.getSenderId32());
    TEST_ASSERT_EQUAL(original.timestamp, parsed.timestamp);
    TEST_ASSERT_EQUAL(original.ttl, parsed.ttl);
    TEST_ASSERT_EQUAL(original.payloadLength, parsed.payloadLength);
    TEST_ASSERT_EQUAL_MEMORY(original.payload, parsed.payload, original.payloadLength);
}

void test_parse_all_message_types(void) {
    uint8_t types[] = {
        BITCHAT_MSG_ANNOUNCE,
        BITCHAT_MSG_MESSAGE,
        BITCHAT_MSG_LEAVE,
        BITCHAT_MSG_IDENTITY,
        BITCHAT_MSG_CHANNEL,
        BITCHAT_MSG_PING,
        BITCHAT_MSG_PONG
    };
    
    for (size_t i = 0; i < sizeof(types); i++) {
        uint8_t buffer[BITCHAT_HEADER_SIZE + 10];
        memset(buffer, 0, sizeof(buffer));
        
        buffer[0] = types[i];
        buffer[1] = 0x12;
        buffer[2] = 0x34;
        buffer[3] = 0x56;
        buffer[4] = 0x78;
        buffer[11] = 5;  // payload length
        memcpy(&buffer[12], "test", 4);
        
        BitChatMessage msg;
        bool result = BitChatProtocolHandler::parseMessage(buffer, BITCHAT_HEADER_SIZE + 5, msg);
        
        TEST_ASSERT_TRUE(result);
        TEST_ASSERT_EQUAL(types[i], msg.type);
        TEST_ASSERT_TRUE(BitChatProtocolHandler::validateMessage(msg));
    }
}

void test_parse_empty_payload(void) {
    uint8_t buffer[BITCHAT_HEADER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    buffer[0] = BITCHAT_MSG_PING;
    buffer[11] = 0;  // Empty payload
    
    BitChatMessage msg;
    bool result = BitChatProtocolHandler::parseMessage(buffer, BITCHAT_HEADER_SIZE, msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(BITCHAT_MSG_PING, msg.type);
    TEST_ASSERT_EQUAL(0, msg.payloadLength);
    TEST_ASSERT_TRUE(BitChatProtocolHandler::validateMessage(msg));
}

void test_parse_max_payload(void) {
    size_t totalSize = BITCHAT_HEADER_SIZE + BITCHAT_MAX_PAYLOAD_SIZE;
    uint8_t* buffer = (uint8_t*)malloc(totalSize);
    memset(buffer, 0, totalSize);
    
    buffer[0] = BITCHAT_MSG_MESSAGE;
    buffer[11] = BITCHAT_MAX_PAYLOAD_SIZE;
    
    // Fill payload with test data
    for (int i = 0; i < BITCHAT_MAX_PAYLOAD_SIZE; i++) {
        buffer[BITCHAT_HEADER_SIZE + i] = (i % 26) + 'A';
    }
    
    BitChatMessage msg;
    bool result = BitChatProtocolHandler::parseMessage(buffer, totalSize, msg);
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(BITCHAT_MAX_PAYLOAD_SIZE, msg.payloadLength);
    TEST_ASSERT_TRUE(BitChatProtocolHandler::validateMessage(msg));
    
    free(buffer);
}


