#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_BLUETOOTH
#include "BluetoothCommon.h"
#include "NimbleBluetooth.h"
#include "PowerFSM.h"

#include "main.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"
#include "sleep.h"
#include <NimBLEDevice.h>
#include <mutex>

#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
#include "modules/BitChatBridgeModule.h"
#endif

#ifdef NIMBLE_TWO
#include "NimBLEAdvertising.h"
#include "NimBLEExtAdvertising.h"
#include "PowerStatus.h"
#endif

NimBLECharacteristic *fromNumCharacteristic;
NimBLECharacteristic *BatteryCharacteristic;
NimBLECharacteristic *logRadioCharacteristic;
NimBLEServer *bleServer;

#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
NimBLEService *bitchatBLEService = nullptr;
NimBLECharacteristic *bitchatBLECharacteristic = nullptr;
#endif

static bool passkeyShowing;

class BluetoothPhoneAPI : public PhoneAPI, public concurrency::OSThread
{
  public:
    BluetoothPhoneAPI() : concurrency::OSThread("NimbleBluetooth") { nimble_queue.resize(3); }
    std::vector<NimBLEAttValue> nimble_queue;
    std::mutex nimble_mutex;
    uint8_t queue_size = 0;
    bool has_fromRadio = false;
    uint8_t fromRadioBytes[meshtastic_FromRadio_size] = {0};
    size_t numBytes = 0;
    bool hasChecked = false;
    bool phoneWants = false;

  protected:
    virtual int32_t runOnce() override
    {
        std::lock_guard<std::mutex> guard(nimble_mutex);
        if (queue_size > 0) {
            for (uint8_t i = 0; i < queue_size; i++) {
                handleToRadio(nimble_queue.at(i).data(), nimble_queue.at(i).length());
            }
            LOG_DEBUG("Queue_size %u", queue_size);
            queue_size = 0;
        }
        if (hasChecked == false && phoneWants == true) {
            numBytes = getFromRadio(fromRadioBytes);
            hasChecked = true;
        }

        return 100;
    }
    /**
     * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
     */
    virtual void onNowHasData(uint32_t fromRadioNum)
    {
        PhoneAPI::onNowHasData(fromRadioNum);

        uint8_t cc = bleServer->getConnectedCount();
        LOG_DEBUG("BLE notify fromNum: %d connections: %d", fromRadioNum, cc);

        uint8_t val[4];
        put_le32(val, fromRadioNum);

        fromNumCharacteristic->setValue(val, sizeof(val));
#ifdef NIMBLE_TWO
        fromNumCharacteristic->notify(val, sizeof(val), BLE_HS_CONN_HANDLE_NONE);
#else
        fromNumCharacteristic->notify();
#endif
    }

    /// Check the current underlying physical link to see if the client is currently connected
    virtual bool checkIsConnected() { return bleServer && bleServer->getConnectedCount() > 0; }
};

static BluetoothPhoneAPI *bluetoothPhoneAPI;
/**
 * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
 */

// Last ToRadio value received from the phone
static uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE];

class NimbleBluetoothToRadioCallback : public NimBLECharacteristicCallbacks
{
#ifdef NIMBLE_TWO
    virtual void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo)
#else
    virtual void onWrite(NimBLECharacteristic *pCharacteristic)

#endif
    {
        auto val = pCharacteristic->getValue();

        if (memcmp(lastToRadio, val.data(), val.length()) != 0) {
            if (bluetoothPhoneAPI->queue_size < 3) {
                memcpy(lastToRadio, val.data(), val.length());
                std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
                bluetoothPhoneAPI->nimble_queue.at(bluetoothPhoneAPI->queue_size) = val;
                bluetoothPhoneAPI->queue_size++;
                bluetoothPhoneAPI->setIntervalFromNow(0);
            }
        }
    }
};

class NimbleBluetoothFromRadioCallback : public NimBLECharacteristicCallbacks
{
#ifdef NIMBLE_TWO
    virtual void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo)
#else
    virtual void onRead(NimBLECharacteristic *pCharacteristic)
