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
                LOG_INFO("NimBLE: toRadio write %d bytes, queued (slot %d)", (int)val.length(), bluetoothPhoneAPI->queue_size);
                memcpy(lastToRadio, val.data(), val.length());
                std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
                bluetoothPhoneAPI->nimble_queue.at(bluetoothPhoneAPI->queue_size) = val;
                bluetoothPhoneAPI->queue_size++;
                bluetoothPhoneAPI->setIntervalFromNow(0);
            } else {
                LOG_WARN("NimBLE: toRadio write %d bytes DROPPED - queue full", (int)val.length());
            }
        } else {
            LOG_WARN("NimBLE: toRadio write %d bytes DROPPED - duplicate of previous write", (int)val.length());
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
        LOG_INFO("NimBLE: fromRadio read request");
        bluetoothPhoneAPI->phoneWants = true;
        while (!bluetoothPhoneAPI->hasChecked && tries < 100) {
            bluetoothPhoneAPI->setIntervalFromNow(0);
            delay(20);
            tries++;
        }
        std::lock_guard<std::mutex> guard(bluetoothPhoneAPI->nimble_mutex);
        pCharacteristic->setValue(bluetoothPhoneAPI->fromRadioBytes, bluetoothPhoneAPI->numBytes);
        LOG_INFO("NimBLE: fromRadio read served %d bytes after %d ms (hasChecked=%d)",
                 (int)bluetoothPhoneAPI->numBytes, tries * 20, (int)bluetoothPhoneAPI->hasChecked);

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

#ifndef NIMBLE_TWO
// Passive GAP event listener (registered in setup() via setCustomGapHandler, which uses
// ble_gap_event_listener_register - it observes events in ADDITION to the normal handlers).
// Exists because the 1.4.x server callbacks don't expose connect status / disconnect reason /
// encryption results, which are needed to diagnose phones that connect and immediately drop.
// NimBLE host reasons are HCI code + 0x200: 0x213=remote user terminated, 0x208=supervision
// timeout, 0x23d=MIC failure (stale bond), 0x205=authentication failure.
static int gapEventLogger(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        struct ble_gap_conn_desc desc;
        if (event->connect.status == 0 && ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
            LOG_INFO("NimBLE GAP: connect status=0 peer=%02x:%02x:%02x:%02x:%02x:%02x addrtype=%d",
                     desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4], desc.peer_ota_addr.val[3],
                     desc.peer_ota_addr.val[2], desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0],
                     desc.peer_ota_addr.type);
        } else {
            LOG_INFO("NimBLE GAP: connect status=%d", event->connect.status);
        }
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        LOG_INFO("NimBLE GAP: disconnect reason=0x%03x (%d)", event->disconnect.reason, event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        LOG_INFO("NimBLE GAP: encryption change status=%d", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        LOG_INFO("NimBLE GAP: repeat pairing request (phone forgot bond but device still holds one)");
        break;
    case BLE_GAP_EVENT_MTU:
        LOG_INFO("NimBLE GAP: MTU update value=%d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        LOG_INFO("NimBLE GAP: subscribe attr_handle=%d", event->subscribe.attr_handle);
        break;
    default:
        LOG_DEBUG("NimBLE GAP: event type=%d", event->type);
        break;
    }
    return 0;
}
#endif

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

#ifndef NIMBLE_TWO
    NimBLEDevice::setCustomGapHandler(gapEventLogger); // passive listener: log GAP connect/disconnect/enc events
#endif
    
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

#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    bitChatAdvertisingDeferred = true;
    LOG_INFO("NimBLE: Deferring advertising until BitChat service is ready");
#endif

    if (bitChatAdvertisingDeferred) {
        LOG_INFO("NimBLE: Advertising deferred - waiting for BitChat service setup");
        return;
    }

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
    LOG_INFO("NimBLE: mesh service handles - toRadio=%d fromRadio=%d fromNum=%d logRadio=%d",
             ToRadioCharacteristic->getHandle(), FromRadioCharacteristic->getHandle(),
             fromNumCharacteristic->getHandle(), logRadioCharacteristic->getHandle());

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
}

