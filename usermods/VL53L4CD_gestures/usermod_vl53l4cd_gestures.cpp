/*
 * Usermod for VL53L4CD Time-of-Flight sensor with gesture support.
 * Direct I2C implementation - no external library required.
 *
 * Features:
 * - Short gesture (swipe): triggers on/off ("shortPressAction")
 * - Long gesture (hold): adjusts brightness based on distance
 */
#include "wled.h"
#include <Wire.h>

#ifndef VL53L4CD_MAX_RANGE_MM
#define VL53L4CD_MAX_RANGE_MM 230
#endif

#ifndef VL53L4CD_MIN_RANGE_OFFSET
#define VL53L4CD_MIN_RANGE_OFFSET 60
#endif

#ifndef VL53L4CD_DELAY_MS
#define VL53L4CD_DELAY_MS 100
#endif

#ifndef VL53L4CD_LONG_MOTION_DELAY_MS
#define VL53L4CD_LONG_MOTION_DELAY_MS 1000
#endif

class UsermodVL53L4CDGestures : public Usermod {
  private:
    static const uint8_t VL53L4CD_ADDR = 0x29;

    // Key registers
    static const uint16_t REG_MODEL_ID = 0x010F;
    static const uint16_t REG_FIRMWARE_STATUS = 0x00E5;
    static const uint16_t REG_GPIO_HV_MUX = 0x0030;
    static const uint16_t REG_GPIO_TIO_STATUS = 0x0031;
    static const uint16_t REG_SYSTEM_START = 0x0087;
    static const uint16_t REG_CLEAR_INTERRUPT = 0x0086;
    static const uint16_t REG_RESULT_STATUS = 0x0089;
    static const uint16_t REG_RESULT_DISTANCE = 0x0096;

    unsigned long lastTime = 0;
    bool enabled = true;
    bool initialized = false;

    bool wasMotionBefore = false;
    bool isLongMotion = false;
    unsigned long motionStartTime = 0;

    // configurable parameters
    int16_t maxRange = VL53L4CD_MAX_RANGE_MM;
    int16_t minRangeOffset = VL53L4CD_MIN_RANGE_OFFSET;
    uint16_t delayMs = VL53L4CD_DELAY_MS;
    uint16_t longMotionDelayMs = VL53L4CD_LONG_MOTION_DELAY_MS;

    static const char _name[];
    static const char _enabled[];

    // Default configuration blob (registers 0x2D to 0x87) - 91 bytes
    static const uint8_t defaultConfig[91];

    bool writeReg8(uint16_t reg, uint8_t val) {
      Wire.beginTransmission(VL53L4CD_ADDR);
      Wire.write((reg >> 8) & 0xFF);
      Wire.write(reg & 0xFF);
      Wire.write(val);
      return Wire.endTransmission() == 0;
    }

    bool writeRegMulti(uint16_t reg, const uint8_t* data, size_t len) {
      Wire.beginTransmission(VL53L4CD_ADDR);
      Wire.write((reg >> 8) & 0xFF);
      Wire.write(reg & 0xFF);
      for (size_t i = 0; i < len; i++) {
        Wire.write(data[i]);
      }
      return Wire.endTransmission() == 0;
    }

    uint8_t readReg8(uint16_t reg) {
      Wire.beginTransmission(VL53L4CD_ADDR);
      Wire.write((reg >> 8) & 0xFF);
      Wire.write(reg & 0xFF);
      Wire.endTransmission(false);
      Wire.requestFrom(VL53L4CD_ADDR, (uint8_t)1);
      return Wire.read();
    }

    uint16_t readReg16(uint16_t reg) {
      Wire.beginTransmission(VL53L4CD_ADDR);
      Wire.write((reg >> 8) & 0xFF);
      Wire.write(reg & 0xFF);
      Wire.endTransmission(false);
      Wire.requestFrom(VL53L4CD_ADDR, (uint8_t)2);
      uint16_t val = Wire.read() << 8;
      val |= Wire.read();
      return val;
    }

    bool waitForBoot(uint16_t timeoutMs = 1000) {
      unsigned long start = millis();
      while (millis() - start < timeoutMs) {
        if (readReg8(REG_FIRMWARE_STATUS) == 0x03) {
          return true;
        }
        delay(1);
      }
      return false;
    }

    bool isDataReady() {
      uint8_t mux = readReg8(REG_GPIO_HV_MUX);
      uint8_t intPol = ((mux >> 4) & 1) ? 0 : 1;
      uint8_t status = readReg8(REG_GPIO_TIO_STATUS);
      return (status & 1) == intPol;
    }

    void clearInterrupt() {
      writeReg8(REG_CLEAR_INTERRUPT, 0x01);
    }

    void startRanging() {
      writeReg8(REG_SYSTEM_START, 0x40);
    }

    void stopRanging() {
      writeReg8(REG_SYSTEM_START, 0x00);
    }

    uint16_t getDistance() {
      return readReg16(REG_RESULT_DISTANCE);
    }

    uint8_t getRangeStatus() {
      static const uint8_t statusMap[24] = {
        255, 255, 255, 5, 2, 4, 1, 7, 3, 0, 255, 255,
        9, 13, 255, 255, 255, 255, 10, 6, 255, 255, 11, 12
      };
      uint8_t raw = readReg8(REG_RESULT_STATUS) & 0x1F;
      return (raw < 24) ? statusMap[raw] : 255;
    }

