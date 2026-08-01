# AGENTS — nRF52 BLE advertising (`src/platform/nrf52/`)

nRF52840 (rak4631, t-echo, …) BLE via Adafruit Bluefruit (`NRF52Bluetooth.cpp/.h`). Read the repo-root `AGENTS.md` "Critical BLE Gotchas" first. BitChat code is gated by `!MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE`.

## Alternating-UUID advertising (the iOS discovery fix — do not undo)

Same reason as ESP32: iOS matches its scan filter only against the **main advertising packet**, and one 31-byte legacy advertisement fits only ONE 128-bit UUID. We alternate:

- File-scope `static bool bitchatAdvMainIsBitChat` — `false`: Meshtastic UUID in main (`Bluefruit.Advertising.addService(meshBleService)`) / BitChat UUID in scan response (`Bluefruit.ScanResponse.addUuid(BITCHAT_SERVICE_UUID_16)`); `true`: swapped.
- `startAdv()` reads that flag to place the UUIDs. **It rebuilds from scratch on every call**: `Bluefruit.Advertising.stop(); clearData(); ScanResponse.clearData();` at the top — required so it can be re-run for alternation without accumulating stale AD structures or overflowing 31 bytes. It also calls `Bluefruit.Advertising.start(0)` at the end, so it is self-contained.
- `NRF52Bluetooth::swapBitChatAdvertising()` flips the flag and calls `startAdv()`. **Early-returns while `isConnected()`** — never advertise/reconfigure over a live link. Driven on a timer from `BitChatBridgeModule::runOnce()` (`ADV_ALTERNATE_INTERVAL_MS`).
- Do NOT call `resumeAdvertising()` after `startAdv()` — `startAdv()` already starts advertising; doing both double-starts.

**Do not** park one UUID permanently in the scan response — it makes either the Meshtastic app or BitChat invisible on iOS.

## Extended advertising is NOT available here

Adafruit Bluefruit's `BLEAdvertising` is **legacy-only** (payload hard-capped at 31 bytes; no ext-adv / secondary-PHY API). Fitting both 128-bit UUIDs via BLE-5 extended advertising would require raw SoftDevice calls (`sd_ble_gap_adv_set_configure`) that fight Bluefruit's advertising state machine. Not worth it — the alternation approach is the sanctioned fix.

## UUID names & byte budget

- Here use the **`_16` byte-array** variants: `BITCHAT_SERVICE_UUID_16`, and `meshBleService` (wraps `MESH_SERVICE_UUID_16`). These are full **128-bit** UUIDs despite the `_16` name (16 bytes). Pass to `BLEUuid(...)`. The NimBLE side uses the string macros instead — don't cross them.
- Scan-response budget is tight: `TxPower(3) + shortName(~10) + one 128-bit UUID(18) = 31` — exactly at the limit. `addUuid()`/`addService()` returns false on overflow; the code logs `FAILED: Could not add BitChat UUID…`. Watch this if you add anything to the scan response. The main packet holds `flags(3) + one 128-bit UUID(18)`.

## Names & lifecycle

- Advertising uses a shortened name (`Mes_xxxx`) to fit the UUID; full name is restored into the GATT Device Name via `Bluefruit.setName(fullName)` after `startAdv()` configures the packets. A scanner showing full `Meshtastic_xxxx` = plugin advertising path not active.
- `Bluefruit.Advertising.restartOnDisconnect(true)` auto-resumes advertising after a disconnect (nRF52 equivalent of the ESP32 `onDisconnect` restart).

## Platform constraints

No `std::mutex` on nRF52. Flash is extremely tight (rak4631 ~98% full) — keep additions minimal. Any BLE change must also compile on the ESP32/NimBLE path.

## Always verify on a real iPhone after changes (Android won't reveal iOS breakage).