#endif
    {
        int tries = 0;
        bluetoothPhoneAPI->phoneWants = true;
        while (!bluetoothPhoneAPI->hasChecked && tries < 100) {
            bluetoothPhoneAPI->setIntervalFromNow(0);
            delay(20);
            tries++;
        }
        std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
        pCharacteristic->setValue(bluetoothPhoneAPI->fromRadioBytes, bluetoothPhoneAPI->numBytes);

        if (bluetoothPhoneAPI->numBytes != 0) // if we did send something, queue it up right away to reload
            bluetoothPhoneAPI->setIntervalFromNow(0);
        bluetoothPhoneAPI->numBytes = 0;
        bluetoothPhoneAPI->hasChecked = false;
        bluetoothPhoneAPI->phoneWants = false;
    }
};

class NimbleBluetoothServerCallback : public NimBLEServerCallbacks
{
#ifdef NIMBLE_TWO
  public:
    NimbleBluetoothServerCallback(NimbleBluetooth *ble) { this->ble = ble; }

  private:
    NimbleBluetooth *ble;

    virtual uint32_t onPassKeyDisplay()
#else
    virtual uint32_t onPassKeyRequest()
#endif
    {
        uint32_t passkey = config.bluetooth.fixed_pin;

        if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN) {
            LOG_INFO("Use random passkey");
            // This is the passkey to be entered on peer - we pick a number >100,000 to ensure 6 digits
            passkey = random(100000, 999999);
        }
        LOG_INFO("*** Enter passkey %d on the peer side ***", passkey);

        powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
        meshtastic::BluetoothStatus newStatus(std::to_string(passkey));
        bluetoothStatus->updateStatus(&newStatus);

#if HAS_SCREEN // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
        if (screen) {
            screen->startAlert([passkey](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
                char btPIN[16] = "888888";
                snprintf(btPIN, sizeof(btPIN), "%06u", passkey);
                int x_offset = display->width() / 2;
                int y_offset = display->height() <= 80 ? 0 : 12;
                display->setTextAlignment(TEXT_ALIGN_CENTER);
                display->setFont(FONT_MEDIUM);
                display->drawString(x_offset + x, y_offset + y, "Bluetooth");

                display->setFont(FONT_SMALL);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
                display->drawString(x_offset + x, y_offset + y, "Enter this code");

                display->setFont(FONT_LARGE);
                char pin[8];
                snprintf(pin, sizeof(pin), "%.3s %.3s", btPIN, btPIN + 3);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
                display->drawString(x_offset + x, y_offset + y, pin);

                display->setFont(FONT_SMALL);
                char deviceName[64];
                snprintf(deviceName, sizeof(deviceName), "Name: %s", getDeviceName());
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
                display->drawString(x_offset + x, y_offset + y, deviceName);
            });
        }
#endif
        passkeyShowing = true;

        return passkey;
    }

#ifdef NIMBLE_TWO
    virtual void onAuthenticationComplete(NimBLEConnInfo &connInfo)
#else
    virtual void onAuthenticationComplete(ble_gap_conn_desc *desc)
#endif
    {
        LOG_INFO("BLE authentication complete");

        meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&newStatus);

        // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
        if (passkeyShowing) {
            passkeyShowing = false;
            if (screen)
                screen->endAlert();
        }
    }

