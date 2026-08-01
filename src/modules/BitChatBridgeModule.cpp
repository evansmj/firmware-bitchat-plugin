#include "BitChatBridgeModule.h"
#include "configuration.h"
#include "mesh/MeshService.h"
#include "mesh/Router.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Default.h"
#include <SHA256.h>
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
#include <Curve25519.h>
#include <Ed25519.h>
#endif

#if !MESHTASTIC_EXCLUDE_BLUETOOTH
#ifdef ARCH_ESP32
#include "nimble/NimbleBluetooth.h"
extern NimBLEServer *bleServer;
extern NimbleBluetooth *nimbleBluetooth;
#elif defined(ARCH_NRF52)
#include "platform/nrf52/NRF52Bluetooth.h"
extern NRF52Bluetooth *nrf52Bluetooth;
#endif
#endif

// Global instance
BitChatBridgeModule *bitchatBridgeModule = nullptr;

BitChatBridgeModule::BitChatBridgeModule()
    : SinglePortModule("BitChatBridge", meshtastic_PortNum_PRIVATE_APP), 
      concurrency::OSThread("BitChatBridge")
{
    LOG_INFO("BitChat Bridge: Initializing module");
    
    // Initialize statistics
    messagesRelayed = 0;
    messagesBridged = 0;
    duplicatesDropped = 0;
    // messageQueue (BitChatMessageRing) self-initializes head/tail to 0
}

BitChatBridgeModule::~BitChatBridgeModule()
{
    LOG_INFO("BitChat Bridge: Shutting down module");
    
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
    if (bleEnabled && bleBridge.isServiceActive()) {
        bleBridge.stopAdvertising();
    }
#endif
}

void BitChatBridgeModule::setup()
{
    LOG_INFO("BitChat Bridge: Setting up module");
    
    if (bridgeEnabled) {
        setIntervalFromNow(setStartDelay());
        LOG_INFO("BitChat Bridge: Module setup complete - bridge enabled");
    } else {
        LOG_INFO("BitChat Bridge: Module setup complete - bridge disabled");
    }
}

ProcessMessage BitChatBridgeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!bridgeEnabled) {
        return ProcessMessage::CONTINUE;
    }
    
    // Check if this is a BitChat packet
    if (mp.decoded.portnum != meshtastic_PortNum_PRIVATE_APP) {
        return ProcessMessage::CONTINUE;
    }
    
    LOG_DEBUG("BitChat Bridge: Received mesh packet, size=%d", mp.decoded.payload.size);
    
    // Extract BitChat message from Meshtastic packet
    BitChatMessage bitchatMsg;
    if (!BitChatProtocolHandler::extractBitChatMessage(mp, bitchatMsg)) {
        LOG_WARN("BitChat Bridge: Failed to extract BitChat message from mesh packet");
        return ProcessMessage::CONTINUE;
    }
    
    processBitChatMessage(bitchatMsg, false);
    
    return ProcessMessage::CONTINUE;
}

