#pragma once
#include "SinglePortModule.h"
#include "BitChatTopologyManager.h"
#include "concurrency/OSThread.h"
#include "mesh/MeshModule.h"
#include <array>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>

#if !MESHTASTIC_EXCLUDE_BLUETOOTH
#ifdef ARCH_ESP32
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <mutex>
#elif defined(ARCH_NRF52)
#include <bluefruit.h>
#endif
#endif

// BitChat Protocol Constants
#define BITCHAT_HEADER_SIZE_V1 13
#define BITCHAT_HEADER_SIZE_V2 16
#define BITCHAT_SIGNATURE_SIZE 64  // Ed25519 signature is 64 bytes
#define BITCHAT_MAX_PAYLOAD_SIZE 245  // Increased to handle larger announcements
#define BITCHAT_VERSION 1
#define BITCHAT_VERSION_1 1
#define BITCHAT_VERSION_2 2
#define BITCHAT_CURRENT_VERSION BITCHAT_VERSION_2
#define BITCHAT_SERVICE_UUID "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"
#define BITCHAT_DUPLICATE_CACHE_SIZE 100
#define BITCHAT_BLE_TIME_WINDOW_MS 30000  // 30 seconds - long enough to be discovered
#define BITCHAT_BLE_INTERVAL_MS 35000     // 35 seconds between windows
#define BITCHAT_MAX_HOPS 8                // Max hops for source routing

// Fragmentation Constants
#define BITCHAT_FRAGMENT_HEADER_SIZE 6    // fragment_id(2) + index(1) + total(1) + original_size(2)
#define BITCHAT_MAX_FRAGMENT_PAYLOAD 211  // 217 (max BitChat payload) - 6 (fragment header)
#define BITCHAT_FRAGMENT_TIMEOUT_MS 30000 // 30 seconds to receive all fragments
#define BITCHAT_MAX_FRAGMENTS 8           // Max fragments per message (support up to ~1.6KB messages)

// BitChat Message Types
enum BitChatMessageType {
    BITCHAT_MSG_ANNOUNCE = 0x01,
    BITCHAT_MSG_MESSAGE = 0x02,
    BITCHAT_MSG_LEAVE = 0x03,
    BITCHAT_MSG_IDENTITY = 0x04,
    BITCHAT_MSG_CHANNEL = 0x05,
    BITCHAT_MSG_PING = 0x06,
    BITCHAT_MSG_PONG = 0x07,
    BITCHAT_MSG_NOISE_HANDSHAKE = 0x10,
    BITCHAT_MSG_NOISE_ENCRYPTED = 0x11,
    BITCHAT_MSG_FRAGMENT_NEW = 0x20,  // New fragment protocol
    BITCHAT_MSG_REQUEST_SYNC = 0x21,  // GCS filter-based sync request
    BITCHAT_MSG_FILE_TRANSFER = 0x22,  // Binary file/audio/image payloads
    BITCHAT_MSG_FRAGMENT = 0xFF  // Special internal type for fragmented messages (legacy)
};

// BLE Management Modes
enum BLEMode {
    BLE_MODE_MESHTASTIC_PRIORITY,
    BLE_MODE_BITCHAT_WINDOW,
    BLE_MODE_BITCHAT_ACTIVE
};

// BitChat Protocol Flags
#define BITCHAT_FLAG_HAS_RECIPIENT 0x01
#define BITCHAT_FLAG_HAS_SIGNATURE 0x02
#define BITCHAT_FLAG_IS_COMPRESSED 0x04
#define BITCHAT_FLAG_HAS_ROUTE     0x08

/**
 * BitChat Protocol Message Structure
 * Matches iOS BinaryProtocol format for compatibility
 */
struct BitChatMessage {
    uint8_t version;        // Protocol version (1)
    uint8_t type;           // Message type (0x01-0x07)
    uint8_t ttl;            // Time to live
    uint64_t timestamp;     // Unix timestamp (8 bytes, milliseconds since epoch)
    uint8_t flags;          // Flags
    uint32_t payloadLength; // Length of payload (4 bytes in V2)
    uint64_t senderId;    // Sender ID (8 bytes, padded)
    uint64_t recipientId; // Recipient ID (8 bytes, optional, based on flags)
    
    // Source Routing (V2)
    uint8_t routeCount;
    uint64_t route[BITCHAT_MAX_HOPS]; // Intermediate hops
    
