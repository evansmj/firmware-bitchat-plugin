# AI Agent Context — firmware-bitchat-plugin

## Project Overview

This is a **Meshtastic firmware fork** with a custom **BitChat BLE-to-LoRa bridge plugin**. The plugin bridges BitChat's Bluetooth Low Energy protocol (~100m range) over Meshtastic's LoRa mesh network (15km+ per hop). Meshtastic devices act as both relay bridges and full BitChat peers. BitChat mobile apps (iOS/Android) require zero modifications.

```
BitChat App <-> BLE <-> Meshtastic Node (Plugin) <-> LoRa Mesh <-> Remote Node <-> BLE <-> BitChat App
  [100m]                  [Bridge/Peer]             [15km+]         [Bridge/Peer]          [100m]
```

Most of the 713+ source files are upstream Meshtastic. The BitChat plugin is the custom code.

## Two Target Architectures

|                      | ESP32                                  | nRF52                                      |
|----------------------|----------------------------------------|--------------------------------------------|
| **Arch macro**       | `ARCH_ESP32`                           | `ARCH_NRF52`                               |
| **BLE stack**        | NimBLE (`<NimBLEDevice.h>`)            | Bluefruit (`<bluefruit.h>`)                |
| **Platform dir**     | `src/platform/esp32/`                  | `src/platform/nrf52/`                      |
| **Arch config**      | `arch/esp32/esp32.ini`                 | `arch/nrf52/nrf52.ini`                     |
| **WiFi**             | Yes                                    | No                                         |
| **Crypto**           | Software (rweather/Crypto)             | Hardware accelerated                       |
| **BLE service init** | `setupBitChatService(NimBLEServer*)`   | `setupBitChatService()` (no args)          |
| **Mutex**            | `std::mutex` available                 | NOT available                              |
| **Excluded features**| Fewer exclusions                       | No WiFi, HTTP, audio, paxcounter           |
| **Example boards**   | tbeam, heltec_v1/v2/v3, tlora, t-deck | rak4631, t-echo, canaryone, heltec pocket  |

**Critical**: Any BLE code MUST be wrapped in `#ifdef ARCH_ESP32` / `#elif defined(ARCH_NRF52)` blocks. Never mix NimBLE and Bluefruit APIs. Both paths must compile independently. Never use `std::mutex` inside nRF52 code paths.

## BitChat Plugin Source Files (the custom code)

### Core plugin files (6 files):
- `src/modules/BitChatBridgeModule.h` — Header: all structs, classes, constants, protocol definitions
- `src/modules/BitChatBridgeModule.cpp` — Main module: setup, runOnce loop, message routing, Ed25519 signing, fragmentation
- `src/modules/BitChatBLEBridge.cpp` — BLE service: platform-split ESP32/nRF52 implementations, notification broadcasting, write reassembly
- `src/modules/BitChatProtocolHandler.cpp` — Binary protocol parsing/serialization, Meshtastic packet wrapping/unwrapping
- `src/modules/BitChatDuplicateCache.cpp` — FNV-1a hash duplicate detection with circular buffer
- `src/modules/BitChatFragmentBuffer.cpp` — Fragment reassembly with timeout cleanup

### Modified Meshtastic files:
- `src/BluetoothCommon.h/.cpp` — Exports `BITCHAT_SERVICE_UUID_16` and `BITCHAT_CHARACTERISTIC_UUID_16`
- `src/nimble/NimbleBluetooth.cpp` — ESP32 BLE integration (gated by `!MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE`)
- `src/platform/nrf52/NRF52Bluetooth.cpp` — nRF52 BLE integration (same gate)
- `src/modules/Modules.cpp` — Registers `bitchatBridgeModule` in the module list
- `src/configuration.h` — Feature flag: `MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE`

## BitChat Protocol Format