int32_t BitChatBridgeModule::runOnce()
{
    if (!bridgeEnabled) {
        return 30000; // Check every 30 seconds when disabled
    }
    
    // Derive our BitChat identity keys and peer ID as soon as the Meshtastic secret is
    // populated. The peer ID is SHA256(noisePublicKey)[0..8] (set in initializeBitChatKeys),
    // NOT the Meshtastic node number - iOS rejects announces whose senderID isn't the
    // key-derived fingerprint. Safe to call repeatedly: it no-ops once done or if not ready.
    if (!ed25519KeysInitialized) {
        initializeBitChatKeys();
    }
    
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
    // Set up BLE service (after Bluetooth is initialized). Retry on a backoff instead of
    // permanently disabling BLE on a single failure - a transient failure should not
    // require a reboot.
    if (bleEnabled && !bleServiceSetup && config.bluetooth.enabled && millis() >= nextBleSetupAttempt) {
        nextBleSetupAttempt = millis() + BLE_SETUP_RETRY_MS;
        LOG_DEBUG("BitChat Bridge: Attempting BLE service setup...");
#ifdef ARCH_ESP32
        if (bleServer) {
            // Set bridge module reference BEFORE service setup so callbacks work
            bleBridge.setBridgeModule(this);

            if (bleBridge.setupBitChatService(bleServer)) {
                LOG_INFO("BitChat Bridge: BLE service setup successful (ESP32)");
                bleServiceSetup = true;

                // Stop and restart advertising so phones discover the newly created GATT service.
                // Without this, a phone that connected before the service was ready will cache
                // the incomplete GATT table and never find BitChat on subsequent connections.
                // This matches the nRF52 pattern (Bluefruit.Advertising.stop / resumeAdvertising).
                if (nimbleBluetooth && !nimbleBluetooth->isDeInit) {
                    nimbleBluetooth->setBitChatServiceReady();
                    LOG_INFO("BitChat Bridge: Starting advertising with BitChat service...");
                    nimbleBluetooth->startAdvertising();
                    LOG_INFO("BitChat Bridge: Advertising started");
                }

                LOG_INFO("BitChat Bridge: Creating initial announcement for BLE characteristic...");
                sendPeerAnnouncement();
            } else {
                LOG_ERROR("BitChat Bridge: BLE service setup failed, will retry in %d ms", BLE_SETUP_RETRY_MS);
                // Don't disable BLE - leave bleServiceSetup false so we retry. Bring up
                // base Meshtastic advertising meanwhile so the device stays usable. Crucially
                // do NOT mark the BitChat service ready here: startBaseAdvertising() advertises
                // Meshtastic-only, so we never point phones at a BitChat GATT service that
                // doesn't exist. The next retry will set it ready once setup actually succeeds.
                if (nimbleBluetooth && !nimbleBluetooth->isDeInit) {
                    LOG_WARN("BitChat Bridge: Advertising Meshtastic-only until BitChat setup succeeds");
                    nimbleBluetooth->startBaseAdvertising();
                }
            }
        }
#elif defined(ARCH_NRF52)
        // Check if Bluefruit is initialized by checking if we can access it
        if (nrf52Bluetooth) {
            LOG_DEBUG("BitChat Bridge: nrf52Bluetooth is ready, setting up service...");
            // Set bridge module reference BEFORE service setup so callbacks work
            bleBridge.setBridgeModule(this);
            
            if (bleBridge.setupBitChatService()) {
                LOG_INFO("BitChat Bridge: BLE peripheral service setup successful (nRF52)");
                
                bleServiceSetup = true;
                
                // Stop and restart advertising so iOS can discover the newly created service
                LOG_INFO("BitChat Bridge: Stopping advertising...");
                Bluefruit.Advertising.stop();
                delay(500); // Give iOS time to notice the device disappeared
                LOG_INFO("BitChat Bridge: Restarting advertising with BitChat service...");
                nrf52Bluetooth->resumeAdvertising();
            } else {
                LOG_ERROR("BitChat Bridge: BLE service setup failed, will retry in %d ms", BLE_SETUP_RETRY_MS);
                // Don't disable BLE - leave bleServiceSetup false so we retry.
            }
        } else {
            LOG_DEBUG("BitChat Bridge: nrf52Bluetooth not ready yet");
        }
#endif
    }
    
    // Periodically alternate which service UUID sits in the main advertising packet, so the
    // Meshtastic app AND the BitChat apps can each discover this node on iOS (iOS filters scans
    // on the main packet only, and two 128-bit UUIDs don't fit in one legacy advertisement).
    // Only swaps while disconnected; skipped entirely while a phone is connected.
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    if (bleServiceSetup) {
        uint32_t nowMs = millis();
        if (nowMs - lastAdvAlternateTime >= ADV_ALTERNATE_INTERVAL_MS) {
            lastAdvAlternateTime = nowMs;
#ifdef ARCH_ESP32
            if (nimbleBluetooth) {
                nimbleBluetooth->swapBitChatAdvertising();
            }
#elif defined(ARCH_NRF52)
            if (nrf52Bluetooth) {
                nrf52Bluetooth->swapBitChatAdvertising();
            }
#endif
        }
    }
#endif

    // Periodically ensure advertising is active on ESP32 (safety net).
    // NimBLE stops advertising on connect and relies on onDisconnect to restart it,
    // but if that callback is missed for any reason, advertising stays off silently.
    // nRF52's Bluefruit.Advertising.restartOnDisconnect(true) handles this automatically.
#ifdef ARCH_ESP32
    if (bleServiceSetup && nimbleBluetooth) {
        nimbleBluetooth->ensureAdvertising();
    }
#endif
#endif
    
    // Send peer announcements (act as a BitChat peer)
    #if !MESHTASTIC_EXCLUDE_BLUETOOTH
    if (bleEnabled && bleServiceSetup) {
        uint32_t currentTime = millis();
        // Service an immediate announcement requested by the BLE connect callback (signing
        // runs here on the main thread, not in the callback).
        if (announceRequested) {
            announceRequested = false;
            sendPeerAnnouncement();
            lastAnnounceTime = currentTime;
        } else {
            uint32_t announceInterval = bleBridge.isServiceActive()
                                        ? 5000
                                        : ANNOUNCE_INTERVAL_MS;
            if (currentTime - lastAnnounceTime >= announceInterval) {
                sendPeerAnnouncement();
                lastAnnounceTime = currentTime;
            }
        }
    }
    #endif
    
    // Clean up expired fragment buffers
    fragmentBuffer.cleanup(millis());
    
    // Process queued messages from BLE callbacks (deferred to avoid stack overflow in callbacks)
    // Process up to 2 messages per run to avoid blocking too long
    size_t processed = 0;
    QueuedMessage queued;
    while (processed < 2 && messageQueue.pop(queued)) {
        processBitChatMessage(queued.msg, queued.fromBLE);
        processed++;
    }

    // Update statistics and cleanup
    updateStatistics();

    // If we have queued messages, run more frequently to process them quickly
    // Otherwise, run every 5 seconds during normal operation
    if (!messageQueue.empty()) {
        return 100; // Process remaining messages quickly (100ms)
    }
    return 5000;
}

void BitChatBridgeModule::queueMessageForProcessing(const BitChatMessage& msg, bool fromBLE)
{
    // Producer side of the SPSC ring — safe to call from a BLE callback task.
    QueuedMessage q;
    q.msg = msg;
    q.fromBLE = fromBLE;
    if (!messageQueue.push(q)) {
        LOG_WARN("BitChat Bridge: Message queue full, dropping message");
    }
}

