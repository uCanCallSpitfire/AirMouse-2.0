#include <Wire.h>
#include <MPU6050_light.h>
#include "AirMouseBleHid.h"
#include <math.h>
#include <string.h>

/*
  ESP32-C3 Mini + MPU6050 BLE Hava Faresi

  ESP32-C3, Bluetooth LE HID fare olarak çalışır. Windows'ta "C3 AirMouse BLE"
  aygıtıyla eşleştirilir; USB yardımcı yazılımı gerekmez.

    Bağlantılar:
    MPU6050 VCC -> ESP32-C3 3V3
    MPU6050 GND -> ESP32-C3 GND
    Önerilen sensör pinleri:
    MPU6050 SDA -> ESP32-C3 GPIO3
    MPU6050 SCL -> ESP32-C3 GPIO4
    Sağ tıklama / basılı tutarak kaydırma düğmesi uç 1 -> ESP32-C3 GPIO0
    Düğme uç 2 -> ESP32-C3 GND
    Sol tıklama düğmesi uç 1 -> ESP32-C3 GPIO1
    Düğme uç 2 -> ESP32-C3 GND
    GPIO9 etkin I2C SCL pini olarak seçilmemişse dahili BOOT/GPIO9 düğmesi de
    tıklama ve kaydırma için kullanılabilir.

    Yazılım yaygın ESP32-C3 pin çiftlerini otomatik olarak da tarar.
*/

static const int SDA_PIN = 3;
static const int SCL_PIN = 4;

static const bool INVERT_X = false;
static const bool INVERT_Y = false;
static const bool SWAP_XY = false;

static const float SENSITIVITY = 0.93f;
static const float DEAD_ZONE = 1.15f;
static const float SMOOTHING = 0.58f;
static const int MAX_STEP = 16;
static const float MAX_CARRY = MAX_STEP * 2.0f;
static const float GYRO_DRIFT_LEARN_THRESHOLD = 30.0f;
static const float GYRO_DRIFT_ALPHA = 0.006f;
static const float GYRO_SPIKE_LIMIT = 220.0f;
static const uint16_t LOOP_DELAY_MS = 15;
static const uint16_t CALIBRATION_SETTLE_MS = 2500;

static const bool GESTURES_ENABLED = false;
static const bool FIRMWARE_VERTICAL_GESTURES = false;
static const float VERTICAL_FLICK_THRESHOLD = 14.0f;
static const float VERTICAL_RELEASE_THRESHOLD = 4.0f;
static const uint16_t DOUBLE_FLICK_WINDOW_MS = 700;
static const uint16_t GESTURE_POINTER_PAUSE_MS = 260;

// Çift ileri hareketle tıklama algılanmıyorsa FORWARD_ACCEL_SIGN değerini
// -1.0f veya FORWARD_ACCEL_AXIS değerini 0/2 yapmayı deneyin.
static const int FORWARD_ACCEL_AXIS = 1;       // 0=X, 1=Y, 2=Z
static const float FORWARD_ACCEL_SIGN = 1.0f;
static const float FORWARD_ACCEL_THRESHOLD = 0.24f;
static const float FORWARD_RELEASE_THRESHOLD = 0.10f;
static const float ACCEL_BASELINE_ALPHA = 0.96f;

static const bool BUTTON_CLICK_ENABLED = true;
static const bool BOOT_CLICK_BUTTON_ENABLED = true;
static const int BOOT_CLICK_BUTTON_PIN = 9; // I2C kullanmıyorsa dahili BOOT düğmesi
static const int RIGHT_CLICK_BUTTON_PIN = 0; // Kısa bas: sağ tık; basılı tut ve eğ: kaydır
static const int LEFT_CLICK_BUTTON_PIN = 1;  // Basılı tutulduğu sürece sol tuş basılı
static const bool CLICK_BUTTON_ACTIVE_LOW = true;
static const uint16_t BUTTON_DEBOUNCE_MS = 35;
static const uint16_t BUTTON_CLICK_MAX_MS = 800;
static const uint16_t BUTTON_HOLD_SCROLL_MS = 300;
static const uint16_t BUTTON_SCROLL_INTERVAL_MS = 115;
static const float BUTTON_SCROLL_THRESHOLD = 4.4f;
static const int BLE_SCROLL_STEPS = 3;