```
Header (13 bytes): version(1) | type(1) | TTL(1) | timestamp(8) | flags(1) | payloadLength(2)
Body:              senderID(8) | [recipientID(8)] | payload(N) | [signature(64)]
Meshtastic wrap:   magic "BCHT"(4) | full BitChat message
```

- **Flags**: `0x01` = has recipient, `0x02` = has signature, `0x04` = compressed
- **Message types**: ANNOUNCE(0x01), MESSAGE(0x02), LEAVE(0x03), IDENTITY(0x04), CHANNEL(0x05), PING(0x06), PONG(0x07), NOISE_HANDSHAKE(0x10), NOISE_ENCRYPTED(0x11), FRAGMENT_NEW(0x20), REQUEST_SYNC(0x21), FILE_TRANSFER(0x22), FRAGMENT(0xFF)
- **Announcement TLV payload**: 0x01=nickname, 0x02=Noise pubkey(32B), 0x03=Ed25519 signing pubkey(32B)
- **Signature**: Ed25519 over serialized message with TTL=0, PKCS#7 padded to block boundary (matching Android BinaryProtocol format)

## Build System

PlatformIO-based. Key commands:

```bash
# Build for specific board
pio run -e tbeam          # ESP32
pio run -e rak4631        # nRF52

# Build all ESP32 targets
bin/build-esp32.sh

# Build all nRF52 targets
bin/build-nrf52.sh
```

- Architecture configs: `arch/esp32/esp32.ini`, `arch/nrf52/nrf52.ini`
- Board variants: `variants/<arch>/<board>/`
- Disable plugin: uncomment `#define MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE 1` in `src/configuration.h`

## Coding Conventions

- **Logging**: Use `LOG_INFO()`, `LOG_DEBUG()`, `LOG_WARN()`, `LOG_ERROR()` — never `Serial.println()`
- **Log prefixes**: `"BitChat Bridge:"`, `"BitChat BLE:"`, `"BitChat Fragment:"`, `"BitChat:"`
- **Module pattern**: Extend `SinglePortModule` for mesh messages; use `meshtastic_PortNum_PRIVATE_APP`
- **Threading**: Use `concurrency::OSThread` with `runOnce()` for periodic tasks
- **Memory**: Fixed-size arrays preferred over dynamic allocation. Circular buffers for caches/queues. Especially constrained on nRF52.
- **BLE callbacks**: Limited stack — queue messages via `queueMessageForProcessing()`, process in `runOnce()`
- **Timestamps**: Milliseconds since Unix epoch (`uint64_t`), matching iOS/Android BitChat format
- **Peer ID**: Derived from `nodeDB->getNodeNum()` — stable across reboots
- **Crypto**: Ed25519 via `rweather/Crypto` library (`<Ed25519.h>`, `<RNG.h>`)
- **Non-interference**: Plugin must not break existing Meshtastic BLE, Serial, or mesh routing

## Key Meshtastic APIs

```cpp
router->allocForSending()                    // Allocate mesh packet
service->sendToMesh(packet, RX_SRC_LOCAL)    // Send to mesh
nodeDB->getNodeNum()                         // Local node ID
owner.long_name                              // Device display name
config.bluetooth.enabled                     // BLE enabled check
getTime()                                    // Device time (seconds since epoch)
millis()                                     // Uptime (milliseconds)
packetPool.release(packet)                   // Free packet on error
```

## Testing

- Flash firmware, connect BitChat iOS/Android app via BLE
- Verify "people nearby" counter increments on the app
- Check serial logs for `"BitChat Bridge:"` prefixed messages
- Startup logs: `"Setting up module"`, `"Acting as peer ID 0x..."`, `"Module setup complete"`
- Announcements every 30 seconds in logs

## Documentation

- `BITCHAT_PLUGIN.md` — Full plugin documentation
- `MESHTASTIC_PEER_ANNOUNCEMENT.md` — Peer announcement protocol details
- `RELEASE_README.md` — Release instructions with board support matrix