#ifndef NIMBLE_TWO
// Build the legacy (31-byte) advertising and scan-response payloads for the current
// alternation state. Shared by startAdvertising() (full reconfigure) and
// swapBitChatAdvertising() (in-place data update while advertising stays running).
void NimbleBluetooth::buildLegacyAdvertisingData(NimBLEAdvertisementData &advertisingData, NimBLEAdvertisementData &scanResponse)
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // For BitChat support, we need room in scan response for BitChat UUID (18 bytes)
    // Use a shortened name to fit: ShortName(~8) + UUID(18) = ~26 bytes (fits in 31)
    // Truncate prefix to 3 characters and preserve device ID (e.g., "Mes_17b8" from "Meshtastic_17b8")
    const size_t bleShortNameLen = 8;
    const char *fullName = getDeviceName();
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

    advertisingData.setName(shortName);
    advertisingData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);

    // Main advertising packet gets ONE 128-bit service UUID (two won't fit in 31 bytes).
    // We alternate it (see swapBitChatAdvertising) because iOS filters scans on this packet only.
    // Never advertise the BitChat UUID before its GATT service exists - fall back to
    // Meshtastic-only so phones aren't pointed at a nonexistent service.
    bool advBitChatMain = bitChatServiceReady && bitChatAdvMainIsBitChat;
    advertisingData.setCompleteServices(NimBLEUUID(advBitChatMain ? BITCHAT_SERVICE_UUID : MESH_SERVICE_UUID));

    // The OTHER 128-bit UUID goes in the scan response. iOS ignores scan-response UUIDs when
    // filtering, but Android reads them, and the app whose UUID is in the main packet this
    // cycle covers iOS. While the BitChat service isn't up, keep the scan response Meshtastic-only.
    scanResponse.setName(shortName);
    const char *scanRespUUID = bitChatServiceReady
                                   ? (bitChatAdvMainIsBitChat ? MESH_SERVICE_UUID : BITCHAT_SERVICE_UUID)
                                   : MESH_SERVICE_UUID;
    scanResponse.setCompleteServices(NimBLEUUID(scanRespUUID));
    LOG_DEBUG("NimBLE: Adv payloads built - main=%s UUID, scanrsp=%s UUID, name='%s'",
              advBitChatMain ? "BitChat" : "Meshtastic",
              (bitChatServiceReady && !bitChatAdvMainIsBitChat) ? "BitChat" : "Meshtastic", shortName);
#else
    advertisingData.setName(getDeviceName());
    advertisingData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advertisingData.setCompleteServices(NimBLEUUID(MESH_SERVICE_UUID));
    (void)scanResponse;
#endif
}
#endif

void NimbleBluetooth::startAdvertising()
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    if (bitChatAdvertisingDeferred) {
        LOG_INFO("NimBLE: Advertising deferred until BitChat service is ready");
        return;
    }