  public:
    void setup() {
      if (i2c_scl < 0 || i2c_sda < 0) {
        DEBUG_PRINTLN(F("VL53L4CD: I2C pins not configured"));
        enabled = false;
        return;
      }

      // Wait for sensor to boot
      if (!waitForBoot(2000)) {
        DEBUG_PRINTLN(F("VL53L4CD: Boot timeout"));
        enabled = false;
        return;
      }

      // Check sensor ID
      uint16_t modelId = readReg16(REG_MODEL_ID);
      if (modelId != 0xEBAA) {
        DEBUG_PRINTF("VL53L4CD: Wrong model ID 0x%04X\n", modelId);
        enabled = false;
        return;
      }

      // Write default configuration
      if (!writeRegMulti(0x002D, defaultConfig, 91)) {
        DEBUG_PRINTLN(F("VL53L4CD: Config failed"));
        enabled = false;
        return;
      }

      // Start ranging
      clearInterrupt();
      startRanging();

      initialized = true;
      DEBUG_PRINTLN(F("VL53L4CD: Initialized successfully"));
    }

    void loop() {
      if (!enabled || !initialized || strip.isUpdating()) return;

      if (millis() - lastTime > delayMs) {
        lastTime = millis();

        if (!isDataReady()) return;

        uint16_t range = getDistance();
        uint16_t signal = readReg16(0x008E);
        clearInterrupt();

        // range=0 with low signal means no target
        bool validTarget = (range > 0 && signal > 300) || (range > 0 && range < 1300);

        if (!validTarget) {
          // No valid target - treat as "hand removed"
          if (wasMotionBefore) {
            if (!isLongMotion) {
              DEBUG_PRINTLN(F("VL53L4CD: short gesture -> toggle"));
              shortPressAction();
            } else {
              DEBUG_PRINTLN(F("VL53L4CD: brightness mode ended"));
            }
            wasMotionBefore = false;
            isLongMotion = false;
          }
          return;
        }

        if (range < maxRange && range > 0) {
          if (!wasMotionBefore) {
            motionStartTime = millis();
          }
          wasMotionBefore = true;

          if (millis() - motionStartTime > longMotionDelayMs) {
            if (!isLongMotion) {
              isLongMotion = true;
              DEBUG_PRINTLN(F("VL53L4CD: entering brightness mode"));
            }

            int effectiveRange = max((int)range, (int)minRangeOffset);
            byte newBri = (maxRange - effectiveRange) * 255 / (maxRange - minRangeOffset);
            if (newBri != bri) {
              bri = newBri;
              DEBUG_PRINTF("VL53L4CD: brightness -> %d\n", bri);
              stateUpdated(CALL_MODE_DIRECT_CHANGE);
            }
          }
        } else if (wasMotionBefore) {
          if (!isLongMotion) {
            DEBUG_PRINTLN(F("VL53L4CD: short gesture -> toggle"));
            shortPressAction();
          } else {
            DEBUG_PRINTLN(F("VL53L4CD: brightness mode ended"));
          }
          wasMotionBefore = false;
          isLongMotion = false;
        }
      }
    }

    void addToConfig(JsonObject& root) {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[F("maxRange")] = maxRange;
      top[F("minOffset")] = minRangeOffset;
      top[F("delay")] = delayMs;
      top[F("longDelay")] = longMotionDelayMs;
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) {
        DEBUG_PRINTLN(F("VL53L4CD: No config found, using defaults"));
        return false;
      }

      bool configComplete = !top[FPSTR(_enabled)].isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[F("maxRange")], maxRange);
      configComplete &= getJsonValue(top[F("minOffset")], minRangeOffset);
      configComplete &= getJsonValue(top[F("delay")], delayMs);
      configComplete &= getJsonValue(top[F("longDelay")], longMotionDelayMs);

      return configComplete;
    }

    uint16_t getId() {
      return USERMOD_ID_VL53L4CD;
    }
};

const char UsermodVL53L4CDGestures::_name[] PROGMEM = "VL53L4CD";
const char UsermodVL53L4CDGestures::_enabled[] PROGMEM = "enabled";

// Default configuration (registers 0x2D through 0x87)
const uint8_t UsermodVL53L4CDGestures::defaultConfig[91] = {
  0x12, 0x00, 0x00, 0x11, 0x02, 0x00, 0x02, 0x08,
  0x00, 0x08, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00,
  0x00, 0xFF, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x20, 0x0B, 0x00, 0x00, 0x02, 0x14, 0x21,
  0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC8,
  0x00, 0x00, 0x38, 0xFF, 0x01, 0x00, 0x08, 0x00,
  0x00, 0x01, 0xCC, 0x07, 0x01, 0xF1, 0x05, 0x00,
  0xA0, 0x00, 0x80, 0x08, 0x38, 0x00, 0x00, 0x00,
  0x00, 0x0F, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x07, 0x05, 0x06, 0x06, 0x00,
  0x00, 0x02, 0xC7, 0xFF, 0x9B, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00
};

static UsermodVL53L4CDGestures vl53l4cd_gestures;
REGISTER_USERMOD(vl53l4cd_gestures);