    uint8_t payload[BITCHAT_MAX_PAYLOAD_SIZE]; // Message payload
    uint8_t signature[BITCHAT_SIGNATURE_SIZE]; // Ed25519 signature
    
    uint64_t getSenderId() const {
        return this->senderId;
    }
    
    // Helper to set senderId from uint32_t
    void setSenderId(uint64_t id) {
        this->senderId = id;
    }

    void decrementTtl() {
        if (this->ttl > 1) {
            this->ttl--;
        }
    }
    
    // Constructor
    BitChatMessage() : version(BITCHAT_CURRENT_VERSION), type(0), ttl(0), timestamp(0), flags(0), payloadLength(0), routeCount(0) {
        senderId = 0;
        recipientId = 0;
        memset(route, 0, sizeof(route));
        memset(payload, 0, sizeof(payload));
        memset(signature, 0, sizeof(signature));
    }
};

/**
 * Duplicate Message Cache for preventing loops
 */
class BitChatDuplicateCache {
private:
    struct CacheEntry {
        uint64_t senderId;
        uint32_t timestamp;
        uint8_t messageType;
        uint32_t hash;
        
        CacheEntry() : senderId(0), timestamp(0), messageType(0), hash(0) {}
    };
    
    std::array<CacheEntry, BITCHAT_DUPLICATE_CACHE_SIZE> cache;
    size_t currentIndex = 0;
    
public:
    bool isDuplicate(const BitChatMessage& msg);
    void addMessage(const BitChatMessage& msg);
    uint32_t calculateHash(const BitChatMessage& msg);
};

/**
 * Fragment Reassembly Buffer
 */
class FragmentReassemblyBuffer {
private:
    struct FragmentBuffer {
        uint16_t fragmentId;
        uint8_t totalFragments;
        uint8_t receivedFragments;
        uint16_t originalSize;
        uint32_t firstFragmentTime;
        bool fragmentsReceived[BITCHAT_MAX_FRAGMENTS];
        BitChatMessage originalMessage;  // Store message metadata
        uint8_t reassemblyBuffer[BITCHAT_MAX_PAYLOAD_SIZE * 2];  // Buffer for reassembly
        
        FragmentBuffer() : fragmentId(0), totalFragments(0), receivedFragments(0), 
                          originalSize(0), firstFragmentTime(0) {
            memset(fragmentsReceived, 0, sizeof(fragmentsReceived));
            memset(reassemblyBuffer, 0, sizeof(reassemblyBuffer));
        }
    };
    
    std::array<FragmentBuffer, 4> buffers;  // Support up to 4 concurrent fragmented messages
    
public:
    bool addFragment(const BitChatMessage& fragment, uint16_t fragmentId, uint8_t index, 
                    uint8_t total, uint16_t originalSize);
    bool isComplete(uint16_t fragmentId);
    bool getReassembledMessage(uint16_t fragmentId, BitChatMessage& outMsg);
    void cleanup(uint32_t currentTime);
};

#if !MESHTASTIC_EXCLUDE_BLUETOOTH
/**
 * BLE Time Manager for coordinating BLE access between Meshtastic and BitChat
 */
class BLETimeManager {
private:
    BLEMode currentMode = BLE_MODE_MESHTASTIC_PRIORITY;
    uint32_t lastModeSwitch = 0;
    uint32_t nextBitChatWindow = 0;
    bool bitchatSessionActive = false;
    
public:
    bool canUseBLEForBitChat();
    void requestBitChatSession();
    void endBitChatSession();
    void update();
    BLEMode getCurrentMode() const { return currentMode; }
};

/**
 * BitChat BLE Service Handler
 */
class BitChatBLEBridge {
private:
    // Reference to bridge module for callbacks
    class BitChatBridgeModule* bridgeModule = nullptr;
    
    // Peripheral role (server) - receives writes from phone
#ifdef ARCH_ESP32
    NimBLEService* bitchatService = nullptr;
    NimBLECharacteristic* bitchatCharacteristic = nullptr;
    std::mutex bleMutex;
#elif defined(ARCH_NRF52)
    BLEService* bitchatService = nullptr;
    BLECharacteristic* bitchatCharacteristic = nullptr;
    
    size_t getPeripheralNotificationLimit();
#endif
    bool serviceActive = false;
    