void BitChatBridgeModule::processBitChatMessage(const BitChatMessage& msg, bool fromBLE)
{
    logMessage(msg, fromBLE ? "BLE->Mesh" : "Mesh->BLE");
    
    // Learn real time from any BitChat peer timestamp (BLE phone or a relayed mesh
    // message), but only as a fallback when Meshtastic's own RTC has no valid time. If the
    // RTC is valid we prefer it directly (see computeAnnounceTimestampMs).
    if (!haveSyncedBase && getValidTime(RTCQualityDevice) == 0) {
        uint64_t msgTimestampMs = msg.timestamp;

        // Only sync if the message timestamp looks reasonable (within ~50 years of epoch)
        const uint64_t year2020Ms = 1577836800000ULL; // Jan 1, 2020 in ms
        const uint64_t year2070Ms = 3155760000000ULL; // Jan 1, 2070 in ms

        if (msgTimestampMs >= year2020Ms && msgTimestampMs <= year2070Ms) {
            syncedUnixMs0 = msgTimestampMs;
            syncedAtMillis0 = millis();
            haveSyncedBase = true;
            LOG_INFO("BitChat Bridge: Time base learned from %s peer (%llu ms)",
                     fromBLE ? "BLE" : "mesh", (unsigned long long)msgTimestampMs);
        }
    }
    
    // Handle fragments specially
    if (msg.type == BITCHAT_MSG_FRAGMENT) {
        if (fromBLE) {
            // Fragments from BLE shouldn't happen (BLE doesn't fragment)
            LOG_WARN("BitChat Bridge: Received fragment from BLE (unexpected)");
            return;
        }
        
        // Fragment from mesh - try to reassemble
        if (handleFragment(msg)) {
            // Fragment complete - get reassembled message
            BitChatMessage reassembledMsg;
            uint16_t fragmentId = (msg.payload[0] << 8) | msg.payload[1];
            
            if (fragmentBuffer.getReassembledMessage(fragmentId, reassembledMsg)) {
                LOG_INFO("BitChat Bridge: Reassembled message type 0x%02x from %d fragments",
                         reassembledMsg.type, msg.payload[3]); // total fragments at byte 3
                
                // Process the reassembled message
                broadcastToBLE(reassembledMsg);
                messagesBridged++;
            } else {
                LOG_ERROR("BitChat Bridge: Failed to get reassembled message 0x%04x", fragmentId);
            }
        }
        // If not complete yet, just return (waiting for more fragments)
        return;
    }
    
    // Validate message
    if (!BitChatProtocolHandler::validateMessage(msg)) {
        LOG_WARN("BitChat Bridge: Invalid message rejected");
        return;
    }
    
    // Check for duplicates
    if (duplicateCache.isDuplicate(msg)) {
        duplicatesDropped++;
        LOG_DEBUG("BitChat Bridge: Duplicate message dropped");
        return;
    }
    
    // Add to cache
    duplicateCache.addMessage(msg);
    
    // Determine if we should relay this message
    if (!shouldRelayMessage(msg)) {
        LOG_DEBUG("BitChat Bridge: Message filtering - not relaying");
        return;
    }
    
    if (fromBLE) {
        // Rate-limit announcements to avoid flooding LoRa mesh
        if (msg.type == BITCHAT_MSG_ANNOUNCE && isAnnouncementRateLimited(msg)) {
            return;
        }
        relayToMesh(msg);
    } else {
        broadcastToBLE(msg);
        messagesBridged++;
    }
}

void BitChatBridgeModule::broadcastToBLE(const BitChatMessage& msg)
{
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
    if (!bleEnabled || !bleBridge.isServiceActive()) {
        LOG_DEBUG("BitChat Bridge: BLE not available for broadcast");
        return;
    }
    
    // On ESP32, both services are now advertised continuously (no time windows)
    // On nRF52, both services are always active
    // No time window checks needed - advertising is always on
    
    // Broadcast the message
    bleBridge.broadcastMessage(msg);
    
    LOG_DEBUG("BitChat Bridge: Message broadcasted to BLE");
#else
    LOG_DEBUG("BitChat Bridge: BLE not available - skipping broadcast");
#endif
}

