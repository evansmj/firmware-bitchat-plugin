#include "BitChatTopologyManager.h"
#include "configuration.h" // For millis() usually, or Arduino.h

void BitChatTopologyManager::updateNeighbor(uint64_t peerId, uint16_t connHandle, bool isDirect) {
    uint32_t currentTime = millis();
    neighbors[peerId] = NeighborInfo(connHandle, currentTime, isDirect);
}

bool BitChatTopologyManager::getNeighbor(uint32_t peerId, NeighborInfo& info) {
    auto it = neighbors.find(peerId);
    if (it != neighbors.end()) {
        info = it->second;
        return true;
    }
    return false;
}

void BitChatTopologyManager::removeNeighbor(uint32_t peerId) {
    neighbors.erase(peerId);
}

void BitChatTopologyManager::cleanup(uint32_t currentTime) {
    auto it = neighbors.begin();
    while (it != neighbors.end()) {
        if (currentTime - it->second.lastHeard > NEIGHBOR_TIMEOUT_MS) {
            it = neighbors.erase(it);
        } else {
            ++it;
        }
    }
}

uint8_t BitChatTopologyManager::getDirectNeighborIds(uint64_t* outIds, uint8_t maxCount) {
    uint8_t count = 0;
    for (const auto& pair : neighbors) {
        if (pair.second.isDirect && count < maxCount) {
            // Promote 32-bit ID to 64-bit for protocol V2
            outIds[count++] = (uint64_t)pair.first;
        }
    }
    return count;
}