    // BLE write reassembly buffer (for handling MTU-limited writes from peripheral role)
    // Buffer must accommodate: header(13/16) + sender(8) + recipient(8) + route(var) + payload(245) + signature(64) = ~400 bytes
    // Use slightly larger buffer to be safe
    static constexpr size_t BITCHAT_MAX_MESSAGE_SIZE = 512;
    uint8_t writeBuffer[BITCHAT_MAX_MESSAGE_SIZE];
    size_t writeBufferOffset = 0;
    uint32_t lastWriteTime = 0;
    static constexpr uint32_t WRITE_TIMEOUT_MS = 5000; // Clear buffer if no write for 5 seconds
    
    // Track connected handles for non-V2 NimBLE API workaround
    std::vector<uint16_t> connectedHandles;
    
public:
    void addConnection(uint16_t handle) {
        if (std::find(connectedHandles.begin(), connectedHandles.end(), handle) == connectedHandles.end()) {
            connectedHandles.push_back(handle);
        }
    }
    
    void removeConnection(uint16_t handle) {
        auto it = std::remove(connectedHandles.begin(), connectedHandles.end(), handle);
        connectedHandles.erase(it, connectedHandles.end());
    }
    
    void clearConnections() {
        connectedHandles.clear();
    }
    
    uint16_t getSingleConnectionHandle() {
        if (connectedHandles.size() == 1) {
            return connectedHandles[0];
        }
        return 0xFFFF;
    }

#ifdef ARCH_ESP32
    bool setupBitChatService(NimBLEServer* server);
#elif defined(ARCH_NRF52)
    bool setupBitChatService();
#endif
    void startAdvertising();
    void stopAdvertising();
    void broadcastMessage(const BitChatMessage& msg);
    void unicastMessage(uint16_t connHandle, const BitChatMessage& msg);
    bool isServiceActive() const { return serviceActive; }
    // Set bridge module reference for callbacks
    void setBridgeModule(class BitChatBridgeModule* module) { bridgeModule = module; }
    
    // BLE Callbacks - Peripheral Role (Server)
    void onBitChatWrite(const uint8_t* data, size_t length, uint16_t connHandle);
    void onBitChatConnect();
    void onBitChatDisconnect();
    void handleBitChatDisconnect(uint16_t connHandle, uint8_t reason);
};
#endif

/**
 * Protocol Translation between BitChat and Meshtastic formats
 */
class BitChatProtocolHandler {
public:
    static bool parseMessage(const uint8_t* data, size_t length, BitChatMessage& msg);
    static size_t serializeMessage(const BitChatMessage& msg, uint8_t* buffer, size_t maxLength);
    static bool validateMessage(const BitChatMessage& msg);
    static meshtastic_MeshPacket* createMeshtasticPacket(const BitChatMessage& bitchatMsg);
    static bool extractBitChatMessage(const meshtastic_MeshPacket& meshPacket, BitChatMessage& bitchatMsg);
};

/**
 * Main BitChat Bridge Plugin Module
 * Bridges BitChat protocol messages with Meshtastic mesh network
 */
class BitChatBridgeModule : public SinglePortModule, private concurrency::OSThread
{
private:
    // Core components
    BitChatDuplicateCache duplicateCache;
    FragmentReassemblyBuffer fragmentBuffer;
    BitChatTopologyManager topologyManager; // Tracks direct neighbors
    uint16_t nextFragmentId = 1;  // Counter for generating unique fragment IDs
    
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
    BLETimeManager bleTimeManager;
    BitChatBLEBridge bleBridge;
#endif
    
    // Configuration
    bool bridgeEnabled = true;
    bool bleEnabled = true;
    bool bleServiceSetup = false;  // Track if BLE service has been set up
    uint32_t bleServiceSetupTime = 0;  // When BLE service was set up (for delayed Central scan)
    static constexpr uint32_t CENTRAL_SCAN_DELAY_MS = 2000;  // Delay Central scan by 2 seconds (reduced for faster Android connection)
    uint8_t maxHops = 8;
    uint32_t bleInterval = BITCHAT_BLE_INTERVAL_MS;
    
    // Statistics
    uint32_t messagesRelayed = 0;
    uint32_t messagesBridged = 0;
    uint32_t duplicatesDropped = 0;
    
    // Peer identity - Meshtastic acts as a BitChat peer
    uint32_t myBitChatPeerId = 0;      // Our BitChat peer ID (derived from Meshtastic node ID)
    uint32_t lastAnnounceTime = 0;     // Last time we sent an announcement
    static constexpr uint32_t ANNOUNCE_INTERVAL_MS = 30000; // Announce every 30 seconds
    
    // Time synchronization from BLE peers
    int64_t timeOffsetMs = 0;          // Offset to add to getTime() to get real Unix time
    bool timeSynced = false;           // Whether we've synced time from a BLE peer
    
