#include "BitChatBridgeModule.h"
#include "configuration.h"
#include "meshUtils.h"
#include "mesh/MeshService.h"
#include "mesh/Router.h"
#include "NodeDB.h"
#include "RTC.h"
#include <cstring>
#include <pb_encode.h>
#include <pb_decode.h>

// BitChat Protocol Handler Implementation

bool BitChatProtocolHandler::parseMessage(const uint8_t* data, size_t length, BitChatMessage& msg)
{
    if (!data || length < 1) {
        LOG_WARN("BitChat: Invalid message - empty");
        return false;
    }

    // Peak at version (Byte 0)
    uint8_t version = data[0];
    size_t headerSize = 0;
    
    if (version == BITCHAT_VERSION_1) {
        headerSize = BITCHAT_HEADER_SIZE_V1;
    } else if (version == BITCHAT_VERSION_2) {
        headerSize = BITCHAT_HEADER_SIZE_V2;
    } else {
        LOG_WARN("BitChat: Unsupported version %d", version);
        return false;
    }
    
    if (length < headerSize) {
        LOG_WARN("BitChat: Invalid message - too short for header (%d < %d)", length, headerSize);
        return false;
    }

    // Parse Fixed Header
    size_t offset = 0;
    msg.version = data[offset++];
    msg.type = data[offset++];
    msg.ttl = data[offset++];
    
    // Timestamp (8 bytes)
    msg.timestamp = 0;
    for (int i = 0; i < 8; i++) {
        msg.timestamp = (msg.timestamp << 8) | static_cast<uint64_t>(data[offset++]);
    }
    
    msg.flags = data[offset++];
    
    // Payload Length
    if (version == BITCHAT_VERSION_2) {
        msg.payloadLength = (static_cast<uint32_t>(data[offset]) << 24) |
                            (static_cast<uint32_t>(data[offset + 1]) << 16) |
                            (static_cast<uint32_t>(data[offset + 2]) << 8) |
                            static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
    } else {
        msg.payloadLength = (static_cast<uint16_t>(data[offset]) << 8) | static_cast<uint16_t>(data[offset + 1]);
        offset += 2;
    }
    
    bool hasRecipient = (msg.flags & BITCHAT_FLAG_HAS_RECIPIENT) != 0;
    bool hasSignature = (msg.flags & BITCHAT_FLAG_HAS_SIGNATURE) != 0;
    bool hasRoute = (version >= BITCHAT_VERSION_2) && ((msg.flags & BITCHAT_FLAG_HAS_ROUTE) != 0);
    bool isCompressed = (msg.flags & BITCHAT_FLAG_IS_COMPRESSED) != 0;
    
    // Validate minimum remaining length
    // SenderID (8) + RecipientID (opt 8) + Route (opt var) + Payload + Signature (opt 64)
    size_t minRemaining = 8 + (hasRecipient ? 8 : 0) + (hasSignature ? BITCHAT_SIGNATURE_SIZE : 0) + msg.payloadLength;
    if (length - offset < minRemaining) {
        LOG_WARN("BitChat: Message truncated (expected at least %d more bytes, got %d)", minRemaining, length - offset);
        return false;
    }
    
    // Sender ID
    memcpy(&msg.senderId, data + offset, sizeof(msg.senderId));
    offset += sizeof(msg.senderId);
    
    // Recipient ID
    if (hasRecipient) {
        memcpy(&msg.recipientId, data + offset, sizeof(msg.recipientId));
        offset += sizeof(msg.recipientId);
    } else {
        msg.recipientId = 0;
    }
    
    // Source Route (V2 only)
    msg.routeCount = 0;
    if (hasRoute) {
        // Read Count (1 byte)
        if (offset >= length) return false;
        uint8_t count = data[offset++];
        
        if (count > BITCHAT_MAX_HOPS) {
             LOG_WARN("BitChat: Route too long (%d hops), truncating to %d", count, BITCHAT_MAX_HOPS);
             LOG_WARN("BitChat: Route too long, rejecting");
             return false;
        }
        
        // Check size
        if (length - offset < (size_t)(count * 8)) {
            LOG_WARN("BitChat: Truncated route data");
            return false;
        }
        
        msg.routeCount = count;
        for (int i = 0; i < count; i++) {
            uint64_t hopId = 0;
            for (int b = 0; b < 8; b++) {
                hopId = (hopId << 8) | data[offset++];
            }
            msg.route[i] = hopId;
        }
    }
    
    // Validate payload length again vs MAX
    if (msg.payloadLength > BITCHAT_MAX_PAYLOAD_SIZE) {
        LOG_WARN("BitChat: Payload too large (%d bytes)", msg.payloadLength);
        return false;
    }
    
    // Payload
    if (isCompressed) {
        LOG_WARN("BitChat: Compressed payload not supported yet");
        return false;
    }
    
    if (msg.payloadLength > 0) {
        memcpy(msg.payload, data + offset, msg.payloadLength);
        offset += msg.payloadLength;
    }
    // Zero rest
    if (msg.payloadLength < BITCHAT_MAX_PAYLOAD_SIZE) {
         memset(msg.payload + msg.payloadLength, 0, BITCHAT_MAX_PAYLOAD_SIZE - msg.payloadLength);
    }
    
    // Signature
    if (hasSignature) {
        memcpy(msg.signature, data + offset, BITCHAT_SIGNATURE_SIZE);
        offset += BITCHAT_SIGNATURE_SIZE;
    } else {
        memset(msg.signature, 0, BITCHAT_SIGNATURE_SIZE);
    }
    
    LOG_DEBUG("BitChat: Parsed V%d msg type=0x%02x, len=%d, route=%d hops", 
              msg.version, msg.type, msg.payloadLength, msg.routeCount);
              
    return true;
}