#ifdef NIMBLE_TWO
    virtual void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo)
    {
        LOG_INFO("BLE incoming connection %s", connInfo.getAddress().toString().c_str());
        LOG_DEBUG("NimBLE: onConnect - NimBLE will automatically stop advertising when connected");
        LOG_DEBUG("NimBLE: Advertising will be restarted automatically when this connection disconnects");
    }

    virtual void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason)
    {
        LOG_INFO("BLE disconnect reason: %d", reason);
#else
    virtual void onDisconnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
    {
        LOG_INFO("BLE disconnect");
#endif
#ifdef NIMBLE_TWO
        if (ble->isDeInit)
            return;
#endif

        meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        bluetoothStatus->updateStatus(&newStatus);

        if (bluetoothPhoneAPI) {
            std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
            bluetoothPhoneAPI->close();
            bluetoothPhoneAPI->hasChecked = false;
            bluetoothPhoneAPI->phoneWants = false;
            bluetoothPhoneAPI->numBytes = 0;
            bluetoothPhoneAPI->queue_size = 0;
        }
        // Restart Advertising (for both NIMBLE_TWO and old API)
        // NimBLE stops advertising when connected, so we need to restart on disconnect
        // This matches nRF52's Bluefruit.Advertising.restartOnDisconnect(true) behavior
        LOG_INFO("NimBLE: Disconnect detected, restarting advertising...");
#ifdef NIMBLE_TWO
        if (ble->isDeInit) {
            LOG_WARN("NimBLE: Cannot restart advertising - BLE is deinitialized");
            return;
        }
        LOG_DEBUG("NimBLE: Restarting advertising (NIMBLE_TWO path)");
        ble->startAdvertising();
        LOG_INFO("NimBLE: Advertising restart called (NIMBLE_TWO)");
#else
        // Old NimBLE API: use global nimbleBluetooth (callback class doesn't have ble member)
        if (!nimbleBluetooth) {
            LOG_ERROR("NimBLE: Cannot restart advertising - nimbleBluetooth is null!");
            return;
        }
        if (nimbleBluetooth->isDeInit) {
            LOG_WARN("NimBLE: Cannot restart advertising - BLE is deinitialized");
            return;
        }
        LOG_DEBUG("NimBLE: Restarting advertising (old API path)");
        nimbleBluetooth->startAdvertising();
        LOG_INFO("NimBLE: Advertising restart called (old API)");
#endif
    }
};

static NimbleBluetoothToRadioCallback *toRadioCallbacks;
static NimbleBluetoothFromRadioCallback *fromRadioCallbacks;

void NimbleBluetooth::shutdown()
{
    // No measurable power saving for ESP32 during light-sleep(?)
#ifndef ARCH_ESP32
    // Shutdown bluetooth for minimum power draw
    LOG_INFO("Disable bluetooth");
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->reset();
    pAdvertising->stop();
#endif
}

// Proper shutdown for ESP32. Needs reboot to reverse.
void NimbleBluetooth::deinit()
{
#ifdef ARCH_ESP32
    LOG_INFO("Disable bluetooth until reboot");
    isDeInit = true;

#ifdef BLE_LED
#ifdef BLE_LED_INVERTED
    digitalWrite(BLE_LED, HIGH);
#else
    digitalWrite(BLE_LED, LOW);
#endif
#endif
#ifndef NIMBLE_TWO
    NimBLEDevice::deinit();
#endif
#endif
}

// Has initial setup been completed
bool NimbleBluetooth::isActive()
{
    return bleServer;
}

bool NimbleBluetooth::isConnected()
{
    return bleServer->getConnectedCount() > 0;
}

int NimbleBluetooth::getRssi()
{
    if (bleServer && isConnected()) {
        auto service = bleServer->getServiceByUUID(MESH_SERVICE_UUID);
        uint16_t handle = service->getHandle();
#ifdef NIMBLE_TWO
        return NimBLEDevice::getClientByHandle(handle)->getRssi();
#else
        return NimBLEDevice::getClientByID(handle)->getRssi();
#endif
    }
    return 0; // FIXME figure out where to source this
}

void NimbleBluetooth::setup()
{
    // Uncomment for testing
    // clearBonds();

    LOG_INFO("Init the NimBLE bluetooth module");
    
    const char* deviceName = getDeviceName();
    LOG_DEBUG("NimBLE: Device name is '%s'", deviceName);
    LOG_DEBUG("NimBLE: Bluetooth enabled: %s", config.bluetooth.enabled ? "YES" : "NO");
    LOG_DEBUG("NimBLE: Pairing mode: %d", config.bluetooth.mode);

    LOG_DEBUG("NimBLE: Calling NimBLEDevice::init('%s')...", deviceName);
    NimBLEDevice::init(deviceName);
    LOG_DEBUG("NimBLE: NimBLEDevice::init() completed");
    
    LOG_DEBUG("NimBLE: Setting BLE power level to ESP_PWR_LVL_P9...");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    LOG_DEBUG("NimBLE: Power level set");

    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        LOG_DEBUG("NimBLE: Pairing mode requires PIN, setting up security...");
        NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM | BLE_SM_PAIR_AUTHREQ_SC);
        NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
        LOG_DEBUG("NimBLE: Security configured (requires PIN)");
    } else {
        LOG_DEBUG("NimBLE: Pairing mode is NO_PIN, skipping security setup");
    }
    
    LOG_DEBUG("NimBLE: Creating BLE server...");
    bleServer = NimBLEDevice::createServer();
    LOG_DEBUG("NimBLE: BLE server created");
    
