#include "NRF52Bluetooth.h"
#include "BLEDfuSecure.h"
#include "BluetoothCommon.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"
#include <bluefruit.h>
#include <utility/bonding.h>
static BLEService meshBleService = BLEService(BLEUuid(MESH_SERVICE_UUID_16));
static BLECharacteristic fromNum = BLECharacteristic(BLEUuid(FROMNUM_UUID_16));
static BLECharacteristic fromRadio = BLECharacteristic(BLEUuid(FROMRADIO_UUID_16));
static BLECharacteristic toRadio = BLECharacteristic(BLEUuid(TORADIO_UUID_16));
static BLECharacteristic logRadio = BLECharacteristic(BLEUuid(LOGRADIO_UUID_16));
static BLEDis bledis; // DIS (Device Information Service) helper class instance
static BLEBas blebas; // BAS (Battery Service) helper class instance
#ifndef BLE_DFU_SECURE
static BLEDfu bledfu; // DFU software update helper service
#else
static BLEDfuSecure bledfusecure;                                             // DFU software update helper service
#endif

// This scratch buffer is used for various bluetooth reads/writes - but it is safe because only one bt operation can be in
// process at once
// static uint8_t trBytes[_max(_max(_max(_max(ToRadio_size, RadioConfig_size), User_size), MyNodeInfo_size), FromRadio_size)];
static uint8_t fromRadioBytes[meshtastic_FromRadio_size];
static uint8_t toRadioBytes[meshtastic_ToRadio_size];

static uint16_t connectionHandle;

#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
// Alternating-UUID advertising toggle (see startAdv / NRF52Bluetooth::swapBitChatAdvertising).
// A 31-byte legacy advertisement holds only one 128-bit UUID; iOS filters scans on the main
// packet only, so we periodically swap which UUID lives there.
// false: Meshtastic UUID in main packet, BitChat UUID in scan response.
// true:  BitChat UUID in main packet, Meshtastic UUID in scan response.
static bool bitchatAdvMainIsBitChat = false;
#endif

class BluetoothPhoneAPI : public PhoneAPI
{
    /**
     * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
     */
    virtual void onNowHasData(uint32_t fromRadioNum) override
    {
        PhoneAPI::onNowHasData(fromRadioNum);

        LOG_INFO("BLE notify fromNum");
        fromNum.notify32(fromRadioNum);
    }

    /// Check the current underlying physical link to see if the client is currently connected
    virtual bool checkIsConnected() override { return Bluefruit.connected(connectionHandle); }
};

static BluetoothPhoneAPI *bluetoothPhoneAPI;

void onConnect(uint16_t conn_handle)
{
    // Get the reference to current connection
    BLEConnection *connection = Bluefruit.Connection(conn_handle);
    connectionHandle = conn_handle;
    char central_name[32] = {0};
    connection->getPeerName(central_name, sizeof(central_name));
    LOG_INFO("BLE Connected to %s", central_name);

    // Notify UI (or any other interested firmware components)
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&newStatus);
}
/**
 * Callback invoked when a connection is dropped
 * @param conn_handle connection where this event happens
 * @param reason is a BLE_HCI_STATUS_CODE which can be found in ble_hci.h
 */
