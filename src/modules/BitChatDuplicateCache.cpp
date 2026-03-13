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
        uint64_t senderId;
        uint32_t timestamp;
        uint8_t messageType;
        uint8_t payloadLength;
        uint8_t payload[BITCHAT_MAX_PAYLOAD_SIZE];
    } __attribute__((packed));
    
    HashData hashData = {};
    hashData.senderId = msg.senderId;
    // Use lower 32 bits of timestamp for hash (for backward compatibility)
    hashData.timestamp = static_cast<uint32_t>(msg.timestamp & 0xFFFFFFFFULL);
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
    
    // Check if message already exists in cache
    for (size_t i = 0; i < cache.size(); i++) {
        const auto& entry = cache[i];
        
        uint64_t msgSenderId = msg.senderId;
        uint32_t msgTimestamp32 = static_cast<uint32_t>(msg.timestamp & 0xFFFFFFFFULL);
        
        // Check for exact match
        if (entry.senderId == msgSenderId && 
            entry.timestamp == msgTimestamp32 &&
            entry.messageType == msg.type &&
            entry.hash == hash) {
            
            LOG_DEBUG("BitChat: Duplicate message detected - sender=0x%08x, timestamp=%u, type=0x%02x",
                     msgSenderId, msgTimestamp32, msg.type);
            return true;
        }
        
        // Also check for messages that are very similar (potential retransmissions)
        // Allow small timestamp variations to handle clock skew
        if (entry.senderId == msgSenderId &&
            entry.messageType == msg.type &&
            entry.hash == hash &&
            abs(static_cast<int32_t>(entry.timestamp) - static_cast<int32_t>(msgTimestamp32)) <= 5) {
            
            LOG_DEBUG("BitChat: Near-duplicate message detected (timestamp skew) - sender=0x%08x", 
                     msgSenderId);
            return true;
        }
    }
    
    return false;
}

void BitChatDuplicateCache::addMessage(const BitChatMessage& msg)
{
    // Add message to cache using circular buffer
    auto& entry = cache[currentIndex];
    
    entry.senderId = msg.senderId;
    // Use lower 32 bits of timestamp for cache (for backward compatibility)
    entry.timestamp = static_cast<uint32_t>(msg.timestamp & 0xFFFFFFFFULL);
    entry.messageType = msg.type;
    entry.hash = calculateHash(msg);
    
    // Move to next cache slot
    currentIndex = (currentIndex + 1) % cache.size();
    
    LOG_DEBUG("BitChat: Added message to cache - sender=0x%08x, timestamp=%u, hash=0x%08x",
              entry.senderId, entry.timestamp, entry.hash);
}
