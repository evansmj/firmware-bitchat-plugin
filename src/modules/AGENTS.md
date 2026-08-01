# AGENTS — BitChat plugin core (`src/modules/`)

Custom BitChat plugin code. This is where identity, the ANNOUNCE packet, signing, protocol (de)serialization, dedup, and fragmentation live. See the repo-root `AGENTS.md` "Critical BLE Gotchas" first.

## Identity & the ANNOUNCE

Files: `BitChatBridgeModule.cpp` (`initializeBitChatKeys`, `createPeerAnnouncement`, `sendPeerAnnouncement`, `signAnnouncement`), `BitChatBridgeModule.h`.

- **Peer ID = `SHA256(noiseStaticPublicKey)[0..8]`** — stored in `myBitChatPeerId8[8]`, set in `initializeBitChatKeys()`. This is what goes in the announce `senderId` (`memcpy(msg.senderId, myBitChatPeerId8, 8)`). It is NOT `nodeDB->getNodeNum()`. iOS re-derives this from the announced Noise key (`PeerID(publicKey:)`) and drops the announce as `.senderMismatch` if it differs. `myBitChatPeerId` (uint32) is now **log-only** (low 4 bytes of the fingerprint).
- **Keys are deterministic** from `config.security.private_key` (32-byte Meshtastic secret) via `deriveBitChatSeed(secret, domainTag, out)` = `SHA256(secret || domainTag)`:
  - Ed25519 signing seed: domain tag `"bitchat-ed25519-v1"`.
  - Noise static X25519: domain tag `"bitchat-noise-v1"`, then manual clamp + `Curve25519::eval` (do NOT use `Curve25519::dh1` — it overwrites the private key with random bytes → different identity every boot).
  - Changing a domain tag changes the node's identity permanently. Don't.
- **Don't announce before keys exist.** `sendPeerAnnouncement()` and `createPeerAnnouncement()` call `initializeBitChatKeys()` and bail if `config.security.private_key` isn't populated yet — otherwise senderID/signature are garbage and every phone rejects the announce.
- **Signature** (`signAnnouncement`): Ed25519 over `serializeMessage(copy with ttl=0, HAS_SIGNATURE flag cleared)` then `applyPadding()` (PKCS#7, block sizes 256/512/1024/2048, `dataLen+16` rule). This must byte-match iOS `BitchatPacket.toBinaryDataForSigning()` (TTL=0, no sig, `BinaryProtocol.encode` + padding) and Android's `toBinaryDataForSigning()`. If you change serialization or padding, you break signature verification on BOTH apps.
- **ANNOUNCE TLV payload**: `0x01`=nickname (owner.long_name, ≤255), `0x02`=Noise pubkey (32B), `0x03`=Ed25519 signing pubkey (32B). Order/lengths matter — parsers are strict.
- **Timestamp** must be real (ms since epoch). No valid time → iOS treats the announce as stale and ignores it. `computeAnnounceTimestampMs()` prefers the RTC, falls back to a peer-learned base.

## senderId field mechanics

- `BitChatMessage::senderId` is `uint8_t[8]`. `setSenderId32()`/`getSenderId32()` only touch the low 4 bytes (little-endian) — fine as an opaque dedup key for *relayed* peers, but our own announce needs the full 8-byte fingerprint, so use `memcpy`, not `setSenderId32`.
- Dedup (`BitChatDuplicateCache`) and LoRa announce rate-limiting key off `getSenderId32()` (32-bit). That's an opaque key; leave it alone.

## Threading / memory (shared with upstream constraints)

- BLE callbacks have a tiny stack: never do real work there. Queue via `queueMessageForProcessing()` and handle in `runOnce()` (processes ≤2 msgs/call).
- No dynamic allocation in hot paths; fixed arrays + circular buffers. nRF52 has **no `std::mutex`** and very tight flash (rak4631 ~98% full) — keep additions small.
- Any BLE-touching code must compile on BOTH `ARCH_ESP32` (NimBLE) and `ARCH_NRF52` (Bluefruit). Guard with `#ifdef`.