size_t BitChatProtocolHandler::serializeMessage(const BitChatMessage& msg, uint8_t* buffer, size_t maxLength)
{
    // Determine header size based on version
    size_t headerSize = (msg.version >= BITCHAT_VERSION_2) ? BITCHAT_HEADER_SIZE_V2 : BITCHAT_HEADER_SIZE_V1;
    
    bool hasRecipient = (msg.flags & BITCHAT_FLAG_HAS_RECIPIENT) != 0;
    bool hasSignature = (msg.flags & BITCHAT_FLAG_HAS_SIGNATURE) != 0;
    bool hasRoute = (msg.version >= BITCHAT_VERSION_2) && ((msg.flags & BITCHAT_FLAG_HAS_ROUTE) != 0);
    
    size_t routeSize = hasRoute ? (1 + msg.routeCount * 8) : 0;
    size_t requiredLength = headerSize + 8 + (hasRecipient ? 8 : 0) + routeSize + msg.payloadLength + (hasSignature ? BITCHAT_SIGNATURE_SIZE : 0);
    
    if (!buffer || maxLength < requiredLength) {
        return 0;
    }
    
    size_t offset = 0;
    
    // Header
    buffer[offset++] = msg.version;
    buffer[offset++] = msg.type;
    buffer[offset++] = msg.ttl;
    
    // Timestamp (Big Endian)
    for (int i = 7; i >= 0; i--) {
        buffer[offset++] = static_cast<uint8_t>((msg.timestamp >> (i * 8)) & 0xFF);
    }
    
    buffer[offset++] = msg.flags;
    
    // Payload Length
    if (msg.version >= BITCHAT_VERSION_2) {
        buffer[offset++] = (msg.payloadLength >> 24) & 0xFF;
        buffer[offset++] = (msg.payloadLength >> 16) & 0xFF;
        buffer[offset++] = (msg.payloadLength >> 8) & 0xFF;
        buffer[offset++] = msg.payloadLength & 0xFF;
    } else {
        buffer[offset++] = (msg.payloadLength >> 8) & 0xFF;
        buffer[offset++] = msg.payloadLength & 0xFF;
    }
    
    // Sender ID
    memcpy(buffer + offset, &msg.senderId, sizeof(msg.senderId));
    offset += 8;
    
    // Recipient ID
    if (hasRecipient) {
        memcpy(buffer + offset, &msg.recipientId, sizeof(msg.recipientId));
        offset += 8;
    }
    
    // Route (V2)
    if (hasRoute) {
        buffer[offset++] = msg.routeCount;
        for (int i = 0; i < msg.routeCount; i++) {
            uint64_t hop = msg.route[i];
            for (int b = 7; b >= 0; b--) {
                buffer[offset++] = (hop >> (b * 8)) & 0xFF;
            }
        }
    }
    
    // Payload
    if (msg.payloadLength > 0) {
        memcpy(buffer + offset, msg.payload, msg.payloadLength);
        offset += msg.payloadLength;
    }
    
    // Signature
    if (hasSignature) {
        memcpy(buffer + offset, msg.signature, BITCHAT_SIGNATURE_SIZE);
        offset += BITCHAT_SIGNATURE_SIZE;
    }
    
    LOG_DEBUG("BitChat: Serialized V%d msg %d bytes (route=%d)", msg.version, offset, msg.routeCount);
    printBytes("BitChat Outgoing", buffer, offset);
    return offset;
}