#ifdef NIMBLE_TWO
    LOG_DEBUG("NimBLE: Creating server callbacks (NIMBLE_TWO path)...");
    NimbleBluetoothServerCallback *serverCallbacks = new NimbleBluetoothServerCallback(this);
#else
    LOG_DEBUG("NimBLE: Creating server callbacks (old API path)...");
    NimbleBluetoothServerCallback *serverCallbacks = new NimbleBluetoothServerCallback();
#endif
    LOG_DEBUG("NimBLE: Setting server callbacks...");
    bleServer->setCallbacks(serverCallbacks, true);
    LOG_DEBUG("NimBLE: Server callbacks set");
    
    LOG_DEBUG("NimBLE: Setting up BLE services...");
    setupService();
    LOG_DEBUG("NimBLE: BLE services setup complete");
    
    LOG_DEBUG("NimBLE: Starting BLE advertising...");
    startAdvertising();
    LOG_DEBUG("NimBLE: startAdvertising() call completed");
}

void NimbleBluetooth::setupService()
{
    NimBLEService *bleService = bleServer->createService(MESH_SERVICE_UUID);
    NimBLECharacteristic *ToRadioCharacteristic;
    NimBLECharacteristic *FromRadioCharacteristic;
    // Define the characteristics that the app is looking for
    if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        ToRadioCharacteristic = bleService->createCharacteristic(TORADIO_UUID, NIMBLE_PROPERTY::WRITE);
        FromRadioCharacteristic = bleService->createCharacteristic(FROMRADIO_UUID, NIMBLE_PROPERTY::READ);
        fromNumCharacteristic = bleService->createCharacteristic(FROMNUM_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
        logRadioCharacteristic =
            bleService->createCharacteristic(LOGRADIO_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 512U);
    } else {
        ToRadioCharacteristic = bleService->createCharacteristic(
            TORADIO_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN | NIMBLE_PROPERTY::WRITE_ENC);
        FromRadioCharacteristic = bleService->createCharacteristic(
            FROMRADIO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC);
        fromNumCharacteristic =
            bleService->createCharacteristic(FROMNUM_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                               NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC);
        logRadioCharacteristic = bleService->createCharacteristic(
            LOGRADIO_UUID,
            NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::READ_ENC, 512U);
    }
    bluetoothPhoneAPI = new BluetoothPhoneAPI();

    toRadioCallbacks = new NimbleBluetoothToRadioCallback();
    ToRadioCharacteristic->setCallbacks(toRadioCallbacks);

    fromRadioCallbacks = new NimbleBluetoothFromRadioCallback();
    FromRadioCharacteristic->setCallbacks(fromRadioCallbacks);

    bleService->start();

    // Setup the battery service
    NimBLEService *batteryService = bleServer->createService(NimBLEUUID((uint16_t)0x180f)); // 0x180F is the Battery Service
    BatteryCharacteristic = batteryService->createCharacteristic( // 0x2A19 is the Battery Level characteristic)
        (uint16_t)0x2a19, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, 1);
#ifdef NIMBLE_TWO
    NimBLE2904 *batteryLevelDescriptor = BatteryCharacteristic->create2904();
#else
    NimBLE2904 *batteryLevelDescriptor = (NimBLE2904 *)BatteryCharacteristic->createDescriptor((uint16_t)0x2904);
#endif
    batteryLevelDescriptor->setFormat(NimBLE2904::FORMAT_UINT8);
    batteryLevelDescriptor->setNamespace(1);
    batteryLevelDescriptor->setUnit(0x27ad);

    batteryService->start();

#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // Setup the BitChat service - must be created here before advertising starts
    LOG_INFO("NimBLE: Setting up BitChat service");
    bitchatBLEService = bleServer->createService(NimBLEUUID(BITCHAT_SERVICE_UUID));
    if (bitchatBLEService) {
        bitchatBLECharacteristic = bitchatBLEService->createCharacteristic(
            NimBLEUUID(BITCHAT_CHARACTERISTIC_UUID),
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
        );
        if (bitchatBLECharacteristic) {
            bitchatBLECharacteristic->setValue((uint8_t*)nullptr, 0);
            LOG_INFO("NimBLE: BitChat characteristic created with READ, WRITE, WRITE_NR, NOTIFY");
        } else {
            LOG_ERROR("NimBLE: Failed to create BitChat characteristic");
        }
        bitchatBLEService->start();
        LOG_INFO("NimBLE: BitChat service started");
    } else {
        LOG_ERROR("NimBLE: Failed to create BitChat service");
    }
#endif
}