MPU6050 mpu(Wire);
AirMouseBleHid bleMouse("C3 AirMouse BLE", "ESP32-C3", 100);

int candidatePins[] = {
  SDA_PIN, SCL_PIN, 6, 7, 8, 9, 10, 20, 21
};

int activeSdaPin = -1;
int activeSclPin = -1;
float smoothX = 0.0f;
float smoothY = 0.0f;
float carryX = 0.0f;
float carryY = 0.0f;
float gyroXDrift = 0.0f;
float gyroYDrift = 0.0f;
float accelBaseline = 0.0f;
bool accelBaselineReady = false;
bool verticalFlickReady = true;
bool forwardFlickReady = true;
struct ButtonState {
  bool lastRaw = false;
  bool stable = false;
  bool scrollMode = false;
  bool clickSent = false;
  unsigned long changedMs = 0;
  unsigned long pressedMs = 0;
  unsigned long lastScrollMs = 0;
};

ButtonState bootButton;
ButtonState rightButton;
ButtonState leftButton;
int lastVerticalDir = 0;
unsigned long lastVerticalMs = 0;
unsigned long lastForwardMs = 0;
unsigned long pointerPauseUntilMs = 0;

float applyDeadZone(float value) {
  if (fabs(value) <= DEAD_ZONE) {
    return 0.0f;
  }
  return value > 0.0f ? value - DEAD_ZONE : value + DEAD_ZONE;
}

float sanitizeGyro(float value) {
  if (isnan(value) || isinf(value)) {
    return 0.0f;
  }
  return constrain(value, -GYRO_SPIKE_LIMIT, GYRO_SPIKE_LIMIT);
}

void updateGyroDrift(float gyroX, float gyroY) {
  if (fabs(gyroX) >= GYRO_DRIFT_LEARN_THRESHOLD || fabs(gyroY) >= GYRO_DRIFT_LEARN_THRESHOLD) {
    return;
  }

  gyroXDrift = (gyroXDrift * (1.0f - GYRO_DRIFT_ALPHA)) + (gyroX * GYRO_DRIFT_ALPHA);
  gyroYDrift = (gyroYDrift * (1.0f - GYRO_DRIFT_ALPHA)) + (gyroY * GYRO_DRIFT_ALPHA);
}

int takeMouseStep(float &carry) {
  if (isnan(carry) || isinf(carry)) {
    carry = 0.0f;
  }

  carry = constrain(carry, -MAX_CARRY, MAX_CARRY);
  int wholeStep = (int)carry;
  int step = constrain(wholeStep, -MAX_STEP, MAX_STEP);
  carry -= step;
  if (fabs(carry) < 0.001f) {
    carry = 0.0f;
  }
  return step;
}

void printStatus(const char *message) {
  Serial.print("DURUM ");
  Serial.println(message);
}

void performBleGesture(const char *message) {
  if (!bleMouse.isConnected()) {
    return;
  }

  if (strcmp(message, "AŞAĞI_KAYDIR") == 0) {
    bleMouse.move(0, 0, -BLE_SCROLL_STEPS);
  } else if (strcmp(message, "YUKARI_KAYDIR") == 0) {
    bleMouse.move(0, 0, BLE_SCROLL_STEPS);
  } else if (strcmp(message, "SOL_TIK") == 0) {
    bleMouse.press(AirMouseBleHid::BUTTON_LEFT);
    delay(80);
    bleMouse.release(AirMouseBleHid::BUTTON_LEFT);
  } else if (strcmp(message, "SOL_BAS") == 0) {
    bleMouse.press(AirMouseBleHid::BUTTON_LEFT);
  } else if (strcmp(message, "SOL_BIRAK") == 0) {
    bleMouse.release(AirMouseBleHid::BUTTON_LEFT);
  } else if (strcmp(message, "SAĞ_TIK") == 0) {
    bleMouse.press(AirMouseBleHid::BUTTON_RIGHT);
    delay(120);
    bleMouse.release(AirMouseBleHid::BUTTON_RIGHT);
  }
}

