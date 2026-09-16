#pragma once
#include "BluetoothCommon.h"

#ifndef NIMBLE_TWO
class NimBLEAdvertisementData;
#endif

class NimbleBluetooth : BluetoothApi
{
  public:
    void setup();
    void shutdown();
    void deinit();
    void clearBonds();
    bool isActive();
    bool isConnected();
    int getRssi();
    void sendLog(const uint8_t *logMessage, size_t length);
    void startAdvertising(); // Public so callback can restart advertising on disconnect
    void ensureAdvertising(); // Ensure advertising is active when not connected (called periodically)
    void setBitChatServiceReady();
    void startBaseAdvertising(); // Advertise Meshtastic-only (BitChat GATT service not up yet)
    void swapBitChatAdvertising(); // Alternate which service UUID sits in the main advertising packet
    bool isDeInit = false;

  private:
    void setupService();
#ifndef NIMBLE_TWO
    // Build the legacy 31-byte advertising + scan-response payloads for the current
    // alternation state. Shared by startAdvertising() and the in-place update in
    // swapBitChatAdvertising().
    void buildLegacyAdvertisingData(NimBLEAdvertisementData &advertisingData, NimBLEAdvertisementData &scanResponse);
#endif
    bool bitChatAdvertisingDeferred = false;
    // Whether the BitChat GATT service has actually been created. Until it has, advertising
    // must NOT include the BitChat UUID - otherwise phones try to connect to a service that
    // doesn't exist. Set true by setBitChatServiceReady().
    bool bitChatServiceReady = false;
    // false: Meshtastic UUID in main packet / BitChat UUID in scan response.
    // true:  BitChat UUID in main packet / Meshtastic UUID in scan response.
    bool bitChatAdvMainIsBitChat = false;
};

void setBluetoothEnable(bool enable);
void clearNVS();