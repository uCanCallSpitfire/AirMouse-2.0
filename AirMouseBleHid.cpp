#include "AirMouseBleHid.h"

#include <NimBLEAdvertising.h>
#include <NimBLEConnInfo.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>

namespace {
const uint8_t kMouseReportMap[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01,
  0x09, 0x01, 0xa1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
  0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
  0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0x05, 0x0c, 0x0a, 0x38, 0x02,
  0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
  0xc0, 0xc0
};
} // adsız ad alanı

class AirMouseBleHid::ServerCallbacks : public NimBLEServerCallbacks {
public:
  explicit ServerCallbacks(AirMouseBleHid &mouse) : mouse_(mouse) {}

  void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
    mouse_.connected_ = true;
    Serial.println("DURUM BLE_BAĞLANDI");
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    mouse_.connected_ = false;
    mouse_.buttons_ = 0;
    Serial.print("DURUM BLE_BAĞLANTISI_KESİLDİ neden=");
    Serial.println(reason);
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo &connection) override {
    Serial.print("DURUM BLE_KİMLİK_DOĞRULAMA şifreli=");
    Serial.print(connection.isEncrypted() ? 1 : 0);
    Serial.print(" eşleşmiş=");
    Serial.println(connection.isBonded() ? 1 : 0);
  }

private:
  AirMouseBleHid &mouse_;
};

AirMouseBleHid::AirMouseBleHid(const char *deviceName, const char *manufacturer, uint8_t batteryLevel)
    : deviceName_(deviceName), manufacturer_(manufacturer), batteryLevel_(batteryLevel) {}

void AirMouseBleHid::begin() {
  xTaskCreate(serverTask, "airmouse_ble", 20000, this, 5, nullptr);
}

bool AirMouseBleHid::isConnected() const {
  return connected_;
}

void AirMouseBleHid::move(int8_t x, int8_t y, int8_t wheel, int8_t horizontalWheel) {
  sendReport(x, y, wheel, horizontalWheel);
}

void AirMouseBleHid::press(uint8_t buttons) {
  uint8_t nextButtons = buttons_ | buttons;
  if (nextButtons != buttons_) {
    buttons_ = nextButtons;
    sendReport(0, 0, 0, 0);
  }
}

void AirMouseBleHid::release(uint8_t buttons) {
  uint8_t nextButtons = buttons_ & ~buttons;
  if (nextButtons != buttons_) {
    buttons_ = nextButtons;
    sendReport(0, 0, 0, 0);
  }
}

void AirMouseBleHid::click(uint8_t buttons) {
  press(buttons);
  delay(50);
  release(buttons);
}

void AirMouseBleHid::setBatteryLevel(uint8_t level) {
  batteryLevel_ = constrain(level, 0, 100);
  if (hid_ != nullptr) {
    hid_->setBatteryLevel(batteryLevel_, true);
  }
}

void AirMouseBleHid::serverTask(void *parameter) {
  static_cast<AirMouseBleHid *>(parameter)->startServer();
  vTaskDelete(nullptr);
}

void AirMouseBleHid::startServer() {
  NimBLEDevice::init(deviceName_);
  NimBLEDevice::setPower(3);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->advertiseOnDisconnect(true);
  callbacks_ = new ServerCallbacks(*this);
  server->setCallbacks(callbacks_, false);

  hid_ = new NimBLEHIDDevice(server);
  inputReport_ = hid_->getInputReport(1);
  hid_->setManufacturer(manufacturer_);
  hid_->setPnp(0x02, 0x02e5, 0xa111, 0x0210);
  hid_->setHidInfo(0x00, 0x02);
  hid_->setReportMap(const_cast<uint8_t *>(kMouseReportMap), sizeof(kMouseReportMap));
  hid_->startServices();
  hid_->setBatteryLevel(batteryLevel_);

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_MOUSE);
  advertising->addServiceUUID("1812");
  advertising->setName(deviceName_);
  advertising->start();

  Serial.println("DURUM BLE_NIMBLE_HID_HAZIR");
}

void AirMouseBleHid::sendReport(int8_t x, int8_t y, int8_t wheel, int8_t horizontalWheel) {
  if (!connected_ || inputReport_ == nullptr) {
    return;
  }

  uint8_t report[] = {
    buttons_,
    static_cast<uint8_t>(x),
    static_cast<uint8_t>(y),
    static_cast<uint8_t>(wheel),
    static_cast<uint8_t>(horizontalWheel)
  };
  inputReport_->setValue(report, sizeof(report));
  inputReport_->notify();
}
