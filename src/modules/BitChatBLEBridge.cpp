#include "BitChatBridgeModule.h"
#include "configuration.h"
#include "BluetoothCommon.h"

#if !MESHTASTIC_EXCLUDE_BLUETOOTH

#ifdef ARCH_ESP32
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>

// Forward declaration for callback class
class BitChatBLECharacteristicCallbacks;

// Global pointer to the active bridge instance for callbacks
static BitChatBLEBridge* activeBridge = nullptr;

/**
 * BLE Characteristic Callbacks for BitChat service
 */
class BitChatBLECharacteristicCallbacks : public NimBLECharacteristicCallbacks {
public:
#ifdef NIMBLE_TWO
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        if (activeBridge) {
            auto value = pCharacteristic->getValue();
            activeBridge->onBitChatWrite(reinterpret_cast<const uint8_t*>(value.data()), value.length(), connInfo.getConnHandle());
        }
    }
#else
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        if (activeBridge) {
            auto value = pCharacteristic->getValue();
            // On older NimBLE, connection handle isn't provided directly in onWrite.
            // Workaround: Use the tracked handle if there is exactly one connection.
            uint16_t handle = activeBridge->getSingleConnectionHandle();
            activeBridge->onBitChatWrite(reinterpret_cast<const uint8_t*>(value.data()), value.length(), handle);
        }
    }
#endif

    void onRead(NimBLECharacteristic* pCharacteristic) override {
        LOG_INFO("BitChat BLE: Characteristic READ by client");
        if (activeBridge) {
            activeBridge->onBitChatConnect();
        }
    }
    
    void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) override {
        if (subValue == 1) {
            LOG_INFO("BitChat BLE: Client SUBSCRIBED to notifications (handle=%d)", desc->conn_handle);
            if (activeBridge) {
                // Track this connection
                activeBridge->addConnection(desc->conn_handle);
                activeBridge->onBitChatConnect();
            }
        } else if (subValue == 0) {
            LOG_INFO("BitChat BLE: Client UNSUBSCRIBED from notifications (handle=%d)", desc->conn_handle);
            if (activeBridge) {
                // Remove tracked connection
                activeBridge->removeConnection(desc->conn_handle);
                activeBridge->onBitChatDisconnect();
            }
        }
    }
};

/**
 * BLE Server Callbacks for connection management
 */
class BitChatBLEServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer) override {
        LOG_INFO("BitChat BLE: Client CONNECTED to BitChat service");
        if (activeBridge) {
            activeBridge->onBitChatConnect();
        }
    }
    
    void onDisconnect(NimBLEServer* pServer) override {
        LOG_INFO("BitChat BLE: Client DISCONNECTED from BitChat service");
        if (activeBridge) {
            // Check connected count to clean up
            if (pServer->getConnectedCount() == 0) {
                activeBridge->clearConnections();
            }
            activeBridge->onBitChatDisconnect();
        }
    }
};

static BitChatBLECharacteristicCallbacks* characteristicCallbacks = nullptr;
static BitChatBLEServerCallbacks* serverCallbacks = nullptr;

#elif defined(ARCH_NRF52)
// nRF52 uses Bluefruit BLE
#include <bluefruit.h>
extern "C" {
    #include "platform/nrf52/softdevice/ble_gatts.h"
    #include "platform/nrf52/softdevice/ble.h"
}
#include <algorithm>

// Global pointer to the active bridge instance for callbacks
static BitChatBLEBridge* activeBridge = nullptr;

// Callback functions for nRF52 Bluefruit
void bitchat_characteristic_write_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len)
{
    if (activeBridge) {
        activeBridge->onBitChatWrite(data, len, conn_hdl);
    }
}

void bitchat_cccd_write_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint16_t value)
{
    // When iOS/Android enables notifications (value=0x0001), log it
    if (value == 0x0001 && activeBridge) {
        LOG_INFO("BitChat BLE: Client enabled notifications (conn=%d)", conn_hdl);
        // Announcement will be sent by the main loop
    } else if (value == 0x0000 && activeBridge) {
        LOG_INFO("BitChat BLE: Client disabled notifications");
    }
}

size_t BitChatBLEBridge::getPeripheralNotificationLimit()
{
    // Original Meshtastic uses BANDWIDTH_MAX which gives 247-byte MTU
    // Usable payload = 247 - 3 (ATT header) = 244 bytes
    return 244;
}