void BitChatBridgeModule::relayToMesh(const BitChatMessage& msg)
{
    // Decrement TTL for mesh relay
    BitChatMessage relayMsg = msg;
    if (relayMsg.ttl > maxHops) {
        LOG_WARN("BitChat Bridge: Message TTL (%d) exceeds max hops (%d) - clamping", relayMsg.ttl, maxHops);
        relayMsg.ttl = maxHops;
    }

    if (relayMsg.ttl > 0) {
        relayMsg.ttl--;
    }
    
    if (relayMsg.ttl == 0) {
        LOG_DEBUG("BitChat Bridge: TTL expired - not relaying to mesh");
        return;
    }
    
    // Check if message needs fragmentation
    // Max BitChat payload = 217 bytes (233 Meshtastic - 4 magic - 12 BitChat header)
    const size_t MAX_BITCHAT_PAYLOAD = 217;
    
    if (relayMsg.payloadLength > MAX_BITCHAT_PAYLOAD) {
        LOG_INFO("BitChat Bridge: Message too large (%d bytes), fragmenting", relayMsg.payloadLength);
        
        // Fragment the message
        std::vector<BitChatMessage> fragments = fragmentMessage(relayMsg);
        if (fragments.empty()) {
            LOG_ERROR("BitChat Bridge: Failed to fragment message");
            return;
        }
        
        // Send each fragment
        for (const auto& fragment : fragments) {
            meshtastic_MeshPacket* packet = BitChatProtocolHandler::createMeshtasticPacket(fragment);
            if (packet) {
                service->sendToMesh(packet, RX_SRC_LOCAL);
                messagesRelayed++;
            } else {
                LOG_ERROR("BitChat Bridge: Failed to create packet for fragment");
            }
        }
        
        LOG_INFO("BitChat Bridge: Sent %d fragments to mesh", fragments.size());
        return;
    }
    
    // Message fits in single packet - send directly
    meshtastic_MeshPacket* packet = BitChatProtocolHandler::createMeshtasticPacket(relayMsg);
    if (!packet) {
        LOG_ERROR("BitChat Bridge: Failed to create Meshtastic packet");
        return;
    }
    
    // Send to mesh
    service->sendToMesh(packet, RX_SRC_LOCAL);
    messagesRelayed++;
    
    LOG_DEBUG("BitChat Bridge: Message relayed to mesh, TTL=%d", relayMsg.ttl);
}

bool BitChatBridgeModule::shouldRelayMessage(const BitChatMessage& msg)
{
    if (msg.ttl == 0) {
        return false;
    }
    
    // Check message type filtering
    switch (msg.type) {
        case BITCHAT_MSG_ANNOUNCE:
        case BITCHAT_MSG_MESSAGE:
        case BITCHAT_MSG_LEAVE:
        case BITCHAT_MSG_PING:
        case BITCHAT_MSG_PONG:
            // These message types are generally allowed
            return true;
            
        case BITCHAT_MSG_IDENTITY:
        case BITCHAT_MSG_CHANNEL:
            // These might be restricted in some configurations
            // For now, allow them
            return true;
            
        case BITCHAT_MSG_REQUEST_SYNC:
            // Sync requests are local-only, don't relay to mesh
            LOG_DEBUG("BitChat Bridge: Received sync request (local-only, not relaying)");
            return false;
            
        case BITCHAT_MSG_NOISE_HANDSHAKE:
        case BITCHAT_MSG_NOISE_ENCRYPTED:
            // Noise protocol messages - allow for now
            // TODO: Implement Noise protocol handling
            LOG_DEBUG("BitChat Bridge: Noise protocol message (type 0x%02x)", msg.type);
            return true;
            
        case BITCHAT_MSG_FRAGMENT_NEW:
        case BITCHAT_MSG_FRAGMENT:
            // Fragment messages - allow
            return true;
            
        case BITCHAT_MSG_FILE_TRANSFER:
            // File transfer messages - relay them
            LOG_DEBUG("BitChat Bridge: File transfer message");
            return true;
            
        default:
            LOG_WARN("BitChat Bridge: Unknown message type 0x%02x", msg.type);
            return false;
    }
}

bool BitChatBridgeModule::isAnnouncementRateLimited(const BitChatMessage& msg)
{
    uint32_t senderId = msg.getSenderId32();
    uint32_t now = millis();

    // Check if we already have an entry for this sender
    for (size_t i = 0; i < announceRateLimitCount; i++) {
        if (announceRateLimit[i].senderId == senderId) {
            uint32_t elapsed = now - announceRateLimit[i].lastRelayTime;
            if (elapsed < ANNOUNCE_RELAY_INTERVAL_MS) {
                LOG_DEBUG("BitChat Bridge: Rate-limiting announcement from 0x%08x (%d seconds until next relay)",
                          senderId, (ANNOUNCE_RELAY_INTERVAL_MS - elapsed) / 1000);
                return true; // Rate-limited
            }
            // Interval passed — allow relay and update timestamp
            announceRateLimit[i].lastRelayTime = now;
            LOG_INFO("BitChat Bridge: Relaying announcement from 0x%08x to mesh (rate-limit interval passed)", senderId);
            return false;
        }
    }

    // New sender — add entry
    if (announceRateLimitCount < MAX_RATE_LIMIT_ENTRIES) {
        announceRateLimit[announceRateLimitCount].senderId = senderId;
        announceRateLimit[announceRateLimitCount].lastRelayTime = now;
        announceRateLimitCount++;
    } else {
        // Table full — evict oldest entry
        uint32_t oldestTime = now;
        size_t oldestIdx = 0;
        for (size_t i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
            uint32_t age = now - announceRateLimit[i].lastRelayTime;
            if (age > (now - oldestTime)) {
                oldestTime = announceRateLimit[i].lastRelayTime;
                oldestIdx = i;
            }
        }
        announceRateLimit[oldestIdx].senderId = senderId;
        announceRateLimit[oldestIdx].lastRelayTime = now;
    }

    LOG_INFO("BitChat Bridge: Relaying first announcement from 0x%08x to mesh", senderId);
    return false; // First time — allow relay
}

void BitChatBridgeModule::updateStatistics()
{
    // This could be expanded to track more detailed statistics
    // For now, just log periodic status
    static uint32_t lastStatusLog = 0;
    uint32_t currentTime = millis();
    
    if (currentTime - lastStatusLog > 60000) { // Log every minute
        LOG_INFO("BitChat Bridge: Status - Relayed: %d, Bridged: %d, Duplicates: %d",
                 messagesRelayed, messagesBridged, duplicatesDropped);
        lastStatusLog = currentTime;
    }
}

