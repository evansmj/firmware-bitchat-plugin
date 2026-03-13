#pragma once
#include <map>
#include <stdint.h>

/**
 * Manages the topology of directly connected BitChat peers
 * primarily for BLE Source Routing (unicast)
 */
class BitChatTopologyManager {
public:
    struct NeighborInfo {
        uint16_t connHandle;      // BLE Connection Handle
        uint32_t lastHeard;       // Timestamp (millis)
        bool isDirect;            // True if TTL=7
        
        NeighborInfo() : connHandle(0xFFFF), lastHeard(0), isDirect(false) {}
        NeighborInfo(uint16_t handle, uint32_t time, bool direct) 
            : connHandle(handle), lastHeard(time), isDirect(direct) {}
    };

private:
    std::map<uint64_t, NeighborInfo> neighbors; // PeerID -> Info
    static constexpr uint32_t NEIGHBOR_TIMEOUT_MS = 60000; // 1 minute timeout

public:
    void updateNeighbor(uint64_t peerId, uint16_t connHandle, bool isDirect);
    bool getNeighbor(uint32_t peerId, NeighborInfo& info);
    void removeNeighbor(uint32_t peerId);
    void cleanup(uint32_t currentTime);
    
    // Get list of direct neighbors for ANNOUNCE packet
    // returns count, fills array up to maxCount
    uint8_t getDirectNeighborIds(uint64_t* outIds, uint8_t maxCount);
};