extern void onConnect(uint16_t conn_handle);
extern void onDisconnect(uint16_t conn_handle, uint8_t reason);

void bitchat_connect_callback(uint16_t conn_handle)
{
    onConnect(conn_handle);
    LOG_INFO("BitChat BLE: Peripheral connection established (handle=%d)", conn_handle);
    
    if (activeBridge) {
        activeBridge->onBitChatConnect();
    }
}

void bitchat_disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    onDisconnect(conn_handle, reason);
    
    if (activeBridge) {
        activeBridge->handleBitChatDisconnect(conn_handle, reason);
    } else {
        LOG_INFO("BitChat BLE: Disconnect (handle=%d, reason=0x%02x) with no active bridge", conn_handle, reason);
    }
}

#endif // ARCH_ESP32 / ARCH_NRF52

// BLE Time Manager Implementation (shared across architectures)

bool BLETimeManager::canUseBLEForBitChat()
{
    uint32_t currentTime = millis();
    
    switch (currentMode) {
        case BLE_MODE_MESHTASTIC_PRIORITY:
            // Check if it's time for a BitChat window
            if (currentTime >= nextBitChatWindow) {
                currentMode = BLE_MODE_BITCHAT_WINDOW;
                lastModeSwitch = currentTime;
                nextBitChatWindow = currentTime + BITCHAT_BLE_INTERVAL_MS;
                LOG_DEBUG("BitChat BLE: Switching to BitChat window");
                return true;
            }
            return false;
            
        case BLE_MODE_BITCHAT_WINDOW:
            // Stay in BitChat window for the designated time
            if (currentTime - lastModeSwitch < BITCHAT_BLE_TIME_WINDOW_MS) {
                return true;
            } else {
                // Window expired, go back to Meshtastic priority
                currentMode = BLE_MODE_MESHTASTIC_PRIORITY;
                LOG_DEBUG("BitChat BLE: BitChat window expired, returning to Meshtastic priority");
                return false;
            }
            
        case BLE_MODE_BITCHAT_ACTIVE:
            // Active session - keep it going
            return true;
            
        default:
            return false;
    }
}

void BLETimeManager::requestBitChatSession()
{
    if (currentMode != BLE_MODE_BITCHAT_ACTIVE) {
        currentMode = BLE_MODE_BITCHAT_ACTIVE;
        lastModeSwitch = millis();
        bitchatSessionActive = true;
        LOG_DEBUG("BitChat BLE: Starting active session");
    }
}

void BLETimeManager::endBitChatSession()
{
    if (currentMode == BLE_MODE_BITCHAT_ACTIVE) {
        currentMode = BLE_MODE_MESHTASTIC_PRIORITY;
        lastModeSwitch = millis();
        bitchatSessionActive = false;
        nextBitChatWindow = millis() + BITCHAT_BLE_INTERVAL_MS;
        LOG_DEBUG("BitChat BLE: Ending active session");
    }
}

void BLETimeManager::update()
{
    // Check for session timeout in active mode
    if (currentMode == BLE_MODE_BITCHAT_ACTIVE && bitchatSessionActive) {
        uint32_t currentTime = millis();
        const uint32_t SESSION_TIMEOUT = 30000; // 30 seconds
        
        if (currentTime - lastModeSwitch > SESSION_TIMEOUT) {
            LOG_DEBUG("BitChat BLE: Active session timeout");
            endBitChatSession();
        }
    }
}

// BitChat BLE Bridge Implementation