bool BitChatProtocolHandler::validateMessage(const BitChatMessage& msg)
{
    // Check version
    if (msg.version != BITCHAT_VERSION_1 && msg.version != BITCHAT_VERSION_2) {
        LOG_WARN("BitChat: Invalid version %d", msg.version);
        return false;
    }
    
    // Check message type - validate known types
    bool validType = false;
    switch (msg.type) {
        case BITCHAT_MSG_ANNOUNCE:
        case BITCHAT_MSG_MESSAGE:
        case BITCHAT_MSG_LEAVE:
        case BITCHAT_MSG_IDENTITY:
        case BITCHAT_MSG_CHANNEL:
        case BITCHAT_MSG_PING:
        case BITCHAT_MSG_PONG:
        case BITCHAT_MSG_NOISE_HANDSHAKE:
        case BITCHAT_MSG_NOISE_ENCRYPTED:
        case BITCHAT_MSG_FRAGMENT_NEW:
        case BITCHAT_MSG_REQUEST_SYNC:
        case BITCHAT_MSG_FILE_TRANSFER:
        case BITCHAT_MSG_FRAGMENT:
            validType = true;
            break;
        default:
            LOG_WARN("BitChat: Invalid message type 0x%02x", msg.type);
            return false;
    }
    if (!validType) {
        LOG_WARN("BitChat: Invalid message type 0x%02x", msg.type);
        return false;
    }
    
    // Check TTL
    if (msg.ttl == 0) {
        LOG_DEBUG("BitChat: Message TTL expired");
        return false;
    }
    
    // Check payload length
    if (msg.payloadLength > BITCHAT_MAX_PAYLOAD_SIZE) {
        LOG_WARN("BitChat: Payload too large (%d bytes)", msg.payloadLength);
        return false;
    }
    
    // Check timestamp (not too far in future, not too old)
    // Convert timestamp from milliseconds to seconds for comparison
    uint64_t currentTimeMs = static_cast<uint64_t>(getTime()) * 1000ULL;
    const uint64_t MAX_FUTURE_SKEW_MS = 86400ULL * 1000ULL; // 24 hours in milliseconds
    const uint64_t MAX_PAST_AGE_MS   = 3600ULL * 1000ULL;   // 1 hour in milliseconds

    if (currentTimeMs > 0) {
        if (msg.timestamp > currentTimeMs + MAX_FUTURE_SKEW_MS) {
            LOG_WARN("BitChat: Message timestamp too far in future (accepting due to clock skew)");
        }
        
        // Allow messages up to 1 hour old
        if (currentTimeMs > msg.timestamp && (currentTimeMs - msg.timestamp) > MAX_PAST_AGE_MS) {
            LOG_DEBUG("BitChat: Message too old, dropping");
            return false;
        }
    }
    
    return true;
}