void NimbleBluetooth::startAdvertising()
{
#ifdef NIMBLE_TWO
    NimBLEExtAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEExtAdvertisement legacyAdvertising;

    legacyAdvertising.setLegacyAdvertising(true);
    legacyAdvertising.setScannable(true);
    legacyAdvertising.setConnectable(true);
    legacyAdvertising.setFlags(BLE_HS_ADV_F_DISC_GEN);
    if (powerStatus->getHasBattery() == 1) {
        legacyAdvertising.setCompleteServices(NimBLEUUID((uint16_t)0x180f));
    }
    legacyAdvertising.setCompleteServices(NimBLEUUID(MESH_SERVICE_UUID));
    legacyAdvertising.setMinInterval(500);
    legacyAdvertising.setMaxInterval(1000);

    NimBLEExtAdvertisement legacyScanResponse;
    legacyScanResponse.setLegacyAdvertising(true);
    legacyScanResponse.setConnectable(true);
    legacyScanResponse.setName(getDeviceName());
    
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    legacyScanResponse.addServiceUUID(NimBLEUUID(BITCHAT_SERVICE_UUID));
    #endif

    if (!pAdvertising->setInstanceData(0, legacyAdvertising)) {
        LOG_ERROR("BLE failed to set legacyAdvertising");
    } else if (!pAdvertising->setScanResponseData(0, legacyScanResponse)) {
        LOG_ERROR("BLE failed to set legacyScanResponse");
    } else if (!pAdvertising->start(0, 0, 0)) {
        LOG_ERROR("BLE failed to start legacyAdvertising");
    }
#else
    LOG_INFO("NimBLE: Starting advertising setup (old API path)");
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    LOG_DEBUG("NimBLE: Got advertising object");
    
    // Note: We always reset and reconfigure advertising to ensure consistency
    // This matches nRF52 behavior where advertising is restarted on disconnect
    LOG_DEBUG("NimBLE: Resetting advertising configuration...");
    pAdvertising->reset();
    LOG_DEBUG("NimBLE: Reset complete, enabling scan response...");
    pAdvertising->setScanResponse(true);
    LOG_DEBUG("NimBLE: Scan response enabled");
    
    // Match nRF52 pattern: Use shortened name in scan response to fit BitChat UUID
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // For BitChat support, we need room in scan response for BitChat UUID (18 bytes)
    // Temporarily use a shortened name to fit: ShortName(~8) + BitChatUUID(18) = ~26 bytes (fits in 31)
    // Truncate prefix to 3 characters and preserve device ID (e.g., "Mes_17b8" from "Meshtastic_17b8")
    const size_t bleShortNameLen = 8;
    const char* fullName = getDeviceName();
    size_t fullLen = strlen(fullName);
    char shortName[bleShortNameLen + 1];

    if (fullLen <= bleShortNameLen) {
        strncpy(shortName, fullName, bleShortNameLen);
        shortName[fullLen] = '\0';
    } else {
        strncpy(shortName, fullName, 3);
        strncpy(shortName + 3, fullName + (fullLen - 5), 5);
        shortName[bleShortNameLen] = '\0';
    }
    LOG_INFO("NimBLE: Using shortened BLE name '%s' (full: '%s') to fit BitChat UUID", shortName, fullName);
    #else
    const char* fullName = getDeviceName();
    LOG_DEBUG("NimBLE: BitChat disabled, using full name '%s'", fullName);
    #endif
    
    // ESP32/NimBLE needs device name in MAIN advertising for discoverability in passive scanners (nRF Connect)
    // Build advertising data explicitly with name + flags + service UUID
    LOG_DEBUG("NimBLE: Creating main advertising data...");
    NimBLEAdvertisementData advertisingData;
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    LOG_DEBUG("NimBLE: Setting short name '%s' in main advertising data", shortName);
    advertisingData.setName(shortName); // Use short name to fit service UUIDs
    LOG_INFO("NimBLE: Using shortened name '%s' in main advertising packet (required for passive scanner discovery)", shortName);
    #else
    LOG_DEBUG("NimBLE: Setting full name '%s' in main advertising data", fullName);
    advertisingData.setName(fullName);
    #endif
    
    LOG_DEBUG("NimBLE: Setting advertising flags (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP)");
    advertisingData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP); // General discoverable
    
    LOG_DEBUG("NimBLE: Adding Meshtastic service UUID to advertising data");
    advertisingData.setCompleteServices(NimBLEUUID(MESH_SERVICE_UUID)); // Meshtastic service (128-bit) - primary
    LOG_DEBUG("NimBLE: Meshtastic service UUID added");
    
    LOG_DEBUG("NimBLE: Applying advertising data to advertising object...");
    pAdvertising->setAdvertisementData(advertisingData);
    LOG_DEBUG("NimBLE: Main advertising data applied successfully");
    // Note: Battery service UUID (0x180f) is omitted from advertising to save space
    // It's still available in GATT after connection - this matches how NIMBLE_TWO path works
    LOG_DEBUG("NimBLE: Set main advertising packet with name, flags, and Meshtastic service UUID");
    
    // Add BitChat service UUID to SCAN RESPONSE if BitChat module is enabled
    // This matches the nRF52 pattern where BitChat UUID goes in scan response
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    LOG_DEBUG("NimBLE: Creating scan response data with BitChat UUID...");
    // Create scan response data with shortened name + BitChat UUID
    // This matches nRF52: scan response has ShortName + BitChat UUID
    // nRF52: Bluefruit.ScanResponse.addName() + Bluefruit.ScanResponse.addUuid(BITCHAT_UUID)
    NimBLEAdvertisementData scanResponse;
    LOG_DEBUG("NimBLE: Setting short name '%s' in scan response", shortName);
    scanResponse.setName(shortName); // Shortened name in scan response (matches nRF52 pattern)
    LOG_DEBUG("NimBLE: Adding BitChat service UUID to scan response");
    scanResponse.setCompleteServices(NimBLEUUID(BITCHAT_SERVICE_UUID));
    LOG_DEBUG("NimBLE: Applying scan response data...");
    pAdvertising->setScanResponseData(scanResponse);
    LOG_INFO("NimBLE: Set scan response with shortened name '%s' and BitChat UUID (matches nRF52 pattern)", shortName);
    #else
    LOG_DEBUG("NimBLE: BitChat disabled, skipping scan response setup");
    #endif
    
    // Restore full device name (for GATT Device Name characteristic 0x2A00)
    // This matches nRF52: Bluefruit.setName(fullName) after advertising is configured
    // The advertising/scan response still uses shortened name, but GATT Device Name will be full name
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // Note: NimBLEDevice::init() already set the full name, so GATT Device Name is correct
    // We don't need to change it - the advertising packet just uses short name to fit UUIDs
    LOG_DEBUG("NimBLE: GATT Device Name already set to full name '%s' via NimBLEDevice::init()", fullName);
    #endif
    
    // Set advertising parameters (matching nRF52 behavior)
    LOG_DEBUG("NimBLE: Setting advertising intervals (min=32*0.625ms=20ms, max=244*0.625ms=152.5ms)");
    pAdvertising->setMinInterval(32); // 32 * 0.625ms = 20ms (fast mode)
    pAdvertising->setMaxInterval(244); // 244 * 0.625ms = 152.5ms (slow mode)
    LOG_DEBUG("NimBLE: Advertising intervals set");
    
    // Try to start advertising (0 = advertise forever, matches nRF52)
    LOG_INFO("NimBLE: Starting BLE advertising (Meshtastic service in main adv, ShortName + BitChat UUID in scan response - matches nRF52)");
    LOG_DEBUG("NimBLE: Calling pAdvertising->start(0) to advertise forever...");
    
    bool started = pAdvertising->start(0);
    LOG_DEBUG("NimBLE: pAdvertising->start(0) returned: %s", started ? "true" : "false");
    
    // Give a small delay to let advertising actually start
    delay(100);
    
    if (started) {
        LOG_INFO("NimBLE: BLE advertising started successfully - device should be discoverable as '%s'", 
                 #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
                 shortName
                 #else
                 fullName
                 #endif
                 );
        LOG_INFO("NimBLE: Main advertising packet contains: name='%s', Meshtastic UUID", 
                 #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
                 shortName
                 #else
                 fullName
                 #endif
                 );
        #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
        LOG_INFO("NimBLE: Scan response packet contains: name='%s', BitChat UUID", shortName);
        #endif
        LOG_DEBUG("NimBLE: Advertising intervals: min=%d*0.625ms=%dms, max=%d*0.625ms=%dms", 
                  32, 32*625/1000, 244, 244*625/1000);
        
        // Verify advertising is actually active (for old API, this may not be available)
        // Note: pAdvertising->start(0) with 0 = advertise forever, but NimBLE may stop when connected
        LOG_INFO("NimBLE: Advertising configured with start(0) = advertise forever (until connected)");
        LOG_INFO("NimBLE: When disconnected, advertising will be restarted via onDisconnect callback");
    } else {
        LOG_ERROR("NimBLE: BLE advertising FAILED to start!");
        LOG_ERROR("NimBLE: This means the device will NOT be discoverable!");
        LOG_ERROR("NimBLE: Check advertising data size limits (31 bytes for main, 31 bytes for scan response)");
        LOG_ERROR("NimBLE: Main adv: name='%s' + flags + Meshtastic UUID (128-bit)", 
                 #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
                 shortName
                 #else
                 fullName
                 #endif
                 );
        #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
        LOG_ERROR("NimBLE: Scan resp: name='%s' + BitChat UUID (128-bit)", shortName);
        #endif
    }
