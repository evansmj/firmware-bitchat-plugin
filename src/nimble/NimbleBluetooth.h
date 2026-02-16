#pragma once
#include "BluetoothCommon.h"

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
    bool isDeInit = false;

  private:
    void setupService();
    bool bitChatAdvertisingDeferred = false;
};

void setBluetoothEnable(bool enable);
void clearNVS();