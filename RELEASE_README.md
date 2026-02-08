# BitChat Firmware Release

This release contains Meshtastic firmware with BitChat bridge support.

## Installation

### For ESP32 boards (T-Beam, Heltec V3, T-Beam S3, T-Deck)

**Option 1: Web Flasher (Easiest - Drag & Drop!)**
1. Go to https://flasher.meshtastic.org/
2. Connect your device via USB
3. Click "Connect" and select your device
4. Drag and drop the `.bin` file for your board
5. Click "Flash Meshtastic"
6. Wait for it to complete

**Option 2: Command Line**
```bash
# Install esptool if needed: pip install esptool
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 board-name-firmware.bin
```

### For nRF52 boards (RAK4631, T-Echo, Heltec Pocket Qi2)

**Option 1: Web Flasher**
1. Go to https://flasher.meshtastic.org/
2. Connect your device via USB
3. Click "Connect" and select your device
4. Drag and drop the `.hex` file for your board
5. Click "Flash Meshtastic"
6. Wait for it to complete

**Option 2: Command Line (requires nrfjprog)**
```bash
nrfjprog --program board-name-firmware.hex --chiperase --verify --reset
```

## Supported Boards

- **RAK4631** (nRF52840) - `rak4631-firmware.hex`
- **T-Beam** (ESP32) - `tbeam-firmware.bin`
- **Heltec V3** (ESP32S3) - `heltec-v3-firmware.bin`
- **T-Echo** (nRF52840) - `t-echo-firmware.hex`
- **Heltec Pocket Qi2 5Ah** (nRF52840) - `heltec-mesh-pocket-5000-firmware.hex`
- **Heltec Pocket Qi2 10Ah** (nRF52840) - `heltec-mesh-pocket-10000-firmware.hex`
- **T-Beam S3 Core** (ESP32S3) - `tbeam-s3-core-firmware.bin`
- **T-Deck** (ESP32S3) - `t-deck-firmware.bin`

## What's Included

This firmware adds BitChat bridge functionality to Meshtastic, allowing BitChat mobile apps (Android/iOS) to use your Meshtastic device as a LoRa bridge.

### Features
- ✅ BitChat BLE service advertisement
- ✅ Peer-to-peer mesh communication
- ✅ Compatible with BitChat Android/iOS apps
- ✅ All standard Meshtastic features intact

## Verification

To verify file integrity:
```bash
sha256sum -c SHA256SUMS.txt
```

## Troubleshooting

- **Device not found in flasher**: Make sure drivers are installed (CP210x for ESP32, nRF52 drivers for nRF boards)
- **Flash fails**: Try putting device in bootloader mode manually
- **BitChat not working**: Ensure Bluetooth is enabled in device settings

## Support

For issues or questions, please open an issue on GitHub.