void BitChatBridgeModule::logMessage(const BitChatMessage& msg, const char* action)
{
    const char* typeStr = "UNKNOWN";
    switch (msg.type) {
        case BITCHAT_MSG_ANNOUNCE: typeStr = "ANNOUNCE"; break;
        case BITCHAT_MSG_MESSAGE: typeStr = "MESSAGE"; break;
        case BITCHAT_MSG_LEAVE: typeStr = "LEAVE"; break;
        case BITCHAT_MSG_IDENTITY: typeStr = "IDENTITY"; break;
        case BITCHAT_MSG_CHANNEL: typeStr = "CHANNEL"; break;
        case BITCHAT_MSG_PING: typeStr = "PING"; break;
        case BITCHAT_MSG_PONG: typeStr = "PONG"; break;
        case BITCHAT_MSG_NOISE_HANDSHAKE: typeStr = "NOISE_HANDSHAKE"; break;
        case BITCHAT_MSG_NOISE_ENCRYPTED: typeStr = "NOISE_ENCRYPTED"; break;
        case BITCHAT_MSG_FRAGMENT_NEW: typeStr = "FRAGMENT"; break;
        case BITCHAT_MSG_REQUEST_SYNC: typeStr = "REQUEST_SYNC"; break;
        case BITCHAT_MSG_FILE_TRANSFER: typeStr = "FILE_TRANSFER"; break;
        case BITCHAT_MSG_FRAGMENT: typeStr = "FRAGMENT_LEGACY"; break;
    }
    
    LOG_DEBUG("BitChat Bridge: %s - Type: %s, Sender: 0x%08x, TTL: %d, Payload: %d bytes",
              action, typeStr, msg.getSenderId32(), msg.ttl, msg.payloadLength);
}

bool BitChatBridgeModule::handleConfigMessage(const meshtastic_AdminMessage* request, meshtastic_AdminMessage* response)
{
    // This would handle configuration changes via the admin interface
    // For now, return false to indicate we don't handle admin messages
    return false;
}

/**
 * Fragment a large message into multiple smaller messages
 * Returns a vector of fragment messages
 */
std::vector<BitChatMessage> BitChatBridgeModule::fragmentMessage(const BitChatMessage& msg)
{
    std::vector<BitChatMessage> fragments;
    
    // Calculate number of fragments needed
    const size_t MAX_BITCHAT_PAYLOAD = 217; // Max before fragmentation
    if (msg.payloadLength <= MAX_BITCHAT_PAYLOAD) {
        // No fragmentation needed
        fragments.push_back(msg);
        return fragments;
    }
    
    // Generate unique fragment ID
    uint16_t fragmentId = nextFragmentId++;
    if (nextFragmentId == 0) nextFragmentId = 1; // Avoid 0
    
    // Calculate fragments
    uint8_t totalFragments = (msg.payloadLength + BITCHAT_MAX_FRAGMENT_PAYLOAD - 1) / BITCHAT_MAX_FRAGMENT_PAYLOAD;
    if (totalFragments > BITCHAT_MAX_FRAGMENTS) {
        LOG_ERROR("BitChat Bridge: Message too large to fragment (%d bytes, needs %d fragments, max %d)",
                  msg.payloadLength, totalFragments, BITCHAT_MAX_FRAGMENTS);
        // Return empty vector to indicate error
        return fragments;
    }
    
    LOG_INFO("BitChat Bridge: Fragmenting message into %d fragments (ID: 0x%04x, size: %d bytes)",
             totalFragments, fragmentId, msg.payloadLength);
    
    // Create fragments
    for (uint8_t i = 0; i < totalFragments; i++) {
        BitChatMessage fragment;
        fragment.type = BITCHAT_MSG_FRAGMENT;
        fragment.setSenderId32(msg.getSenderId32());
        fragment.timestamp = msg.timestamp;
        fragment.ttl = msg.ttl;
        
        // Build fragment payload: [fragment_id(2)] [index(1)] [total(1)] [original_size(2)] [original_type(1)] [data...]
        size_t offset = 0;
        
        // Fragment ID (2 bytes, big-endian)
        fragment.payload[offset++] = (fragmentId >> 8) & 0xFF;
        fragment.payload[offset++] = fragmentId & 0xFF;
        
        // Fragment index (1 byte)
        fragment.payload[offset++] = i;
        
        // Total fragments (1 byte)
        fragment.payload[offset++] = totalFragments;
        
        // Original message size (2 bytes, big-endian)
        fragment.payload[offset++] = (msg.payloadLength >> 8) & 0xFF;
        fragment.payload[offset++] = msg.payloadLength & 0xFF;
        
        // Original message type (1 byte)
        fragment.payload[offset++] = msg.type;
        
        // Calculate data chunk size for this fragment
        size_t srcOffset = i * BITCHAT_MAX_FRAGMENT_PAYLOAD;
        size_t chunkSize = std::min((size_t)BITCHAT_MAX_FRAGMENT_PAYLOAD, 
                                    (size_t)(msg.payloadLength - srcOffset));
        
        // Copy data chunk
        memcpy(&fragment.payload[offset], &msg.payload[srcOffset], chunkSize);
        offset += chunkSize;
        
        fragment.payloadLength = offset;
        
        LOG_DEBUG("BitChat Bridge: Fragment %d/%d - %d bytes (chunk: %d bytes)",
                  i + 1, totalFragments, fragment.payloadLength, chunkSize);
        
        fragments.push_back(fragment);
    }
    
    return fragments;
}