void onDisconnect(uint16_t conn_handle, uint8_t reason)
{
    LOG_INFO("BLE Disconnected, reason = 0x%x", reason);
    if (bluetoothPhoneAPI) {
        bluetoothPhoneAPI->close();
    }

    // Notify UI (or any other interested firmware components)
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&newStatus);
}
void onCccd(uint16_t conn_hdl, BLECharacteristic *chr, uint16_t cccd_value)
{
    // Display the raw request packet
    LOG_INFO("CCCD Updated: %u", cccd_value);
    // Check the characteristic this CCCD update is associated with in case
    // this handler is used for multiple CCCD records.

    // According to the GATT spec: cccd value = 0x0001 means notifications are enabled
    // and cccd value = 0x0002 means indications are enabled

    if (chr->uuid == fromNum.uuid || chr->uuid == logRadio.uuid) {
        auto result = cccd_value == 2 ? chr->indicateEnabled(conn_hdl) : chr->notifyEnabled(conn_hdl);
        if (result) {
            LOG_INFO("Notify/Indicate enabled");
        } else {
            LOG_INFO("Notify/Indicate disabled");
        }
    }
}
void startAdv(void)
{
    // Rebuild advertising from scratch on every call so alternating (swapBitChatAdvertising)
    // can re-run this without accumulating stale AD structures or overflowing the packet.
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    // Advertising packet
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    // IncludeService UUID
    // Bluefruit.ScanResponse.addService(meshBleService);
    Bluefruit.ScanResponse.addTxPower();
    
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // For BitChat support, we need room in scan response for BitChat UUID (18 bytes)
    // Temporarily use a shortened name to fit: TxPower(3) + ShortName(~8) + BitChatUUID(18) = ~29 bytes (fits in 31)
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
    
    // Temporarily set short name for advertising
    Bluefruit.setName(shortName);
    LOG_INFO("Using shortened BLE name '%s' (full: '%s') to fit BitChat UUID", shortName, fullName);
    #endif
    
    Bluefruit.ScanResponse.addName();
    // Include Name
    // Bluefruit.Advertising.addName();
    
    #if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    // Only ONE 128-bit UUID fits in the 31-byte main packet, so alternate which one is there.
    // iOS filters scans on the main packet only; the other UUID goes in the scan response
    // (Android reads it there). Over a couple of cycles both the Meshtastic app and the
    // BitChat apps get a window where their UUID is in the main packet -> discoverable on iOS.
    // Scan-response budget is tight: TxPower(3) + shortName + one 128-bit UUID(18) is at the
    // 31-byte edge, so check BOTH additions - a silent overflow drops a UUID and makes one app
    // undiscoverable (Android reads scan-response UUIDs; iOS relies on the main packet).
    bool bitchatAdded;
    bool meshAdded;
    if (bitchatAdvMainIsBitChat) {
        // This cycle: BitChat UUID in main packet -> iOS BitChat app can discover us.
        bitchatAdded = Bluefruit.Advertising.addUuid(BLEUuid(BITCHAT_SERVICE_UUID_16));
        meshAdded = Bluefruit.ScanResponse.addService(meshBleService); // Meshtastic UUID in scan response
        LOG_INFO("nRF52 adv: main=BitChat, scanResp=Meshtastic (bitchatAdded=%d, meshAdded=%d)", bitchatAdded, meshAdded);
    } else {
        // This cycle: Meshtastic UUID in main packet -> Meshtastic app can discover us.
        meshAdded = Bluefruit.Advertising.addService(meshBleService);
        bitchatAdded = Bluefruit.ScanResponse.addUuid(BLEUuid(BITCHAT_SERVICE_UUID_16)); // BitChat UUID in scan response
        LOG_INFO("nRF52 adv: main=Meshtastic, scanResp=BitChat (bitchatAdded=%d, meshAdded=%d)", bitchatAdded, meshAdded);
    }
    if (!bitchatAdded) {
        LOG_WARN("FAILED: Could not add BitChat UUID to advertising even with short name");
    }
    if (!meshAdded) {
        LOG_WARN("FAILED: Could not add Meshtastic UUID to advertising (scan response overflow?)");
    }
    // Restore full name (GATT device name 0x2A00) after advertising is configured
    Bluefruit.setName(fullName);
    #else
    // Advertise Meshtastic service (primary)
    Bluefruit.Advertising.addService(meshBleService);
    LOG_INFO("BitChat module is excluded from build");
    #endif
    
    /* Start Advertising
     * - Enable auto advertising if disconnected
     * - Interval:  fast mode = 20 ms, slow mode = 152.5 ms
     * - Timeout for fast mode is 30 seconds
     * - Start(timeout) with timeout = 0 will advertise forever (until connected)
     *
     * For recommended advertising interval
     * https://developer.apple.com/library/content/qa/qa1931/_index.html
     */
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
    Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
    Bluefruit.Advertising.start(0); // 0 = Don't stop advertising after n seconds.  FIXME, we should stop advertising after X
}
// Just ack that the caller is allowed to read
static void authorizeRead(uint16_t conn_hdl)
{
    ble_gatts_rw_authorize_reply_params_t reply = {.type = BLE_GATTS_AUTHORIZE_TYPE_READ};
    reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply);
}
/**
 * client is starting read, pull the bytes from our API class
 */