#ifdef ARCH_ESP32
// ESP32 Implementation using NimBLE
bool BitChatBLEBridge::setupBitChatService(NimBLEServer* server)
{
    if (!server) {
        LOG_ERROR("BitChat BLE: No BLE server available");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(bleMutex);
    
    try {
        // Create BitChat service
        bitchatService = server->createService(NimBLEUUID(BITCHAT_SERVICE_UUID));
        if (!bitchatService) {
            LOG_ERROR("BitChat BLE: Failed to create BitChat service");
            return false;
        }
        
        // Create BitChat characteristic
        bitchatCharacteristic = bitchatService->createCharacteristic(
            NimBLEUUID(BITCHAT_CHARACTERISTIC_UUID),
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
        );
        
        if (!bitchatCharacteristic) {
            LOG_ERROR("BitChat BLE: Failed to create BitChat characteristic");
            return false;
        }
        
        // Set initial empty value
        bitchatCharacteristic->setValue((uint8_t*)nullptr, 0);
        LOG_DEBUG("BitChat BLE: Created characteristic with READ, WRITE, WRITE_NR, NOTIFY (OPEN permissions)");
        
        // Set up callbacks
        if (!characteristicCallbacks) {
            characteristicCallbacks = new BitChatBLECharacteristicCallbacks();
        }
        
        bitchatCharacteristic->setCallbacks(characteristicCallbacks);
        activeBridge = this; // Set global pointer for callbacks
        
        // Start the service
        bitchatService->start();
        serviceActive = true;
        
        LOG_INFO("BitChat BLE: Service setup complete (characteristic callbacks registered)");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("BitChat BLE: Exception during service setup: %s", e.what());
        return false;
    }
}

#elif defined(ARCH_NRF52)
// nRF52 Implementation using Bluefruit
bool BitChatBLEBridge::setupBitChatService()
{
    LOG_INFO("BitChat BLE: Setting up nRF52 Bluefruit service");
    
    // Allocate and configure the BLE service if not already created
    if (!bitchatService) {
        bitchatService = new BLEService(BLEUuid(BITCHAT_SERVICE_UUID_16));
    }
    if (!bitchatService) {
        LOG_ERROR("BitChat BLE: Failed to allocate service object");
        return false;
    }
    
    // Begin the service (returns void, but registers with the GATT table)
    bitchatService->begin();
    
    // Allocate and configure the characteristic
    if (!bitchatCharacteristic) {
        bitchatCharacteristic = new BLECharacteristic(BLEUuid(BITCHAT_CHARACTERISTIC_UUID_16));
    }
    if (!bitchatCharacteristic) {
        LOG_ERROR("BitChat BLE: Failed to allocate characteristic object");
        return false;
    }
    
    bitchatCharacteristic->setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP | CHR_PROPS_NOTIFY);
    
    // No pin for BitChat connections needed
    bitchatService->setPermission(SECMODE_OPEN, SECMODE_OPEN);
    bitchatCharacteristic->setPermission(SECMODE_OPEN, SECMODE_OPEN);
    
    bitchatCharacteristic->setMaxLen(244);
    bitchatCharacteristic->setWriteCallback(bitchat_characteristic_write_callback);
    bitchatCharacteristic->setCccdWriteCallback(bitchat_cccd_write_callback);
    
    bitchatCharacteristic->begin();

    // Ensure the CCCD descriptor exists so clients can enable notifications
    ble_gatts_char_handles_t handles = bitchatCharacteristic->handles();
    if (handles.cccd_handle == BLE_GATT_HANDLE_INVALID) {
        uint8_t cccd_value[2] = {0x00, 0x00};
        err_t cccdResult = bitchatCharacteristic->addDescriptor(BLEUuid((uint16_t)BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG), cccd_value, sizeof(cccd_value));
        if (cccdResult != ERROR_NONE) {
            LOG_WARN("BitChat BLE: Failed to add CCCD descriptor, err=%d", cccdResult);
        } else {
            LOG_INFO("BitChat BLE: CCCD descriptor added manually");
            handles = bitchatCharacteristic->handles();
        }
    }

    LOG_INFO("BitChat BLE: Char handles value=%d cccd=%d", handles.value_handle, handles.cccd_handle);
    
    LOG_INFO("BitChat BLE: Service setup complete");
    
    Bluefruit.Periph.setConnectCallback(bitchat_connect_callback);
    Bluefruit.Periph.setDisconnectCallback(bitchat_disconnect_callback);
    
    activeBridge = this;
    serviceActive = true;
    LOG_INFO("BitChat BLE: nRF52 service setup complete");
    
    return true;
}

#endif // ARCH_ESP32 / ARCH_NRF52

void BitChatBLEBridge::startAdvertising()
{
    if (!serviceActive) {
        LOG_WARN("BitChat BLE: Cannot advertise - service not active");
        return;
    }
    
#ifdef ARCH_ESP32
    LOG_DEBUG("BitChat BLE: Service registered");
    
#elif defined(ARCH_NRF52)
    // On nRF52, we don't manage advertising directly
    // Our service is already added via bitchatService.begin()
    // Meshtastic's advertising will include all registered services
    LOG_DEBUG("BitChat BLE: Service registered, will be advertised with Meshtastic");
#endif
}