/**
 * Handle incoming fragment message
 * Returns true if message was completely reassembled and should be processed
 */
bool BitChatBridgeModule::handleFragment(const BitChatMessage& fragment)
{
    if (fragment.payloadLength < 7) { // Minimum: 2+1+1+2+1 = 7 bytes header
        LOG_ERROR("BitChat Bridge: Fragment too small (%d bytes)", fragment.payloadLength);
        return false;
    }
    
    // Parse fragment header
    size_t offset = 0;
    uint16_t fragmentId = (fragment.payload[offset] << 8) | fragment.payload[offset + 1];
    offset += 2;
    uint8_t index = fragment.payload[offset++];
    uint8_t total = fragment.payload[offset++];
    uint16_t originalSize = (fragment.payload[offset] << 8) | fragment.payload[offset + 1];
    offset += 2;
    uint8_t originalType = fragment.payload[offset++];
    
    LOG_DEBUG("BitChat Bridge: Received fragment %d/%d (ID: 0x%04x, size: %d, type: 0x%02x)",
              index + 1, total, fragmentId, originalSize, originalType);
    
    // Validate
    if (index >= total || total > BITCHAT_MAX_FRAGMENTS) {
        LOG_ERROR("BitChat Bridge: Invalid fragment indices (index: %d, total: %d)", index, total);
        return false;
    }
    
    // Add to reassembly buffer
    return fragmentBuffer.addFragment(fragment, fragmentId, index, total, originalSize);
}

/**
 * Send peer announcement - makes Meshtastic appear as a BitChat peer
 */
void BitChatBridgeModule::sendPeerAnnouncement()
{
    // Don't announce until our identity keys exist: without them the senderID and signature
    // would be invalid and every phone would reject the announce (iOS silently drops it).
    if (!ed25519KeysInitialized) {
        initializeBitChatKeys();
        if (!ed25519KeysInitialized) {
            LOG_DEBUG("BitChat Bridge: Skipping announcement - identity keys not ready yet");
            return;
        }
    }

    BitChatMessage announcement = createPeerAnnouncement();

    LOG_INFO("BitChat Bridge: Sending peer announcement (ID: 0x%08x, TTL: %d, payload: %d bytes)",
             myBitChatPeerId, announcement.ttl, announcement.payloadLength);
    
    // Broadcast via BLE - broadcastMessage() now handles both Peripheral and Central roles
    // This matches iOS sendOnAllLinks() behavior - sends on all available BLE links
    broadcastToBLE(announcement);
}

/**
 * Create a BitChat ANNOUNCE message for this Meshtastic device
 * Announcement payload format (TLV):
 * - 0x01 <len> <nickname UTF-8>
 * - 0x02 <len> <32-byte Noise public key>
 * - 0x03 <len> <32-byte Ed25519 signing public key>
 */
BitChatMessage BitChatBridgeModule::createPeerAnnouncement()
{
    BitChatMessage msg;

    msg.type = BITCHAT_MSG_ANNOUNCE;
    // Ensure identity keys (and the derived peer ID) exist before we stamp the senderID.
    if (!ed25519KeysInitialized) {
        initializeBitChatKeys();
    }
    // senderID = SHA256(noisePublicKey)[0..8]; iOS matches this against the announced Noise key.
    memcpy(msg.senderId, myBitChatPeerId8, 8);
    // Timestamp in milliseconds since epoch (iOS format).
    // Prefer Meshtastic's RTC (GPS/NTP/mesh/app); fall back to a peer-learned base.
    bool rtcValid = getValidTime(RTCQualityDevice) > 0;
    uint64_t rtcTimeMs = static_cast<uint64_t>(getTime()) * 1000ULL;
    msg.timestamp = computeAnnounceTimestampMs(rtcValid, rtcTimeMs, haveSyncedBase,
                                               syncedUnixMs0, syncedAtMillis0, millis());
    if (!rtcValid && !haveSyncedBase) {
        // No real time source yet - announcement may be rejected until one appears
        static bool warnedAboutTime = false;
        if (!warnedAboutTime) {
            LOG_WARN("BitChat Bridge: Sending announcement with no real time source - may be rejected");
            warnedAboutTime = true;
        }
    }
    msg.ttl = 7; // TTL=7 to match iOS messageTTLDefault (TransportConfig.messageTTLDefault)
    
    // Get device name
    const char* deviceName = owner.long_name;
    if (!deviceName || deviceName[0] == '\0') {
        deviceName = "Meshtastic";
    }
    
    // Build TLV-encoded announcement payload
    size_t offset = 0;
    
    // TLV 1: Nickname (0x01 + length + UTF-8 string)
    msg.payload[offset++] = 0x01; // Type: nickname
    size_t nicknameLen = strlen(deviceName);
    if (nicknameLen > 255) nicknameLen = 255;
    msg.payload[offset++] = (uint8_t)nicknameLen; // Length
    memcpy(&msg.payload[offset], deviceName, nicknameLen);
    offset += nicknameLen;
    
    // Ensure identity keys are derived before we emit them
    if (!ed25519KeysInitialized) {
        initializeBitChatKeys();
    }

    // TLV 2: Noise Public Key (0x02 + 0x20 + 32 bytes) — real X25519 static public key
    msg.payload[offset++] = 0x02;
    msg.payload[offset++] = 32;
    memcpy(&msg.payload[offset], noisePublicKey, 32);
    offset += 32;

    // TLV 3: Signing Public Key (0x03 + 0x20 + 32 bytes)
    // Use Ed25519 public key from rweather/Crypto library
    msg.payload[offset++] = 0x03; // Type: signingPublicKey
    msg.payload[offset++] = 32;   // Length: 32 bytes
    memcpy(&msg.payload[offset], ed25519PublicKey, 32);
    offset += 32;
    
    msg.payloadLength = offset;
    
    LOG_DEBUG("BitChat Bridge: Created announcement with TLV payload (%d bytes)", offset);
    
    // Sign the announcement using Ed25519
    if (!signAnnouncement(msg)) {
        LOG_WARN("BitChat Bridge: Failed to sign announcement");
    }
    
    return msg;
}