void onFromRadioAuthorize(uint16_t conn_hdl, BLECharacteristic *chr, ble_gatts_evt_read_t *request)
{
    if (request->offset == 0) {
        // If the read is long, we will get multiple authorize invocations - we only populate data on the first
        size_t numBytes = bluetoothPhoneAPI->getFromRadio(fromRadioBytes);
        // Someone is going to read our value as soon as this callback returns.  So fill it with the next message in the queue
        // or make empty if the queue is empty
        fromRadio.write(fromRadioBytes, numBytes);
    } else {
        // LOG_INFO("Ignore successor read");
    }
    authorizeRead(conn_hdl);
}
// Last ToRadio value received from the phone
static uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE];

void onToRadioWrite(uint16_t conn_hdl, BLECharacteristic *chr, uint8_t *data, uint16_t len)
{
    LOG_INFO("toRadioWriteCb data %p, len %u", data, len);
    if (memcmp(lastToRadio, data, len) != 0) {
        LOG_DEBUG("New ToRadio packet");
        memcpy(lastToRadio, data, len);
        bluetoothPhoneAPI->handleToRadio(data, len);
    } else {
        LOG_DEBUG("Drop dup ToRadio packet we just saw");
    }
}

void setupMeshService(void)
{
    bluetoothPhoneAPI = new BluetoothPhoneAPI();
    meshBleService.begin();
    // Note: You must call .begin() on the BLEService before calling .begin() on
    // any characteristic(s) within that service definition.. Calling .begin() on
    // a BLECharacteristic will cause it to be added to the last BLEService that
    // was 'begin()'ed!
    auto secMode =
        config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN ? SECMODE_OPEN : SECMODE_ENC_NO_MITM;
    fromNum.setProperties(CHR_PROPS_NOTIFY | CHR_PROPS_READ);
    fromNum.setPermission(secMode, SECMODE_NO_ACCESS); // FIXME, secure this!!!
    fromNum.setFixedLen(
        0); // Variable len (either 0 or 4)  FIXME consider changing protocol so it is fixed 4 byte len, where 0 means empty
    fromNum.setMaxLen(4);
    fromNum.setCccdWriteCallback(onCccd); // Optionally capture CCCD updates
    // We don't yet need to hook the fromNum auth callback
    // fromNum.setReadAuthorizeCallback(fromNumAuthorizeCb);
    fromNum.write32(0); // Provide default fromNum of 0
    fromNum.begin();

    fromRadio.setProperties(CHR_PROPS_READ);
    fromRadio.setPermission(secMode, SECMODE_NO_ACCESS);
    fromRadio.setMaxLen(sizeof(fromRadioBytes));
    fromRadio.setReadAuthorizeCallback(
        onFromRadioAuthorize,
        false); // We don't call this callback via the adafruit queue, because we can safely run in the BLE context
    fromRadio.setBuffer(fromRadioBytes, sizeof(fromRadioBytes)); // we preallocate our fromradio buffer so we won't waste space
    // for two copies
    fromRadio.begin();

    toRadio.setProperties(CHR_PROPS_WRITE);
    toRadio.setPermission(secMode, secMode); // FIXME secure this!
    toRadio.setFixedLen(0);
    toRadio.setMaxLen(512);
    toRadio.setBuffer(toRadioBytes, sizeof(toRadioBytes));
    // We don't call this callback via the adafruit queue, because we can safely run in the BLE context
    toRadio.setWriteCallback(onToRadioWrite, false);
    toRadio.begin();

    logRadio.setProperties(CHR_PROPS_INDICATE | CHR_PROPS_NOTIFY | CHR_PROPS_READ);
    logRadio.setPermission(secMode, SECMODE_NO_ACCESS);
    logRadio.setMaxLen(512);
    logRadio.setCccdWriteCallback(onCccd);
    logRadio.write32(0);
    logRadio.begin();
}
static uint32_t configuredPasskey;
void NRF52Bluetooth::shutdown()
{
    // Shutdown bluetooth for minimum power draw
    LOG_INFO("Disable NRF52 bluetooth");
    Bluefruit.Security.setPairPasskeyCallback(NRF52Bluetooth::onUnwantedPairing); // Actively refuse (during factory reset)
    disconnect();
    Bluefruit.Advertising.stop();
}
void NRF52Bluetooth::startDisabled()
{
    // Setup Bluetooth
    nrf52Bluetooth->setup();
    // Shutdown bluetooth for minimum power draw
    Bluefruit.Advertising.stop();
    Bluefruit.setTxPower(-40); // Minimum power
    LOG_INFO("Disable NRF52 Bluetooth. (Workaround: tx power min, advertise stopped)");
}
bool NRF52Bluetooth::isConnected()
{
    return Bluefruit.connected(connectionHandle);
}
int NRF52Bluetooth::getRssi()
{
    return 0; // FIXME figure out where to source this
}