    // Ed25519 signing keys for BitChat announcements
    // Using rweather/Crypto library: private key is 32 bytes, public key is 32 bytes
    uint8_t ed25519SecretKey[32];      // Ed25519 private key (32 bytes for rweather/Crypto)
    uint8_t ed25519PublicKey[32];     // Ed25519 public key (32 bytes)
    bool ed25519KeysInitialized = false; // Track if keys have been generated/loaded
    
    // Rate-limiting for announcements relayed BLE->Mesh (per sender)
    // Only relay one announcement per sender every ANNOUNCE_RELAY_INTERVAL_MS
    struct AnnouncementRateEntry {
        uint32_t senderId;
        uint32_t lastRelayTime; // millis()
    };
    static constexpr size_t MAX_RATE_LIMIT_ENTRIES = 16;
    static constexpr uint32_t ANNOUNCE_RELAY_INTERVAL_MS = 300000; // 5 minutes
    AnnouncementRateEntry announceRateLimit[MAX_RATE_LIMIT_ENTRIES];
    size_t announceRateLimitCount = 0;

    // Message queue for deferring heavy processing from BLE callbacks to main loop
    // BLE callbacks have limited stack, so we queue messages and process them in runOnce()
    // Use simple fixed-size circular buffer (no dynamic allocation, safe for early initialization)
    // Allow a small burst of BLE messages without dropping them
    struct QueuedMessage {
        BitChatMessage msg;
        bool fromBLE;
        uint16_t bleConnHandle;
    };
    static constexpr size_t MAX_QUEUE_SIZE = 8;
    QueuedMessage messageQueue[MAX_QUEUE_SIZE];
    volatile size_t messageQueueHead = 0;  // Index of next message to process
    volatile size_t messageQueueTail = 0;  // Index of next free slot
    volatile size_t messageQueueCount = 0; // Number of messages in queue
    volatile bool shouldSendAnnouncement = false; // Flag to trigger announcement from main loop

public:
    /** Constructor */
    BitChatBridgeModule();
    
    /** Destructor */
    virtual ~BitChatBridgeModule();
    
protected:
    /** Called to handle a particular incoming message from mesh */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    
    /** Periodic tasks */
    virtual int32_t runOnce() override;
    
    /** Module initialization */
    virtual void setup() override;
    
public:
    // BitChat message handling
    void processBitChatMessage(BitChatMessage& msg, bool fromBLE = false, uint16_t bleConnHandle = 0xFFFF);
    void queueMessageForProcessing(const BitChatMessage& msg, bool fromBLE, uint16_t bleConnHandle = 0xFFFF); // Queue message for deferred processing
    void broadcastToBLE(const BitChatMessage& msg);
    void unicastToBLE(uint16_t connHandle, const BitChatMessage& msg);
    void relayToMesh(const BitChatMessage& msg);
    
    // Configuration
    void setBridgeEnabled(bool enabled) { bridgeEnabled = enabled; }
    void setBLEEnabled(bool enabled) { bleEnabled = enabled; }
    void setMaxHops(uint8_t hops) { maxHops = hops; }
    void setBLEInterval(uint32_t interval) { bleInterval = interval; }
    
    // Statistics
    uint32_t getMessagesRelayed() const { return messagesRelayed; }
    uint32_t getMessagesBridged() const { return messagesBridged; }
    uint32_t getDuplicatesDropped() const { return duplicatesDropped; }
    
    // Plugin configuration interface
    bool handleConfigMessage(const meshtastic_AdminMessage* request, meshtastic_AdminMessage* response);
    
    // Peer announcement (public so BLE bridge can call on connection)
    void sendPeerAnnouncement();
    void requestPeerAnnouncement();
    
private:
    // Internal helpers
    bool shouldRelayMessage(const BitChatMessage& msg);
    bool isAnnouncementRateLimited(const BitChatMessage& msg);
    void updateStatistics();
    void logMessage(const BitChatMessage& msg, const char* action);
    
    // Peer announcement helper
    BitChatMessage createPeerAnnouncement();
    
    // Ed25519 signing helpers
    void initializeEd25519Keys();
    bool signAnnouncement(BitChatMessage& msg);
    
    // Fragmentation helpers
    std::vector<BitChatMessage> fragmentMessage(const BitChatMessage& msg);
    bool handleFragment(const BitChatMessage& fragment);
};

extern BitChatBridgeModule *bitchatBridgeModule;