#endif
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
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // Alternate which 128-bit UUID is in the main packet so both apps can discover us on iOS.
    // But never advertise the BitChat UUID before its GATT service exists (setup may have
    // failed / not run yet) - fall back to Meshtastic-only so we don't point phones at a
    // service that isn't there.
    legacyAdvertising.setCompleteServices(NimBLEUUID(
        (bitChatServiceReady && bitChatAdvMainIsBitChat) ? BITCHAT_SERVICE_UUID : MESH_SERVICE_UUID));
    #else
    legacyAdvertising.setCompleteServices(NimBLEUUID(MESH_SERVICE_UUID));
    #endif
    legacyAdvertising.setMinInterval(500);
    legacyAdvertising.setMaxInterval(1000);

    NimBLEExtAdvertisement legacyScanResponse;
    legacyScanResponse.setLegacyAdvertising(true);
    legacyScanResponse.setConnectable(true);
    legacyScanResponse.setName(getDeviceName());

    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // The other 128-bit UUID goes in the scan response (Android reads it there; iOS ignores it).
    // Only advertise the BitChat UUID once its GATT service is up; otherwise keep the scan
    // response Meshtastic-only.
    if (bitChatServiceReady) {
        legacyScanResponse.addServiceUUID(NimBLEUUID(bitChatAdvMainIsBitChat ? MESH_SERVICE_UUID : BITCHAT_SERVICE_UUID));
    } else {
        legacyScanResponse.addServiceUUID(NimBLEUUID(MESH_SERVICE_UUID));
    }
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

    // Note: We always reset and reconfigure advertising to ensure consistency
    // This matches nRF52 behavior where advertising is restarted on disconnect
    pAdvertising->reset();
    pAdvertising->setScanResponse(true);

    // ESP32/NimBLE needs device name in MAIN advertising for discoverability in passive scanners (nRF Connect)
    // Build advertising data explicitly with name + flags + service UUID. The payloads
    // (short name to fit a 128-bit UUID, alternating UUID placement) are built by the shared
    // helper so swapBitChatAdvertising() produces identical packets when updating in place.
    NimBLEAdvertisementData advertisingData;
    NimBLEAdvertisementData scanResponse;
    buildLegacyAdvertisingData(advertisingData, scanResponse);
    pAdvertising->setAdvertisementData(advertisingData);
    // Note: Battery service UUID (0x180f) is omitted from advertising to save space
    // It's still available in GATT after connection - this matches how NIMBLE_TWO path works
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    pAdvertising->setScanResponseData(scanResponse);
    #endif
    // GATT Device Name (0x2A00) keeps the full name set by NimBLEDevice::init();
    // only the advertising packets use the shortened name.

    // Set advertising parameters (matching nRF52 behavior)
    LOG_DEBUG("NimBLE: Setting advertising intervals (min=32*0.625ms=20ms, max=244*0.625ms=152.5ms)");
    pAdvertising->setMinInterval(32); // 32 * 0.625ms = 20ms (fast mode)
    pAdvertising->setMaxInterval(244); // 244 * 0.625ms = 152.5ms (slow mode)
    LOG_DEBUG("NimBLE: Advertising intervals set");
    
    // Try to start advertising (0 = advertise forever, matches nRF52)
    LOG_INFO("NimBLE: Starting BLE advertising (alternating UUID placement - see buildLegacyAdvertisingData)");

    bool started = pAdvertising->start(0);
    LOG_DEBUG("NimBLE: pAdvertising->start(0) returned: %s", started ? "true" : "false");
    
    // Give a small delay to let advertising actually start
    delay(100);
    
    if (started) {
        LOG_INFO("NimBLE: BLE advertising started successfully");
        // Note: pAdvertising->start(0) with 0 = advertise forever, but NimBLE stops when connected;
        // advertising is restarted via the onDisconnect callback.
    } else {
        LOG_ERROR("NimBLE: BLE advertising FAILED to start!");
        LOG_ERROR("NimBLE: This means the device will NOT be discoverable!");
        LOG_ERROR("NimBLE: Check advertising data size limits (31 bytes for main, 31 bytes for scan response)");
    }
#endif
}

void NimbleBluetooth::setBitChatServiceReady()
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    bitChatServiceReady = true; // GATT service now exists -> safe to advertise its UUID
    if (bitChatAdvertisingDeferred) {
        bitChatAdvertisingDeferred = false;
        LOG_INFO("NimBLE: BitChat service ready, advertising can start");
    }
#endif
}