// TweetNaCl removed - randombytes no longer needed

/**
 * Pure key-derivation helper: out = SHA256(secret32 || domainTag).
 * Static and free of any global/BLE state so it can be unit-tested directly.
 */
void BitChatBridgeModule::deriveBitChatSeed(const uint8_t secret[32], const char* domainTag, uint8_t out[32])
{
    SHA256 sha;
    sha.reset();
    sha.update(secret, 32);
    sha.update(domainTag, strlen(domainTag));
    sha.finalize(out, 32);
}

/**
 * Pure time helper (see header). Prefers the RTC when valid; otherwise advances a
 * peer-learned absolute base by elapsed millis(). Never adds an offset to getTime(), so
 * it can't double-count once the RTC becomes valid.
 */
uint64_t BitChatBridgeModule::computeAnnounceTimestampMs(bool rtcValid, uint64_t rtcTimeMs,
                                                         bool haveSyncedBase, uint64_t syncedBaseMs,
                                                         uint32_t syncedAtMillis, uint32_t nowMillis)
{
    if (rtcValid) {
        return rtcTimeMs; // Meshtastic RTC is authoritative
    }
    if (haveSyncedBase) {
        // uint32 subtraction handles millis() wraparound correctly
        uint32_t elapsed = nowMillis - syncedAtMillis;
        return syncedBaseMs + static_cast<uint64_t>(elapsed);
    }
    return rtcTimeMs; // best effort (likely near zero) - no real source yet
}

/**
 * Derive BitChat identity keys from Meshtastic's persisted random secret.
 *
 * config.security.private_key is a 32-byte random key generated once and persisted by
 * NodeDB (see NodeDB.cpp). We derive two independent BitChat keys from it via SHA256 with
 * distinct domain tags:
 *   - Ed25519 signing key (the rweather Ed25519 "private key" is itself a 32-byte seed)
 *   - Noise static X25519 key (Curve25519::dh1 clamps the private and returns the public)
 * This gives real entropy, deterministic identity across reboots, and no extra storage.
 * Defers if the Meshtastic secret is not populated yet (like myBitChatPeerId).
 */
void BitChatBridgeModule::initializeBitChatKeys()
{
    if (ed25519KeysInitialized) {
        return; // Already derived
    }

    if (config.security.private_key.size != 32) {
        LOG_WARN("BitChat Bridge: Meshtastic security key not ready, deferring key derivation");
        return;
    }
    const uint8_t* secret = config.security.private_key.bytes;

    // Ed25519 signing key
    deriveBitChatSeed(secret, "bitchat-ed25519-v1", ed25519SecretKey);
    Ed25519::derivePublicKey(ed25519PublicKey, ed25519SecretKey);

    // Noise static X25519 key.
    // NOTE: Curve25519::dh1() would OVERWRITE the private key with random bytes (it
    // generates a fresh random keypair), which would give a different Noise identity every
    // boot. To derive a stable key from our seed we clamp the scalar ourselves (exactly as
    // dh1 does) and call eval() against the base point.
    deriveBitChatSeed(secret, "bitchat-noise-v1", noisePrivateKey);
    noisePrivateKey[0] &= 0xF8;
    noisePrivateKey[31] = (noisePrivateKey[31] & 0x7F) | 0x40;
    Curve25519::eval(noisePublicKey, noisePrivateKey, nullptr); // public = private * basepoint(9)

    // Derive our BitChat peer ID exactly as iOS/Android do: the first 8 bytes of
    // SHA256(noiseStaticPublicKey). iOS's announce preflight computes PeerID(publicKey:) from
    // the announced Noise key and rejects the announce unless the packet senderID matches, so
    // the senderID CANNOT be the Meshtastic node number - it must be this fingerprint.
    uint8_t noiseFingerprint[32];
    SHA256 idSha;
    idSha.reset();
    idSha.update(noisePublicKey, 32);
    idSha.finalize(noiseFingerprint, 32);
    memcpy(myBitChatPeerId8, noiseFingerprint, 8);
    // Keep the 32-bit id (log-only) consistent with the real fingerprint.
    myBitChatPeerId = 0;
    for (int i = 0; i < 4; i++) {
        myBitChatPeerId |= (static_cast<uint32_t>(myBitChatPeerId8[i]) << (i * 8));
    }

    ed25519KeysInitialized = true;
    LOG_INFO("BitChat Bridge: Derived Ed25519 + Noise keys; peer ID %02x%02x%02x%02x%02x%02x%02x%02x",
             myBitChatPeerId8[0], myBitChatPeerId8[1], myBitChatPeerId8[2], myBitChatPeerId8[3],
             myBitChatPeerId8[4], myBitChatPeerId8[5], myBitChatPeerId8[6], myBitChatPeerId8[7]);
}