bool NRF52Bluetooth::isCentralRoleSupported()
{
    // nRF52 supports both central and peripheral roles
    return true;
}

void NRF52Bluetooth::setup()
{
    // Initialise the Bluefruit module
    LOG_INFO("Init the Bluefruit nRF52 module");
    Bluefruit.autoConnLed(false);
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    // Clear existing data.
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();
    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        configuredPasskey = config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN
                                ? config.bluetooth.fixed_pin
                                : random(100000, 999999);
        auto pinString = std::to_string(configuredPasskey);
        LOG_INFO("Bluetooth pin set to '%i'", configuredPasskey);
        Bluefruit.Security.setPIN(pinString.c_str());
        Bluefruit.Security.setIOCaps(true, false, false);
        Bluefruit.Security.setPairPasskeyCallback(NRF52Bluetooth::onPairingPasskey);
        Bluefruit.Security.setPairCompleteCallback(NRF52Bluetooth::onPairingCompleted);
        Bluefruit.Security.setSecuredCallback(NRF52Bluetooth::onConnectionSecured);
        meshBleService.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
    } else {
        Bluefruit.Security.setIOCaps(false, false, false);
        meshBleService.setPermission(SECMODE_OPEN, SECMODE_OPEN);
    }
    // Set the advertised device name (keep it short!)
    Bluefruit.setName(getDeviceName());
    // Set the connect/disconnect callback handlers
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);
#ifndef BLE_DFU_SECURE
    bledfu.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
    bledfu.begin(); // Install the DFU helper
#else
    bledfusecure.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM); // add by WayenWeng
    bledfusecure.begin();                                                     // Install the DFU helper
#endif
    // Configure and Start the Device Information Service
    LOG_INFO("Init the Device Information Service");
    bledis.setModel(optstr(HW_VERSION));
    bledis.setFirmwareRev(optstr(APP_VERSION));
    bledis.begin();
    // Start the BLE Battery Service and set it to 100%
    LOG_INFO("Init the Battery Service");
    blebas.begin();
    blebas.write(0); // Unknown battery level for now
    // Setup the Heart Rate Monitor service using
    // BLEService and BLECharacteristic classes
    LOG_INFO("Init the Mesh bluetooth service");
    setupMeshService();
    // Setup the advertising packet(s)
    LOG_INFO("Set up the advertising payload(s)");
    startAdv();
    LOG_INFO("Advertise");
}
void NRF52Bluetooth::resumeAdvertising()
{
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
    Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
    Bluefruit.Advertising.start(0);
}