#endif
}

/// Given a level between 0-100, update the BLE attribute
void updateBatteryLevel(uint8_t level)
{
    if ((config.bluetooth.enabled == true) && bleServer && nimbleBluetooth->isConnected()) {
        BatteryCharacteristic->setValue(&level, 1);
#ifdef NIMBLE_TWO
        BatteryCharacteristic->notify(&level, 1, BLE_HS_CONN_HANDLE_NONE);
#else
        BatteryCharacteristic->notify();
#endif
    }
}

void NimbleBluetooth::clearBonds()
{
    LOG_INFO("Clearing bluetooth bonds!");
    NimBLEDevice::deleteAllBonds();
}

void NimbleBluetooth::ensureAdvertising()
{
    // Ensure advertising is active when not connected (matches nRF52's restartOnDisconnect behavior)
    if (isDeInit) {
        return; // BLE is deinitialized, don't try to advertise
    }
    
    if (!bleServer) {
        return; // Server not initialized
    }
    
    // If we're connected, advertising should be stopped (standard BLE behavior)
    // We only ensure advertising when NOT connected
    if (isConnected()) {
        LOG_DEBUG("NimBLE: ensureAdvertising() - device is connected, advertising should be stopped");
        return;
    }
    
    // When not connected, advertising should be active
    // For old API, we can't easily check if advertising is active, so we just restart it
    // This ensures advertising continues even if it stopped for some reason
    LOG_DEBUG("NimBLE: ensureAdvertising() - device not connected, ensuring advertising is active");
    
    // Get advertising object and check if we need to restart
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        LOG_ERROR("NimBLE: ensureAdvertising() - cannot get advertising object!");
        return;
    }
    
        // Restart advertising to ensure it's active
        // Note: startAdvertising() resets and reconfigures advertising, which is safe
        // This ensures advertising continues even if it stopped for any reason
        LOG_DEBUG("NimBLE: ensureAdvertising() - restarting advertising to ensure it's active");
        startAdvertising();
        
        // Log that we've ensured advertising
        LOG_DEBUG("NimBLE: ensureAdvertising() - advertising restart complete");
}

void NimbleBluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    if (!bleServer || !isConnected() || length > 512) {
        return;
    }
#ifdef NIMBLE_TWO
    logRadioCharacteristic->notify(logMessage, length, BLE_HS_CONN_HANDLE_NONE);
#else
    logRadioCharacteristic->notify(logMessage, length, true);
#endif
}

void clearNVS()
{
    NimBLEDevice::deleteAllBonds();
#ifdef ARCH_ESP32
    ESP.restart();
#endif
}
#endif