void printGesture(const char *message) {
  Serial.print("HAREKET ");
  Serial.println(message);
  performBleGesture(message);
  pointerPauseUntilMs = millis() + GESTURE_POINTER_PAUSE_MS;
  smoothX = 0.0f;
  smoothY = 0.0f;
  carryX = 0.0f;
  carryY = 0.0f;
}

bool buttonPinAvailable(int pin) {
  return pin >= 0 && pin != activeSdaPin && pin != activeSclPin;
}

bool readButtonPressed(int pin) {
  if (!buttonPinAvailable(pin)) {
    return false;
  }

  pinMode(pin, INPUT_PULLUP);
  return CLICK_BUTTON_ACTIVE_LOW ? digitalRead(pin) == LOW : digitalRead(pin) == HIGH;
}

void initButton(ButtonState &state, int pin) {
  if (buttonPinAvailable(pin)) {
    pinMode(pin, INPUT_PULLUP);
  }

  state.lastRaw = readButtonPressed(pin);
  state.stable = state.lastRaw;
  state.scrollMode = false;
  state.clickSent = false;
  state.changedMs = millis();
  state.pressedMs = 0;
  state.lastScrollMs = 0;
}

void initClickButtons() {
  if (BOOT_CLICK_BUTTON_ENABLED) {
    initButton(bootButton, BOOT_CLICK_BUTTON_PIN);
  }
  initButton(rightButton, RIGHT_CLICK_BUTTON_PIN);
  initButton(leftButton, LEFT_CLICK_BUTTON_PIN);
}

