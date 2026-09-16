#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "gps/RTC.h"
#include "mesh/MeshModule.h"
#include <array>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <atomic>

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
#define BITCHAT_HEADER_SIZE 13  // Updated to match iOS format: version(1) + type(1) + ttl(1) + timestamp(8) + flags(1) + payloadLength(2) = 13
#define BITCHAT_SIGNATURE_SIZE 64  // Ed25519 signature is 64 bytes
#define BITCHAT_MAX_PAYLOAD_SIZE 245  // Increased to handle larger announcements
#define BITCHAT_VERSION 1  // Protocol version (matches iOS)
#define BITCHAT_SERVICE_UUID "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"
#define BITCHAT_DUPLICATE_CACHE_SIZE 100
#define BITCHAT_BLE_TIME_WINDOW_MS 30000  // 30 seconds - long enough to be discovered
#define BITCHAT_BLE_INTERVAL_MS 35000     // 35 seconds between windows

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

/**
 * BitChat Protocol Message Structure
 * Matches iOS BinaryProtocol format for compatibility
 */
struct BitChatMessage {
    uint8_t version;        // Protocol version (1)
    uint8_t type;           // Message type (0x01-0x07)
    uint8_t ttl;            // Time to live
    uint64_t timestamp;     // Unix timestamp (8 bytes, milliseconds since epoch)
    uint8_t flags;          // Flags: hasRecipient(0x01), hasSignature(0x02), isCompressed(0x04)
    uint16_t payloadLength; // Length of payload (2 bytes)
    uint8_t senderId[8];    // Sender ID (8 bytes, padded)
    uint8_t recipientId[8]; // Recipient ID (8 bytes, optional, based on flags)
    uint8_t payload[BITCHAT_MAX_PAYLOAD_SIZE]; // Message payload
    uint8_t signature[BITCHAT_SIGNATURE_SIZE]; // Ed25519 signature (64 bytes, optional, based on flags)
    
    // Helper to get senderId as uint32_t (for backward compatibility)
    uint32_t getSenderId32() const {
        uint32_t id = 0;
        for (int i = 0; i < 4; i++) {
            id |= (static_cast<uint32_t>(senderId[i]) << (i * 8));
        }
        return id;
    }
    
    // Helper to set senderId from uint32_t
    void setSenderId32(uint32_t id) {
        memset(senderId, 0, 8);
        for (int i = 0; i < 4; i++) {
            senderId[i] = static_cast<uint8_t>((id >> (i * 8)) & 0xFF);
        }
    }
    