void BitChatBLEBridge::stopAdvertising()
{
#ifdef ARCH_ESP32
    LOG_DEBUG("BitChat BLE: Service remains advertised");
    
#elif defined(ARCH_NRF52)
    // On nRF52, we don't stop Meshtastic's advertising
    // Both services remain advertised continuously
    LOG_DEBUG("BitChat BLE: Service remains advertised with Meshtastic");
#endif
}

void BitChatBLEBridge::broadcastMessage(const BitChatMessage& msg)
{
    // Broadcast sends to ALL connected devices (no handle filtering)
    unicastMessage(0xFFFF, msg);
}

void BitChatBLEBridge::unicastMessage(uint16_t connHandle, const BitChatMessage& msg)
{
    if (!serviceActive) {
        LOG_WARN("BitChat BLE: Cannot send - service not active");
        return;
    }
    
    // Serialize BitChat message once
    uint8_t buffer[BITCHAT_MAX_MESSAGE_SIZE];
    size_t messageSize = BitChatProtocolHandler::serializeMessage(msg, buffer, sizeof(buffer));
    
    if (messageSize == 0) {
        LOG_ERROR("BitChat BLE: Failed to serialize message for send");
        return;
    }
    
#ifdef ARCH_ESP32
    std::lock_guard<std::mutex> lock(bleMutex);
    
    try {
        if (!bitchatCharacteristic) {
            LOG_WARN("BitChat BLE: Cannot send - characteristic not initialized");
            return;
        }
        
        // Set characteristic value (for Read requests)
        bitchatCharacteristic->setValue(buffer, messageSize);
        
        // Send Notification
        if (connHandle == 0xFFFF) {
            // Broadcast to all subscribed clients
            bitchatCharacteristic->notify();
            LOG_DEBUG("BitChat BLE: ESP32 broadcasted message type=0x%02x, %d bytes", msg.type, messageSize);
        } else {
            // Unicast to specific connection
#ifdef NIMBLE_TWO
            bitchatCharacteristic->notify(buffer, messageSize, true, connHandle);
            LOG_DEBUG("BitChat BLE: ESP32 unicast to handle %d, %d bytes", connHandle, messageSize);
#else
            // Old API doesn't support targeted notify easily?
            // Fallback to broadcast or check if we can filter
            // NimBLECharacteristic::notify() usually sends to all subscribed.
            // If we can't target, we must broadcast.
            bitchatCharacteristic->notify(); 
            LOG_WARN("BitChat BLE: ESP32 unicast fallback to broadcast (old API)");
#endif
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("BitChat BLE: Exception during message send: %s", e.what());
    }
    
#elif defined(ARCH_NRF52)
    if (!bitchatCharacteristic) {
        LOG_WARN("BitChat BLE: Cannot send - characteristic not initialized");
        return;
    }

    // BLE notifications are limited by MTU
    size_t peripheralLimit = getPeripheralNotificationLimit();
    
    if (messageSize > peripheralLimit) {
        LOG_WARN("BitChat BLE: Message %d bytes > %zu (peripheral MTU payload limit), skipping notify",
                 messageSize, peripheralLimit);
        // We can't send this large message via notify.
        // Android/iOS might read it if we set value, but notify is how push works.
        // Fragmentation should have handled this?
        // No, BitChat fragmentation is for Mesh. BLE MTU is separate.
        // But we can't easily fragment at BLE level without a protocol.
        // We just drop/warn for now if it exceeds MTU.
    } else {
        bitchatCharacteristic->write(buffer, messageSize);
        
        if (connHandle == 0xFFFF) {
            // Broadcast
            bool notifyResult = bitchatCharacteristic->notify(buffer, messageSize);
            LOG_DEBUG("BitChat BLE: nRF52 broadcast result=%d", notifyResult);
        } else {
            // Unicast using notify(conn_hdl, ...)
            bool notifyResult = bitchatCharacteristic->notify(connHandle, buffer, messageSize);
            LOG_DEBUG("BitChat BLE: nRF52 unicast to %d result=%d", connHandle, notifyResult);
        }
    }
#endif
}

void BitChatBLEBridge::onBitChatWrite(const uint8_t* data, size_t length, uint16_t connHandle)
{
    uint32_t currentTime = millis();
    
    // Check for timeout - clear buffer if too much time passed since last write
    if (writeBufferOffset > 0 && (currentTime - lastWriteTime) > WRITE_TIMEOUT_MS) {
        LOG_WARN("BitChat BLE: Write timeout, clearing buffer (%d bytes lost)", writeBufferOffset);
        writeBufferOffset = 0;
    }
    
    // Check if this looks like the START of a new message (has valid header)
    // We check V1 (13) and V2 (16) minimums.
    size_t minHeader = BITCHAT_HEADER_SIZE_V1;
    
    if (length >= minHeader && writeBufferOffset > 0) {
        // This looks like a new message header while we have buffered data
        // Discard the old buffer and start fresh with this new message
        LOG_WARN("BitChat BLE: New message header detected, discarding %d buffered bytes", writeBufferOffset);
        writeBufferOffset = 0;
    }
    
    // Try to parse directly first
    LOG_INFO("BitChat BLE: Received data %d bytes from handle %d", length, connHandle);
    if (length >= 16) {
        LOG_INFO("Raw Header: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
                 data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
    }
    BitChatMessage msg;
    if (BitChatProtocolHandler::parseMessage(data, length, msg)) {
        // Success! Complete message received in one write
        writeBufferOffset = 0;
        
        // Validate message
        if (!BitChatProtocolHandler::validateMessage(msg)) {
            LOG_WARN("BitChat BLE: Invalid message received via BLE (type=0x%02x)", msg.type);
            return;
        }
        
        // Forward to bridge module for processing
        if (bitchatBridgeModule) {
            // Queue for processing in main thread
            bitchatBridgeModule->queueMessageForProcessing(msg, true, connHandle);
        }
        return;
    }
    
    // Parse failed - need to buffer it
    LOG_DEBUG("BitChat BLE: Parse failed, buffering %d bytes", length);
    
    // Check if adding this would overflow
    if (writeBufferOffset + length > sizeof(writeBuffer)) {
        LOG_ERROR("BitChat BLE: Buffer would overflow (%d + %d > %d), discarding buffer",
                  writeBufferOffset, length, sizeof(writeBuffer));
        writeBufferOffset = 0;
        
        if (length > sizeof(writeBuffer)) {
            return;
        }
    }
    
    // Add to reassembly buffer
    memcpy(writeBuffer + writeBufferOffset, data, length);
    writeBufferOffset += length;
    lastWriteTime = currentTime;
    
    // Try to parse the buffered data
    if (BitChatProtocolHandler::parseMessage(writeBuffer, writeBufferOffset, msg)) {
        // Success!
        LOG_INFO("BitChat BLE: Reassembled message from %d bytes", writeBufferOffset);
        
        // Calculate message size
        // We can't rely on fixed calculation since header is variable.
        // But parseMessage succeeded, so we know it's valid.
        // We should clear the buffer.
        
        // Validate message
        if (!BitChatProtocolHandler::validateMessage(msg)) {
            LOG_WARN("BitChat BLE: Invalid message received via BLE (type=0x%02x)", msg.type);
            // Reset buffer
            writeBufferOffset = 0;
            return;
        }
        
        writeBufferOffset = 0;
        
        if (bitchatBridgeModule) {
            bitchatBridgeModule->queueMessageForProcessing(msg, true, connHandle);
        }
    } else {
        // Still incomplete
        LOG_DEBUG("BitChat BLE: Waiting for more data (buffered %d bytes)", writeBufferOffset);
    }
}

void BitChatBLEBridge::handleBitChatDisconnect(uint16_t connHandle, uint8_t reason)
{
    LOG_INFO("BitChat BLE: Client disconnected (handle=%d, reason=0x%02x)", connHandle, reason);
    onBitChatDisconnect();
}

void BitChatBLEBridge::onBitChatConnect()
{
    // Send an immediate announcement via Peripheral notify when Android/iOS connects
    if (bridgeModule) {
        // Request announcement from main thread to avoid stack overflow in BLE callback
        bridgeModule->requestPeerAnnouncement();
    }
}

void BitChatBLEBridge::onBitChatDisconnect()
{
    LOG_INFO("BitChat BLE: Client disconnected");
    
    // End any active session
    // The time manager will handle returning to normal scheduling
}

#endif // !MESHTASTIC_EXCLUDE_BLUETOOTH