meshtastic_MeshPacket* BitChatProtocolHandler::createMeshtasticPacket(const BitChatMessage& bitchatMsg)
{
    // Allocate Meshtastic packet using router
    meshtastic_MeshPacket* packet = router->allocForSending();
    if (!packet) {
        LOG_ERROR("BitChat: Failed to allocate Meshtastic packet");
        return nullptr;
    }
    
    // Set port number for BitChat bridge
    packet->decoded.portnum = meshtastic_PortNum_PRIVATE_APP;
    
    // Directly serialize the BitChat message into the payload
    // Format: [4 bytes: magic] [BitChat message data]
    const uint32_t BITCHAT_MAGIC = 0x42434854; // "BCHT"
    
    // Calculate actual message size (same calculation as serializeMessage)
    size_t headerSize = (bitchatMsg.version >= BITCHAT_VERSION_2) ? BITCHAT_HEADER_SIZE_V2 : BITCHAT_HEADER_SIZE_V1;
    bool hasRecipient = (bitchatMsg.flags & BITCHAT_FLAG_HAS_RECIPIENT) != 0;
    bool hasSignature = (bitchatMsg.flags & BITCHAT_FLAG_HAS_SIGNATURE) != 0;
    bool hasRoute = (bitchatMsg.version >= BITCHAT_VERSION_2) && ((bitchatMsg.flags & BITCHAT_FLAG_HAS_ROUTE) != 0);
    size_t routeSize = hasRoute ? (1 + bitchatMsg.routeCount * 8) : 0;
    
    size_t messageSize = headerSize + 8 + (hasRecipient ? 8 : 0) + routeSize + bitchatMsg.payloadLength + (hasSignature ? BITCHAT_SIGNATURE_SIZE : 0);
    size_t totalSize = sizeof(BITCHAT_MAGIC) + messageSize;
    
    if (totalSize > sizeof(packet->decoded.payload.bytes)) {
        LOG_ERROR("BitChat: Message too large for Meshtastic packet (%d bytes, max %d)", 
                  totalSize, sizeof(packet->decoded.payload.bytes));
        packetPool.release(packet);
        return nullptr;
    }
    
    uint8_t* buffer = packet->decoded.payload.bytes;
    size_t offset = 0;
    
    // Write magic number
    memcpy(buffer + offset, &BITCHAT_MAGIC, sizeof(BITCHAT_MAGIC));
    offset += sizeof(BITCHAT_MAGIC);
    
    // Serialize BitChat message (messageSize was already calculated above)
    size_t serializedSize = serializeMessage(bitchatMsg, buffer + offset, 
                                        sizeof(packet->decoded.payload.bytes) - offset);
    if (serializedSize == 0) {
        LOG_ERROR("BitChat: Failed to serialize message");
        packetPool.release(packet);
        return nullptr;
    }
    
    // Verify serialized size matches expected size
    if (serializedSize != messageSize) {
        LOG_WARN("BitChat: Serialized size (%d) doesn't match expected size (%d)", serializedSize, messageSize);
    }
    
    packet->decoded.payload.size = offset + serializedSize;
    
    // Set hop limit based on TTL
    packet->hop_limit = bitchatMsg.ttl;
    packet->want_ack = false; // BitChat handles its own reliability
    
    LOG_DEBUG("BitChat: Created Meshtastic packet with %d byte payload", packet->decoded.payload.size);
    return packet;
}

bool BitChatProtocolHandler::extractBitChatMessage(const meshtastic_MeshPacket& meshPacket, BitChatMessage& bitchatMsg)
{
    // Check if this is a BitChat packet
    if (meshPacket.decoded.portnum != meshtastic_PortNum_PRIVATE_APP) {
        return false;
    }
    
    // Check for minimum size (magic + header)
    const uint32_t BITCHAT_MAGIC = 0x42434854; // "BCHT"
    if (meshPacket.decoded.payload.size < sizeof(BITCHAT_MAGIC) + 1) { // At least magic + version
        return false;
    }
    
    const uint8_t* buffer = meshPacket.decoded.payload.bytes;
    size_t offset = 0;
    
    // Check magic number
    uint32_t magic;
    memcpy(&magic, buffer + offset, sizeof(magic));
    offset += sizeof(magic);
    
    if (magic != BITCHAT_MAGIC) {
        // Not a BitChat packet
        return false;
    }
    
    // Parse BitChat message from remaining data
    size_t remainingSize = meshPacket.decoded.payload.size - offset;
    if (!parseMessage(buffer + offset, remainingSize, bitchatMsg)) {
        LOG_WARN("BitChat: Failed to parse BitChat message from Meshtastic packet");
        return false;
    }
    
    LOG_DEBUG("BitChat: Extracted message type=0x%02x, sender=0x%08x, ttl=%d, payload=%d bytes",
              bitchatMsg.type, bitchatMsg.senderId, bitchatMsg.ttl, bitchatMsg.payloadLength);
    
    return true;
}