    // Constructor
    BitChatMessage() : version(BITCHAT_VERSION), type(0), ttl(0), timestamp(0), flags(0), payloadLength(0) {
        memset(senderId, 0, sizeof(senderId));
        memset(recipientId, 0, sizeof(recipientId));
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
        uint32_t senderId;
        uint64_t timestamp;
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

/**
 * A queued BitChat message awaiting deferred processing in runOnce().
 */
struct QueuedMessage {
    BitChatMessage msg;
    bool fromBLE = false;
};

/**
 * Lock-free single-producer/single-consumer ring buffer.
 * The producer (BLE callback task) only writes `tail`; the consumer (main task) only
 * writes `head`. One slot is left empty to distinguish full from empty, so usable
 * capacity is CAPACITY - 1. On overflow push() drops the new message (never touches the
 * consumer's `head`, preserving the SPSC invariant). Standalone for unit testing.
 */
// Single-producer/single-consumer ring. On ESP32 the producer (NimBLE host task) and the
// consumer (Arduino loop task) run on different cores, so plain `volatile` is not enough:
// it stops compiler reordering but gives no cross-core visibility ordering, letting the
// consumer observe an advanced `tail` before the slot write behind it is visible. head/tail
// are std::atomic with release/acquire so the slot write is published before the index that
// exposes it, and seen before the slot is read.
struct BitChatMessageRing {
    static constexpr size_t CAPACITY = 9; // 8 usable slots
    QueuedMessage slots[CAPACITY];
    std::atomic<size_t> head{0}; // next slot to read (consumer writes)
    std::atomic<size_t> tail{0}; // next slot to write (producer writes)

    bool empty() const {
        return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_relaxed);
    }
    bool full() const {
        return ((tail.load(std::memory_order_relaxed) + 1) % CAPACITY) == head.load(std::memory_order_acquire);
    }

    // Producer side. Returns false if the ring was full (message dropped).
    bool push(const QueuedMessage& m) {
        size_t t = tail.load(std::memory_order_relaxed);
        size_t next = (t + 1) % CAPACITY;
        if (next == head.load(std::memory_order_acquire)) {
            return false; // full - drop new message
        }
        slots[t] = m;
        tail.store(next, std::memory_order_release); // publish slot before exposing it
        return true;
    }

    // Consumer side. Returns false if the ring was empty.
    bool pop(QueuedMessage& out) {
        size_t h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = slots[h];
        head.store((h + 1) % CAPACITY, std::memory_order_release); // free slot after reading
        return true;
    }
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
    NimBLEServer* bitchatServer = nullptr; // for querying negotiated ATT MTU
    NimBLEService* bitchatService = nullptr;
    NimBLECharacteristic* bitchatCharacteristic = nullptr;
    std::mutex bleMutex;
    size_t getPeripheralNotificationLimit(); // usable notify payload = min peer MTU - 3
#elif defined(ARCH_NRF52)
    BLEService* bitchatService = nullptr;
    BLECharacteristic* bitchatCharacteristic = nullptr;
    
    size_t getPeripheralNotificationLimit();
#endif
    bool serviceActive = false;
    
    // BLE write reassembly buffer (for handling MTU-limited writes from peripheral role)
    // Buffer must accommodate: header(13) + sender(8) + recipient(8) + payload(245) + signature(64) = 338 bytes
    static constexpr size_t BITCHAT_MAX_MESSAGE_SIZE = BITCHAT_HEADER_SIZE + 8 + 8 + BITCHAT_MAX_PAYLOAD_SIZE + BITCHAT_SIGNATURE_SIZE;
    uint8_t writeBuffer[BITCHAT_MAX_MESSAGE_SIZE];
    size_t writeBufferOffset = 0;
    uint32_t lastWriteTime = 0;
    static constexpr uint32_t WRITE_TIMEOUT_MS = 5000; // Clear buffer if no write for 5 seconds
    
public:
#ifdef ARCH_ESP32
    bool setupBitChatService(NimBLEServer* server);
#elif defined(ARCH_NRF52)
    bool setupBitChatService();
#endif
    void startAdvertising();
    void stopAdvertising();
    void broadcastMessage(const BitChatMessage& msg);
    bool isServiceActive() const { return serviceActive; }
    // Set bridge module reference for callbacks
    void setBridgeModule(class BitChatBridgeModule* module) { bridgeModule = module; }
    
    // BLE Callbacks - Peripheral Role (Server)
    void onBitChatWrite(const uint8_t* data, size_t length);
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
    uint16_t nextFragmentId = 1;  // Counter for generating unique fragment IDs
    
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
    BLETimeManager bleTimeManager;
    BitChatBLEBridge bleBridge;
#endif
    
    // Configuration
    bool bridgeEnabled = true;
    bool bleEnabled = true;
    bool bleServiceSetup = false;
    uint32_t nextBleSetupAttempt = 0;  // Backoff gate for retrying BLE service setup
    static constexpr uint32_t BLE_SETUP_RETRY_MS = 3000;
    uint8_t maxHops = 8;
    uint32_t bleInterval = BITCHAT_BLE_INTERVAL_MS;
    
    // Statistics
    uint32_t messagesRelayed = 0;
    uint32_t messagesBridged = 0;
    uint32_t duplicatesDropped = 0;
    
    // Peer identity - Meshtastic acts as a BitChat peer
    uint32_t myBitChatPeerId = 0;      // Low 32 bits of our BitChat peer ID (for logging only)
    // Our 8-byte BitChat peer ID. MUST equal SHA256(noisePublicKey)[0..8]: iOS derives the
    // expected senderID from the announced Noise key (PeerID(publicKey:)) and rejects the
    // announce with .senderMismatch if the packet's senderID doesn't match. Set in
    // initializeBitChatKeys() once the Noise key exists.
    uint8_t myBitChatPeerId8[8] = {0};
    uint32_t lastAnnounceTime = 0;     // Last time we sent an announcement
    static constexpr uint32_t ANNOUNCE_INTERVAL_MS = 30000;
    volatile bool announceRequested = false; // Set by BLE connect callback, serviced in runOnce()

    // Alternating-UUID advertising. A 31-byte legacy BLE advertisement can't hold two
    // 128-bit service UUIDs, so we can only put one in the *main* advertising packet at a
    // time. iOS filters scans on the main packet only, so we periodically swap which UUID
    // sits there (Meshtastic <-> BitChat). This lets both the Meshtastic app and the BitChat
    // apps discover this node on iOS. See NimbleBluetooth/NRF52Bluetooth swapBitChatAdvertising().
    uint32_t lastAdvAlternateTime = 0; // Last time we swapped the main-packet service UUID
    static constexpr uint32_t ADV_ALTERNATE_INTERVAL_MS = 4000;
    
    // Time synchronization.
    // Preferred source is Meshtastic's own RTC (GPS/NTP/mesh/app). Only when the RTC has
    // no valid time do we fall back to an absolute base learned from a BitChat peer's
    // timestamp, advanced by millis() — avoids the double-count bug of adding an offset to
    // a getTime() that later becomes valid.
    bool haveSyncedBase = false;       // Whether a peer-derived absolute base was captured
    uint64_t syncedUnixMs0 = 0;        // Peer Unix time (ms) captured at syncedAtMillis0
    uint32_t syncedAtMillis0 = 0;      // millis() when syncedUnixMs0 was captured
    
    // BitChat identity keys, derived deterministically from Meshtastic's persisted
    // random secret (config.security.private_key) via SHA256 with domain-separation tags.
    // Real entropy, stable across reboots, no extra storage.
    uint8_t ed25519SecretKey[32];      // Ed25519 private key seed (32 bytes for rweather/Crypto)
    uint8_t ed25519PublicKey[32];      // Ed25519 signing public key
    uint8_t noisePrivateKey[32];       // Noise static X25519 private key
    uint8_t noisePublicKey[32];        // Noise static X25519 public key
    bool ed25519KeysInitialized = false;
    
    // Rate-limiting for announcements relayed BLE->Mesh (per sender)
    // Only relay one announcement per sender every ANNOUNCE_RELAY_INTERVAL_MS
    struct AnnouncementRateEntry {
        uint32_t senderId;
        uint32_t lastRelayTime; // millis()
    };
    static constexpr size_t MAX_RATE_LIMIT_ENTRIES = 16;
    static constexpr uint32_t ANNOUNCE_RELAY_INTERVAL_MS = 300000;
    AnnouncementRateEntry announceRateLimit[MAX_RATE_LIMIT_ENTRIES];
    size_t announceRateLimitCount = 0;

    // Message queue for deferring heavy processing from BLE callbacks to the main loop.
    // BLE callbacks have limited stack, so we queue messages and process them in runOnce().
    // Single-producer (BLE callback task) / single-consumer (runOnce, main task) ring —
    // race-free without a lock because each index is written by exactly one side.
    BitChatMessageRing messageQueue;

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
    void processBitChatMessage(const BitChatMessage& msg, bool fromBLE = false);
    void queueMessageForProcessing(const BitChatMessage& msg, bool fromBLE); // Queue message for deferred processing
    void broadcastToBLE(const BitChatMessage& msg);
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
    // Request an announcement be sent from the main loop (safe to call from BLE callbacks)
    void requestAnnouncement() { announceRequested = true; }
    
private:
    // Internal helpers
    bool shouldRelayMessage(const BitChatMessage& msg);
    bool isAnnouncementRateLimited(const BitChatMessage& msg);
    void updateStatistics();
    void logMessage(const BitChatMessage& msg, const char* action);
    
    // Peer announcement helper
    BitChatMessage createPeerAnnouncement();
    
    // Ed25519 signing helpers
    void initializeBitChatKeys();
    bool signAnnouncement(BitChatMessage& msg);

public:
    // Pure helpers (public/static for unit testing) — no global/BLE state.
    // out = SHA256(secret32 || domainTag)
    static void deriveBitChatSeed(const uint8_t secret[32], const char* domainTag, uint8_t out[32]);
    // Whether Meshtastic's RTC quality is good enough to use getTime() as authoritative.
    static bool shouldAdoptRtc(RTCQuality quality) { return quality >= RTCQualityDevice; }
    // Compute the timestamp (ms since epoch) to stamp on an outgoing announcement.
    static uint64_t computeAnnounceTimestampMs(bool rtcValid, uint64_t rtcTimeMs,
                                               bool haveSyncedBase, uint64_t syncedBaseMs,
                                               uint32_t syncedAtMillis, uint32_t nowMillis);

private:
    
    // Fragmentation helpers
    std::vector<BitChatMessage> fragmentMessage(const BitChatMessage& msg);
    bool handleFragment(const BitChatMessage& fragment);
};

extern BitChatBridgeModule *bitchatBridgeModule;