// Bring up plain Meshtastic advertising when the BitChat GATT service could NOT be set up.
// Clears the boot-time deferral so the device stays discoverable as a Meshtastic node, but
// leaves bitChatServiceReady false so startAdvertising() omits the BitChat UUID entirely -
// we must never advertise a service that doesn't exist in the GATT table.
void NimbleBluetooth::startBaseAdvertising()
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    bitChatAdvertisingDeferred = false;
    startAdvertising();
#endif
}

// Flip which 128-bit service UUID sits in the main advertising packet.
// Called on a timer (~ADV_ALTERNATE_INTERVAL_MS) from BitChatBridgeModule::runOnce().
// On the old API this updates the advertising/scan-response data IN PLACE while advertising
// keeps running (HCI LE Set Advertising Data is legal while enabled). Advertising must never
// be stopped/reset here: isConnected() cannot see a phone whose connection is still being
// established, and a timer-driven teardown in that window kills the attempt (Android GATT 133).
void NimbleBluetooth::swapBitChatAdvertising()
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    if (isDeInit || !bleServer) {
        return; // BLE not up
    }
    if (bitChatAdvertisingDeferred) {
        return; // BitChat service not ready yet - not advertising the BitChat UUID at all
    }
    if (isConnected()) {
        return; // Connected: NimBLE isn't advertising; don't disturb the link
    }
    bitChatAdvMainIsBitChat = !bitChatAdvMainIsBitChat;
    LOG_DEBUG("NimBLE: Alternating advertising - main packet now carries %s UUID",
              bitChatAdvMainIsBitChat ? "BitChat" : "Meshtastic");
#ifdef NIMBLE_TWO
    startAdvertising(); // ext-adv API (esp32c6 only): full reconfigure, as before
#else
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        return;
    }
    // Set the swapped payloads unconditionally - the HCI Set Advertising/Scan Response Data
    // commands are legal whether or not advertising is currently enabled, and this keeps the
    // on-air data in sync with the flag even if ensureAdvertising() later resumes advertising.
    // NEVER start/stop/reset advertising here: !isAdvertising() does not mean idle - it is
    // also the window where a CONNECT_IND just arrived and the host hasn't processed the
    // connect event yet. Resuming advertising is ensureAdvertising()/onDisconnect's job.
    NimBLEAdvertisementData advertisingData;
    NimBLEAdvertisementData scanResponse;
    buildLegacyAdvertisingData(advertisingData, scanResponse);
    pAdvertising->setAdvertisementData(advertisingData);
    pAdvertising->setScanResponseData(scanResponse);
#endif
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
    // Safety net: make sure advertising is running when not connected. This runs
    // periodically, so it MUST be cheap and idempotent. Crucially it must NOT reset or
    // reconfigure advertising when it's already active - doing that on a timer can tear
    // down an in-progress connection attempt from a phone (the root cause of flaky
    // ESP32 connections). Full (re)configuration only happens in startAdvertising(),
    // which is called on setup and on disconnect.
    if (isDeInit) {
        return; // BLE is deinitialized, don't try to advertise
    }
    if (!bleServer) {
        return; // Server not initialized
    }
    if (isConnected()) {
        // Connected -> advertising is expected to be stopped by the stack; leave it alone.
        return;
    }

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    if (!pAdvertising) {
        LOG_ERROR("NimBLE: ensureAdvertising() - cannot get advertising object!");
        return;
    }

    if (pAdvertising->isAdvertising()) {
        return; // Already advertising - nothing to do (no reset storm)
    }

    // Advertising is off while disconnected: resume it WITHOUT reconfiguring. The
    // advertising/scan-response data was already set by the last startAdvertising().
    LOG_DEBUG("NimBLE: ensureAdvertising() - advertising was off, resuming (lightweight start)");
    if (!pAdvertising->start(0)) {
        // Bare resume failed (e.g. never configured yet) - fall back to a full setup.
        LOG_WARN("NimBLE: ensureAdvertising() - bare start failed, doing full reconfigure");
        startAdvertising();
    }
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
