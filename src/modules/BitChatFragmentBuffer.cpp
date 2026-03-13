#include "BitChatBridgeModule.h"
#include "configuration.h"

/**
 * Add a fragment to the reassembly buffer
 * Returns true if the message is complete and ready for processing
 */
bool FragmentReassemblyBuffer::addFragment(const BitChatMessage& fragment, uint16_t fragmentId, 
                                           uint8_t index, uint8_t total, uint16_t originalSize)
{
    // Find or create buffer for this fragment ID
    FragmentBuffer* buffer = nullptr;
    int emptySlot = -1;
    
    for (size_t i = 0; i < buffers.size(); i++) {
        if (buffers[i].fragmentId == fragmentId && buffers[i].totalFragments > 0) {
            buffer = &buffers[i];
            break;
        }
        if (emptySlot == -1 && buffers[i].totalFragments == 0) {
            emptySlot = i;
        }
    }
    
    // If not found, create new buffer
    if (!buffer) {
        if (emptySlot == -1) {
            LOG_ERROR("BitChat Fragment: No free buffer slots");
            return false;
        }
        buffer = &buffers[emptySlot];
        buffer->fragmentId = fragmentId;
        buffer->totalFragments = total;
        buffer->receivedFragments = 0;
        buffer->originalSize = originalSize;
        buffer->firstFragmentTime = millis();
        memset(buffer->fragmentsReceived, 0, sizeof(buffer->fragmentsReceived));
        
        // Store original message metadata from first fragment
        buffer->originalMessage.senderId = fragment.senderId;
        buffer->originalMessage.timestamp = fragment.timestamp;
        buffer->originalMessage.ttl = fragment.ttl;
        
        // Extract original type from fragment payload (byte 6)
        if (fragment.payloadLength >= 7) {
            buffer->originalMessage.type = fragment.payload[6];
        }
        
        LOG_DEBUG("BitChat Fragment: Created new buffer for ID 0x%04x (%d fragments, %d bytes)",
                  fragmentId, total, originalSize);
    }
    
    // Validate fragment
    if (index >= total) {
        LOG_ERROR("BitChat Fragment: Invalid fragment index %d (total: %d)", index, total);
        return false;
    }
    
    if (buffer->fragmentsReceived[index]) {
        LOG_DEBUG("BitChat Fragment: Duplicate fragment %d for ID 0x%04x", index, fragmentId);
        return false; // Already have this fragment
    }
    
    // Extract fragment data (skip 7-byte header: fragment_id(2) + index(1) + total(1) + size(2) + type(1))
    size_t dataOffset = 7;
    size_t dataSize = fragment.payloadLength - dataOffset;
    size_t dstOffset = index * BITCHAT_MAX_FRAGMENT_PAYLOAD;
    
    // Copy fragment data to reassembly buffer
    if (dstOffset + dataSize > sizeof(buffer->reassemblyBuffer)) {
        LOG_ERROR("BitChat Fragment: Buffer overflow (offset: %d, size: %d)", dstOffset, dataSize);
        // Clear this buffer
        buffer->totalFragments = 0;
        return false;
    }
    
    memcpy(&buffer->reassemblyBuffer[dstOffset], &fragment.payload[dataOffset], dataSize);
    buffer->fragmentsReceived[index] = true;
    buffer->receivedFragments++;
    
    LOG_DEBUG("BitChat Fragment: Received fragment %d/%d for ID 0x%04x (%d bytes)",
              index + 1, total, fragmentId, dataSize);
    
    // Check if complete
    if (buffer->receivedFragments == buffer->totalFragments) {
        LOG_INFO("BitChat Fragment: Message 0x%04x complete (%d fragments, %d bytes)",
                 fragmentId, total, originalSize);
        return true;
    }
    
    return false;
}

/**
 * Check if a fragmented message is complete
 */
bool FragmentReassemblyBuffer::isComplete(uint16_t fragmentId)
{
    for (const auto& buffer : buffers) {
        if (buffer.fragmentId == fragmentId && buffer.totalFragments > 0) {
            return buffer.receivedFragments == buffer.totalFragments;
        }
    }
    return false;
}

/**
 * Get the reassembled message and clear the buffer
 */
bool FragmentReassemblyBuffer::getReassembledMessage(uint16_t fragmentId, BitChatMessage& outMsg)
{
    for (auto& buffer : buffers) {
        if (buffer.fragmentId == fragmentId && buffer.totalFragments > 0) {
            if (buffer.receivedFragments != buffer.totalFragments) {
                return false; // Not complete yet
            }
            
            // Reconstruct original message
            outMsg.type = buffer.originalMessage.type;
            outMsg.senderId = buffer.originalMessage.senderId;
            outMsg.timestamp = buffer.originalMessage.timestamp;
            outMsg.ttl = buffer.originalMessage.ttl;
            outMsg.payloadLength = buffer.originalSize;
            
            // Copy reassembled payload
            if (buffer.originalSize > sizeof(outMsg.payload)) {
                LOG_ERROR("BitChat Fragment: Reassembled size too large (%d bytes)", buffer.originalSize);
                buffer.totalFragments = 0; // Clear buffer
                return false;
            }
            
            memcpy(outMsg.payload, buffer.reassemblyBuffer, buffer.originalSize);
            
            LOG_INFO("BitChat Fragment: Reassembled message type 0x%02x, sender 0x%08x, %d bytes",
                     outMsg.type, outMsg.senderId, outMsg.payloadLength);
            
            // Clear buffer
            buffer.totalFragments = 0;
            buffer.receivedFragments = 0;
            
            return true;
        }
    }
    
    return false;
}

/**
 * Clean up expired fragment buffers
 */
void FragmentReassemblyBuffer::cleanup(uint32_t currentTime)
{
    for (auto& buffer : buffers) {
        if (buffer.totalFragments > 0) {
            if (currentTime - buffer.firstFragmentTime > BITCHAT_FRAGMENT_TIMEOUT_MS) {
                LOG_WARN("BitChat Fragment: Timeout for ID 0x%04x (received %d/%d fragments)",
                         buffer.fragmentId, buffer.receivedFragments, buffer.totalFragments);
                buffer.totalFragments = 0;
                buffer.receivedFragments = 0;
            }
        }
    }
}