void updateClickButton(ButtonState &state, int pin, const char *clickGesture, bool holdScrollEnabled, bool clickOnPress, float filteredY) {
  if (!BUTTON_CLICK_ENABLED || !buttonPinAvailable(pin)) {
    return;
  }

  unsigned long now = millis();
  bool raw = readButtonPressed(pin);

  if (raw != state.lastRaw) {
    state.lastRaw = raw;
    state.changedMs = now;
  }

  if (now - state.changedMs < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (raw == state.stable) {
    if (clickOnPress && state.stable && !state.clickSent) {
      printGesture(clickGesture);
      state.clickSent = true;
    }

    if (holdScrollEnabled && state.stable && now - state.pressedMs >= BUTTON_HOLD_SCROLL_MS) {
      state.scrollMode = true;
      if (now - state.lastScrollMs >= BUTTON_SCROLL_INTERVAL_MS) {
        if (filteredY > BUTTON_SCROLL_THRESHOLD) {
          printGesture("AŞAĞI_KAYDIR");
          state.lastScrollMs = now;
        } else if (filteredY < -BUTTON_SCROLL_THRESHOLD) {
          printGesture("YUKARI_KAYDIR");
          state.lastScrollMs = now;
        }
      }
    }
    return;
  }

  state.stable = raw;
  if (state.stable) {
    state.pressedMs = now;
    state.scrollMode = false;
    state.clickSent = false;
    state.lastScrollMs = 0;
    if (clickOnPress) {
      printGesture(clickGesture);
      state.clickSent = true;
    }
  } else {
    unsigned long heldMs = now - state.pressedMs;
    if (!state.scrollMode && !state.clickSent && heldMs <= BUTTON_CLICK_MAX_MS) {
      printGesture(clickGesture);
      state.clickSent = true;
    }
  }
}

void updateHoldMouseButton(ButtonState &state, int pin, const char *downGesture, const char *upGesture) {
  if (!BUTTON_CLICK_ENABLED || !buttonPinAvailable(pin)) {
    return;
  }

  unsigned long now = millis();
  bool raw = readButtonPressed(pin);

  if (raw != state.lastRaw) {
    state.lastRaw = raw;
    state.changedMs = now;
  }

  if (now - state.changedMs < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (raw == state.stable) {
    if (state.stable && !state.clickSent) {
      printGesture(downGesture);
      state.clickSent = true;
    }
    return;
  }

  state.stable = raw;
  if (state.stable) {
    state.pressedMs = now;
    state.clickSent = false;
    printGesture(downGesture);
    state.clickSent = true;
  } else {
    if (state.clickSent) {
      printGesture(upGesture);
      state.clickSent = false;
    }
  }
}

void updateButtonControl(float filteredY) {
  updateClickButton(rightButton, RIGHT_CLICK_BUTTON_PIN, "SAĞ_TIK", true, false, filteredY);
  updateHoldMouseButton(leftButton, LEFT_CLICK_BUTTON_PIN, "SOL_BAS", "SOL_BIRAK");
  if (BOOT_CLICK_BUTTON_ENABLED) {
    updateClickButton(bootButton, BOOT_CLICK_BUTTON_PIN, "SOL_TIK", true, false, filteredY);
  }
}

bool deviceResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool beginMpuAt(uint8_t address) {
  mpu.setAddress(address);
  return mpu.begin(1, 0) == 0;
}

bool findAndBeginMpu() {
  for (size_t sdaIndex = 0; sdaIndex < sizeof(candidatePins) / sizeof(candidatePins[0]); sdaIndex++) {
    for (size_t sclIndex = 0; sclIndex < sizeof(candidatePins) / sizeof(candidatePins[0]); sclIndex++) {
      int sda = candidatePins[sdaIndex];
      int scl = candidatePins[sclIndex];

      if (sda == scl) {
        continue;
      }

      Wire.end();
      delay(20);
      pinMode(sda, INPUT_PULLUP);
      pinMode(scl, INPUT_PULLUP);
      delay(10);
      Wire.begin(sda, scl);
      Wire.setClock(100000);
      delay(30);

      uint8_t address = 0;
      if (deviceResponds(0x68)) {
        address = 0x68;
      } else if (deviceResponds(0x69)) {
        address = 0x69;
      }

      if (address != 0) {
        Serial.print("DURUM I2C_BULUNDU SDA=");
        Serial.print(sda);
        Serial.print(" SCL=");
        Serial.print(scl);
        Serial.print(" ADDR=0x");
        Serial.println(address, HEX);
        bool started = beginMpuAt(address);
        if (started) {
          activeSdaPin = sda;
          activeSclPin = scl;
        }
        return started;
      }
    }
  }

  return false;
}

float selectedAccelAxis() {
  if (FORWARD_ACCEL_AXIS == 0) {
    return mpu.getAccX();
  }
  if (FORWARD_ACCEL_AXIS == 2) {
    return mpu.getAccZ();
  }
  return mpu.getAccY();
}

void updateVerticalGesture(float filteredY) {
  unsigned long now = millis();
  int dir = 0;

  if (filteredY > VERTICAL_FLICK_THRESHOLD) {
    dir = 1;
  } else if (filteredY < -VERTICAL_FLICK_THRESHOLD) {
    dir = -1;
  }

  if (lastVerticalDir != 0 && now - lastVerticalMs > DOUBLE_FLICK_WINDOW_MS) {
    lastVerticalDir = 0;
  }

  if (dir != 0 && verticalFlickReady) {
    if (lastVerticalDir == dir && now - lastVerticalMs <= DOUBLE_FLICK_WINDOW_MS) {
      printGesture(dir > 0 ? "AŞAĞI_KAYDIR" : "YUKARI_KAYDIR");
      lastVerticalDir = 0;
    } else {
      lastVerticalDir = dir;
      lastVerticalMs = now;
    }
    verticalFlickReady = false;
  }

  if (!verticalFlickReady && fabs(filteredY) < VERTICAL_RELEASE_THRESHOLD) {
    verticalFlickReady = true;
  }
}

void updateForwardGesture() {
  unsigned long now = millis();
  float acc = selectedAccelAxis();

  if (!accelBaselineReady) {
    accelBaseline = acc;
    accelBaselineReady = true;
    return;
  }

  float pulse = (acc - accelBaseline) * FORWARD_ACCEL_SIGN;
  accelBaseline = (accelBaseline * ACCEL_BASELINE_ALPHA) + (acc * (1.0f - ACCEL_BASELINE_ALPHA));

  if (lastForwardMs != 0 && now - lastForwardMs > DOUBLE_FLICK_WINDOW_MS) {
    lastForwardMs = 0;
  }

  if (pulse > FORWARD_ACCEL_THRESHOLD && forwardFlickReady) {
    if (lastForwardMs != 0 && now - lastForwardMs <= DOUBLE_FLICK_WINDOW_MS) {
      printGesture("SOL_TIK");
      lastForwardMs = 0;
    } else {
      lastForwardMs = now;
    }
    forwardFlickReady = false;
  }

  if (!forwardFlickReady && pulse < FORWARD_RELEASE_THRESHOLD) {
    forwardFlickReady = true;
  }
}

void updateGestures(float filteredY) {
  if (!GESTURES_ENABLED) {
    return;
  }

  if (FIRMWARE_VERTICAL_GESTURES) {
    updateVerticalGesture(filteredY);
  }
  updateForwardGesture();
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  printStatus("BLE HID fare başlatılıyor");
  bleMouse.begin();
  printStatus("MPU6050 başlatılıyor");

  while (!findAndBeginMpu()) {
    Serial.println("HATA MPU6050_BULUNAMADI_VCC_GND_SDA_SCL_KONTROL_EDIN");
    Serial.println("DURUM I2C taraması yeniden deneniyor");
    delay(1000);
  }

  Wire.setClock(100000);
  initClickButtons();

  printStatus("MPU6050 hazır");
  printStatus("Kalibrasyon başlıyor, cihazı sabit tutun");
  delay(CALIBRATION_SETTLE_MS);
  mpu.calcOffsets(true, true);
  gyroXDrift = 0.0f;
  gyroYDrift = 0.0f;
  smoothX = 0.0f;
  smoothY = 0.0f;
  carryX = 0.0f;
  carryY = 0.0f;
  printStatus("Kalibrasyon tamamlandı");
  printStatus("BLE hava faresi hazır, C3 AirMouse BLE olarak eşleştirin");
}

void loop() {
  mpu.update();

  float gyroX = sanitizeGyro(mpu.getGyroX());
  float gyroY = sanitizeGyro(mpu.getGyroY());
  updateGyroDrift(gyroX, gyroY);

  float moveX = (gyroY - gyroYDrift) * SENSITIVITY;
  float moveY = -(gyroX - gyroXDrift) * SENSITIVITY;

  if (SWAP_XY) {
    float oldX = moveX;
    moveX = moveY;
    moveY = oldX;
  }

  if (INVERT_X) {
    moveX = -moveX;
  }
  if (INVERT_Y) {
    moveY = -moveY;
  }

  smoothX = (smoothX * SMOOTHING) + (moveX * (1.0f - SMOOTHING));
  smoothY = (smoothY * SMOOTHING) + (moveY * (1.0f - SMOOTHING));

  float filteredX = applyDeadZone(smoothX);
  float filteredY = applyDeadZone(smoothY);

  updateGestures(filteredY);
  updateButtonControl(filteredY);

  if (millis() < pointerPauseUntilMs) {
    smoothX = 0.0f;
    smoothY = 0.0f;
    carryX = 0.0f;
    carryY = 0.0f;
    delay(LOOP_DELAY_MS);
    return;
  }

  if (filteredX == 0.0f) {
    smoothX = 0.0f;
    carryX = 0.0f;
  } else {
    carryX += filteredX;
  }

  if (filteredY == 0.0f) {
    smoothY = 0.0f;
    carryY = 0.0f;
  } else {
    carryY += filteredY;
  }

  int dx = takeMouseStep(carryX);
  int dy = takeMouseStep(carryY);

  if (dx != 0 || dy != 0) {
    if (bleMouse.isConnected()) {
      bleMouse.move((signed char)dx, (signed char)dy, 0);
    }
  }

  delay(LOOP_DELAY_MS);
}
