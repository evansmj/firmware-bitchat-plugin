#include "BitChatBridgeModule.h"
#include "configuration.h"
#include <cstring>
#include <algorithm>

// BitChat Duplicate Cache Implementation

uint32_t BitChatDuplicateCache::calculateHash(const BitChatMessage& msg)
{
    // Create a hash based on sender ID, timestamp, message type, and payload
    // This creates a unique fingerprint for each message
    
    struct HashData {
        uint32_t senderId;
        uint64_t timestamp;
        uint8_t messageType;
        uint8_t payloadLength;
        uint8_t payload[BITCHAT_MAX_PAYLOAD_SIZE];
    } __attribute__((packed));

    HashData hashData = {};
    hashData.senderId = msg.getSenderId32();
    // Hash over the full 64-bit millisecond timestamp (no truncation)
    hashData.timestamp = msg.timestamp;
    hashData.messageType = msg.type;
    hashData.payloadLength = msg.payloadLength;
    
    if (msg.payloadLength > 0) {
        memcpy(hashData.payload, msg.payload, 
               std::min(static_cast<size_t>(msg.payloadLength), sizeof(hashData.payload)));
    }
    
    // Calculate simple hash
    size_t dataSize = sizeof(hashData.senderId) + sizeof(hashData.timestamp) + 
                     sizeof(hashData.messageType) + sizeof(hashData.payloadLength) + 
                     msg.payloadLength;
    
    // Simple hash algorithm (FNV-1a variant)
    uint32_t hash = 2166136261u;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&hashData);
    for (size_t i = 0; i < dataSize; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    
    return hash;
}

bool BitChatDuplicateCache::isDuplicate(const BitChatMessage& msg)
{
    uint32_t hash = calculateHash(msg);
    uint32_t msgSenderId = msg.getSenderId32();

    // Exact match on (sender, full timestamp, type, content hash) is the correct
    // dedup key — the hash already covers the payload, so any real retransmission
    // of the same message produces the same fingerprint.
    for (size_t i = 0; i < cache.size(); i++) {
        const auto& entry = cache[i];

        if (entry.senderId == msgSenderId &&
            entry.timestamp == msg.timestamp &&
            entry.messageType == msg.type &&
            entry.hash == hash) {

            LOG_DEBUG("BitChat: Duplicate message detected - sender=0x%08x, timestamp=%llu, type=0x%02x",
                     msgSenderId, (unsigned long long)msg.timestamp, msg.type);
            return true;
        }
    }

    return false;
}

void BitChatDuplicateCache::addMessage(const BitChatMessage& msg)
{
    // Add message to cache using circular buffer
    auto& entry = cache[currentIndex];
    
    entry.senderId = msg.getSenderId32();
    entry.timestamp = msg.timestamp;
    entry.messageType = msg.type;
    entry.hash = calculateHash(msg);

    // Move to next cache slot
    currentIndex = (currentIndex + 1) % cache.size();

    LOG_DEBUG("BitChat: Added message to cache - sender=0x%08x, timestamp=%llu, hash=0x%08x",
              entry.senderId, (unsigned long long)entry.timestamp, entry.hash);
}
