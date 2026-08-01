# AGENTS — ESP32 BLE advertising (`src/nimble/`)

ESP32/ESP32-S3 BLE via NimBLE (`NimbleBluetooth.cpp/.h`). Read the repo-root `AGENTS.md` "Critical BLE Gotchas" first. Everything here is gated by `!MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE`.

## Alternating-UUID advertising (the iOS discovery fix — do not undo)

iOS only matches its scan filter against the **main advertising packet**, and a 31-byte legacy advertisement fits only ONE 128-bit UUID. So we alternate which UUID sits in the main packet:

- `bool bitChatAdvMainIsBitChat` (member): `false` → Meshtastic UUID in main / BitChat UUID in scan response; `true` → swapped.
- `startAdvertising()` reads that flag and places `MESH_SERVICE_UUID` vs `BITCHAT_SERVICE_UUID` accordingly, in BOTH code paths: the `NIMBLE_TWO` path (`NimBLEExtAdvertisement`) and the **old-API path** (`NimBLEAdvertising`, which is what stock ESP32/S3 boards on NimBLE-Arduino 1.4.x actually compile — `NIMBLE_TWO` is only defined for esp32c6).
- `swapBitChatAdvertising()` flips the flag and calls `startAdvertising()`. It **early-returns while connected / deferred / deInit** — never disturb a live link or an in-progress connection. Driven on a timer from `BitChatBridgeModule::runOnce()` (`ADV_ALTERNATE_INTERVAL_MS`).

**Do not** move a UUID permanently into the scan response to "save a swap" — that makes either the Meshtastic app or BitChat invisible on iOS. **Do not** enable `CONFIG_BT_NIMBLE_EXT_ADV` casually to fit both UUIDs: it swaps the advertising API for the whole firmware (Meshtastic core included) and is high-risk.

## UUID macro names (easy to get wrong — they differ per platform)

- Here (NimBLE) use the **string** macros: `MESH_SERVICE_UUID` and `BITCHAT_SERVICE_UUID` (defined in `BluetoothCommon.h` / `BitChatBridgeModule.h`). Pass them to `NimBLEUUID(const char*)`. A `const uint8_t*` variable will NOT compile against `NimBLEUUID` (it has no bare `const uint8_t*` ctor). The nRF52 side uses the `_16` byte-array variants instead — don't cross them.
- Note the misnomer: `BITCHAT_SERVICE_UUID_16` is a full **128-bit** UUID (little-endian bytes), matching iOS `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C`. Not a 16-bit UUID.

## Advertising lifecycle safety

- NimBLE **stops advertising on connect**; it must be restarted on disconnect (`onDisconnect` callback → `startAdvertising()`), plus `ensureAdvertising()` is a periodic safety net called from `runOnce()`.
- `ensureAdvertising()` must stay cheap/idempotent and must NOT reconfigure (`reset()`) while advertising is up — doing that on a timer tears down in-progress connections (past cause of flaky ESP32 connects). Full reconfigure happens only in `startAdvertising()` (setup, disconnect, and the ~4s alternation).
- `startAdvertising()` respects `bitChatAdvertisingDeferred` — advertising is deferred at boot until the BitChat GATT service is ready (`setBitChatServiceReady()`), so phones don't cache an incomplete GATT table.

## Names

Advertising/scan-response use a shortened name (`Mes_xxxx`) to fit the 128-bit UUID in 31 bytes; the full name lives in the GATT Device Name (0x2A00). A node showing a full `Meshtastic_xxxx` name in a scanner is NOT running the plugin advertising path.

## Always verify on a real iPhone after changes (Android won't reveal iOS breakage).
