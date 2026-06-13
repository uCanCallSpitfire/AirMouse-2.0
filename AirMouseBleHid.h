#pragma once

#include <Arduino.h>
#include <NimBLECharacteristic.h>
#include <NimBLEHIDDevice.h>

class AirMouseBleHid {
public:
  static constexpr uint8_t BUTTON_LEFT = 0x01;
  static constexpr uint8_t BUTTON_RIGHT = 0x02;
  static constexpr uint8_t BUTTON_MIDDLE = 0x04;
  static constexpr uint8_t BUTTON_BACK = 0x08;
  static constexpr uint8_t BUTTON_FORWARD = 0x10;

  AirMouseBleHid(const char *deviceName, const char *manufacturer, uint8_t batteryLevel = 100);

  void begin();
  bool isConnected() const;
  void move(int8_t x, int8_t y, int8_t wheel = 0, int8_t horizontalWheel = 0);
  void press(uint8_t buttons = BUTTON_LEFT);
  void release(uint8_t buttons = BUTTON_LEFT);
  void click(uint8_t buttons = BUTTON_LEFT);
  void setBatteryLevel(uint8_t level);

private:
  class ServerCallbacks;

  static void serverTask(void *parameter);
  void startServer();
  void sendReport(int8_t x, int8_t y, int8_t wheel, int8_t horizontalWheel);

  const char *deviceName_;
  const char *manufacturer_;
  uint8_t batteryLevel_;
  uint8_t buttons_ = 0;
  volatile bool connected_ = false;
  NimBLEHIDDevice *hid_ = nullptr;
  NimBLECharacteristic *inputReport_ = nullptr;
  ServerCallbacks *callbacks_ = nullptr;
};