// Flip which 128-bit service UUID sits in the main advertising packet and re-advertise.
// Called on a timer (~ADV_ALTERNATE_INTERVAL_MS) from BitChatBridgeModule::runOnce().
// Only runs while DISCONNECTED so an established link is never disturbed; startAdv() rebuilds
// the advertising/scan-response data from scratch, and resumeAdvertising() restarts it.
void NRF52Bluetooth::swapBitChatAdvertising()
{
#if !MESHTASTIC_EXCLUDE_BITCHAT_BRIDGE
    if (isConnected()) {
        return; // Connected: leave advertising as-is; don't disturb the link
    }
    bitchatAdvMainIsBitChat = !bitchatAdvMainIsBitChat;
    LOG_DEBUG("nRF52: Alternating advertising - main packet now carries %s UUID",
              bitchatAdvMainIsBitChat ? "BitChat" : "Meshtastic");
    startAdv(); // self-contained: stops, rebuilds adv + scan response, and restarts advertising
#endif
}
/// Given a level between 0-100, update the BLE attribute
void updateBatteryLevel(uint8_t level)
{
    blebas.write(level);
}
void NRF52Bluetooth::clearBonds()
{
    LOG_INFO("Clear bluetooth bonds!");
    bond_print_list(BLE_GAP_ROLE_PERIPH);
    bond_print_list(BLE_GAP_ROLE_CENTRAL);
    Bluefruit.Periph.clearBonds();
    Bluefruit.Central.clearBonds();
}
void NRF52Bluetooth::onConnectionSecured(uint16_t conn_handle)
{
    LOG_INFO("BLE connection secured");
}
bool NRF52Bluetooth::onPairingPasskey(uint16_t conn_handle, uint8_t const passkey[6], bool match_request)
{
    char passkey1[4] = {passkey[0], passkey[1], passkey[2], '\0'};
    char passkey2[4] = {passkey[3], passkey[4], passkey[5], '\0'};
    LOG_INFO("BLE pair process started with passkey %s %s", passkey1, passkey2);
    powerFSM.trigger(EVENT_BLUETOOTH_PAIR);

    // Get passkey as string
    // Note: possible leading zeros
    std::string textkey;
    for (uint8_t i = 0; i < 6; i++)
        textkey += (char)passkey[i];

    // Notify UI (or other components) of pairing event and passkey
    meshtastic::BluetoothStatus newStatus(textkey);
    bluetoothStatus->updateStatus(&newStatus);

#if !defined(MESHTASTIC_EXCLUDE_SCREEN) // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
    if (screen) {
        screen->startAlert([](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
            char btPIN[16] = "888888";
            snprintf(btPIN, sizeof(btPIN), "%06u", configuredPasskey);
            int x_offset = display->width() / 2;
            int y_offset = display->height() <= 80 ? 0 : 12;
            display->setTextAlignment(TEXT_ALIGN_CENTER);
            display->setFont(FONT_MEDIUM);
            display->drawString(x_offset + x, y_offset + y, "Bluetooth");

            display->setFont(FONT_SMALL);
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
            display->drawString(x_offset + x, y_offset + y, "Enter this code");

            display->setFont(FONT_LARGE);
            String displayPin(btPIN);
            String pin = displayPin.substring(0, 3) + " " + displayPin.substring(3, 6);
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
            display->drawString(x_offset + x, y_offset + y, pin);

            display->setFont(FONT_SMALL);
            String deviceName = "Name: ";
            deviceName.concat(getDeviceName());
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
            display->drawString(x_offset + x, y_offset + y, deviceName);
        });
    }
#endif
    if (match_request) {
        uint32_t start_time = millis();
        while (millis() < start_time + 30000) {
            if (!Bluefruit.connected(conn_handle))
                break;
        }
    }
    LOG_INFO("BLE passkey pair: match_request=%i", match_request);
    return true;
}

// Actively refuse new BLE pairings
// After clearing bonds (at factory reset), clients seem initially able to attempt to re-pair, even with advertising disabled.
// On NRF52Bluetooth::shutdown, we change the pairing callback to this method, to aggressively refuse any connection attempts.
bool NRF52Bluetooth::onUnwantedPairing(uint16_t conn_handle, uint8_t const passkey[6], bool match_request)
{
    NRF52Bluetooth::disconnect();
    return false;
}

// Disconnect any BLE connections
void NRF52Bluetooth::disconnect()
{
    uint8_t connection_num = Bluefruit.connected();
    if (connection_num) {
        // Close all connections. We're only expecting one.
        for (uint8_t i = 0; i < connection_num; i++)
            Bluefruit.disconnect(i);

        // Wait for disconnection
        while (Bluefruit.connected())
            yield();

        LOG_INFO("Ended BLE connection");
    }
}

void NRF52Bluetooth::onPairingCompleted(uint16_t conn_handle, uint8_t auth_status)
{
    if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        LOG_INFO("BLE pair success");
        meshtastic::BluetoothStatus newConnectedStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&newConnectedStatus);
    } else {
        LOG_INFO("BLE pair failed");
        // Notify UI (or any other interested firmware components)
        meshtastic::BluetoothStatus newDisconnectedStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        bluetoothStatus->updateStatus(&newDisconnectedStatus);
    }

    // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
    if (screen) {
        screen->endAlert();
    }
}

void NRF52Bluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    if (!isConnected() || length > 512)
        return;
    if (logRadio.indicateEnabled())
        logRadio.indicate(logMessage, (uint16_t)length);
    else
        logRadio.notify(logMessage, (uint16_t)length);
}