/**
 * Apply PKCS#7-style padding to match Android's MessagePadding.pad()
 * Android's optimalBlockSize: calculates dataSize + 16, finds smallest block that fits
 * Android's pad: pads data to the target block size
 * 
 * Example: dataLen=107 -> totalSize=123 -> fits in 256 -> pad to 256
 */
static size_t applyPadding(uint8_t* data, size_t dataLen, size_t maxLen)
{
    // Android's optimalBlockSize logic: account for encryption overhead (~16 bytes)
    const size_t blockSizes[] = {256, 512, 1024, 2048};
    size_t totalSize = dataLen + 16; // Account for encryption overhead
    
    // Find smallest block that fits (matching Android's optimalBlockSize)
    size_t targetSize = dataLen; // Default: no padding (for very large messages)
    for (size_t i = 0; i < sizeof(blockSizes)/sizeof(blockSizes[0]); i++) {
        if (totalSize <= blockSizes[i]) {
            targetSize = blockSizes[i]; // Pad to full block size
            break;
        }
    }
    
    // Android's pad() logic: if data.size >= targetSize, return data (no padding)
    if (dataLen >= targetSize) {
        return dataLen;
    }
    
    // Check buffer bounds
    if (targetSize > maxLen) {
        return dataLen; // Can't pad, buffer too small
    }
    
    // Apply PKCS#7 padding: all pad bytes equal to pad length
    size_t paddingNeeded = targetSize - dataLen;
    if (paddingNeeded > 255) {
        return dataLen; // Can't pad more than 255 bytes (Android constraint)
    }
    
    // Fill padding bytes (PKCS#7: all pad bytes = pad length)
    for (size_t i = dataLen; i < targetSize; i++) {
        data[i] = static_cast<uint8_t>(paddingNeeded);
    }
    
    return targetSize;
}

/**
 * Sign an announcement message using Ed25519
 * Uses Android-compatible format: sign over full packet binary with TTL=0 and padding
 * This matches Android's BinaryProtocol.toBinaryDataForSigning() which:
 * 1. Creates packet with TTL=0 and no signature
 * 2. Encodes it (which applies padding)
 * 3. Signs over the padded data
 */
bool BitChatBridgeModule::signAnnouncement(BitChatMessage& msg)
{
    if (!ed25519KeysInitialized) {
        initializeBitChatKeys();
    }
    if (!ed25519KeysInitialized) {
        LOG_WARN("BitChat Bridge: Cannot sign - keys not ready");
        return false;
    }
    
    // Create a copy of the message with TTL=0 and no signature for signing
    // This matches Android's toBinaryDataForSigning() format
    BitChatMessage msgForSigning = msg;
    msgForSigning.ttl = 0;  // Use TTL=0 for signing (Android format)
    msgForSigning.flags &= ~BITCHAT_FLAG_HAS_SIGNATURE;  // Remove signature flag
    
    // Serialize the message for signing (full packet binary format)
    uint8_t signingBuffer[1024]; // Larger buffer to accommodate padding
    size_t signingLen = BitChatProtocolHandler::serializeMessage(msgForSigning, signingBuffer, sizeof(signingBuffer));
    if (signingLen == 0) {
        LOG_ERROR("BitChat Bridge: Failed to serialize message for signing");
        return false;
    }
    
    // Apply padding to match Android's BinaryProtocol.encode() behavior
    size_t paddedLen = applyPadding(signingBuffer, signingLen, sizeof(signingBuffer));
    
    // Sign using Ed25519 from rweather/Crypto library (over padded data)
    Ed25519::sign(msg.signature, ed25519SecretKey, ed25519PublicKey, signingBuffer, paddedLen);
    
    // Set signature flag
    msg.flags |= BITCHAT_FLAG_HAS_SIGNATURE;
    
    // Debug: Log first few bytes of what we're signing (for verification)
    LOG_DEBUG("BitChat Bridge: Signed announcement with Ed25519 (packet bytes: %d, padded: %d, signature: %d)", 
              signingLen, paddedLen, BITCHAT_SIGNATURE_SIZE);
    LOG_DEBUG("BitChat Bridge: Signing data preview: %02x %02x %02x %02x ... (total %d bytes)",
              signingBuffer[0], signingBuffer[1], signingBuffer[2], signingBuffer[3], paddedLen);
    LOG_DEBUG("BitChat Bridge: Signature preview: %02x %02x %02x %02x ...",
              msg.signature[0], msg.signature[1], msg.signature[2], msg.signature[3]);
    return true;
}


