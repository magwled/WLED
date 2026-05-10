#include "wled.h"
#include <Wire.h>

#ifdef USERMOD_FOUR_LINE_DISPLAY
#include "usermod_v2_four_line_display.h"
#endif

//
// Usermod for the Arduino Modulino® Knob (ABX00107).
//
// The Modulino Knob is an STM32-backed I2C rotary encoder with push-button.
// It exposes a signed 16-bit position counter and a button state over I2C.
//
// Rotation controls one of several parameters; a short button press cycles
// through them; a long press toggles power.
//
// Modes (cycled by short press):
//   0: Brightness
//   1: Effect speed
//   2: Effect intensity
//   3: Palette
//   4: Effect
//

#ifndef MODULINO_KNOB_ADDRESS
// Documented (8-bit) I2C address as published by Arduino: 0x76 or 0x74.
// The Wire API needs the 7-bit form (address >> 1); the conversion is
// done internally so this value matches the Arduino library / datasheet.
#define MODULINO_KNOB_ADDRESS 0x76
#endif

#ifndef MODULINO_KNOB_POLL_MS
#define MODULINO_KNOB_POLL_MS 30
#endif

#ifndef MODULINO_KNOB_LONG_PRESS_MS
#define MODULINO_KNOB_LONG_PRESS_MS 1000
#endif

// Per-detent step sizes for analog parameters. The Modulino's encoder is a
// 30-detent/turn unit; with the firmware reporting 1 count per detent, a step
// of 4 sweeps the full 0–255 range in ~2 turns (≈ 0–100 % over two turns).
#ifndef MODULINO_KNOB_BRIGHTNESS_STEP
#define MODULINO_KNOB_BRIGHTNESS_STEP 4
#endif
#ifndef MODULINO_KNOB_SPEED_STEP
#define MODULINO_KNOB_SPEED_STEP 4
#endif
#ifndef MODULINO_KNOB_INTENSITY_STEP
#define MODULINO_KNOB_INTENSITY_STEP 4
#endif

// Usermod-specific debug logging. Enable with -D USERMOD_MODULINO_KNOB_DEBUG.
// Independent of WLED_DEBUG: writes to DEBUGOUT (Serial unless WLED_DEBUG_HOST set).
#ifdef USERMOD_MODULINO_KNOB_DEBUG
  #define MK_DEBUG_PRINTF(...)   DEBUGOUT.printf(__VA_ARGS__)
  #define MK_DEBUG_PRINTF_P(...) DEBUGOUT.printf_P(__VA_ARGS__)
#else
  #define MK_DEBUG_PRINTF(...)
  #define MK_DEBUG_PRINTF_P(...)
#endif

class ModulinoKnobUsermod : public Usermod {
  private:
    static const uint8_t MK_BRIGHTNESS = 0;
    static const uint8_t MK_SPEED      = 1;
    static const uint8_t MK_INTENSITY  = 2;
    static const uint8_t MK_PALETTE    = 3;
    static const uint8_t MK_EFFECT     = 4;
    static const uint8_t MK_NUM_MODES  = 5;

    uint8_t _i2cAddress      = MODULINO_KNOB_ADDRESS;
    uint16_t _pollIntervalMs = MODULINO_KNOB_POLL_MS;
    uint16_t _longPressMs    = MODULINO_KNOB_LONG_PRESS_MS;
    uint8_t _brightnessStep  = MODULINO_KNOB_BRIGHTNESS_STEP;
    uint8_t _speedStep       = MODULINO_KNOB_SPEED_STEP;
    uint8_t _intensityStep   = MODULINO_KNOB_INTENSITY_STEP;
    bool _enabled            = true;

    bool _initDone           = false;
    bool _present            = false;     // true if device responded at setup
    int16_t _lastPosition    = 0;
    bool _lastButton         = false;
    bool _buttonHandled      = false;     // prevents long-press from also firing short-press
    unsigned long _buttonPressedAt = 0;
    unsigned long _lastPollAt      = 0;
    uint8_t _selectedMode    = MK_BRIGHTNESS;

#ifdef USERMOD_FOUR_LINE_DISPLAY
    FourLineDisplayUsermod *_display = nullptr;
#endif

    static const char _name[];
    static const char _enabledKey[];
    static const char _addressKey[];
    static const char _pollKey[];
    static const char _longPressKey[];
    static const char _briStepKey[];
    static const char _spdStepKey[];
    static const char _intStepKey[];

    // Wire wants the 7-bit address; the configured value is the 8-bit form.
    inline uint8_t wireAddr() const { return _i2cAddress >> 1; }

    // Parse "0x76" / "118" / "76" — base=0 auto-detects 0x and 0 prefixes.
    static uint8_t parseAddress(const char *s, uint8_t fallback) {
      if (!s || !*s) return fallback;
      char *end = nullptr;
      long v = strtol(s, &end, 0);
      if (end == s || v < 0 || v > 0xFF) return fallback;
      return (uint8_t)v;
    }

    static String addressToHex(uint8_t addr) {
      char buf[6];
      snprintf(buf, sizeof(buf), "0x%02X", addr);
      return String(buf);
    }

    bool readKnob(int16_t &position, bool &pressed) {
      Wire.requestFrom(wireAddr(), (uint8_t)4);
      unsigned long start = millis();
      while (Wire.available() < 4 && millis() - start < 10) { /* wait briefly */ }
      if (Wire.available() < 4) {
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] read failed: only %d bytes available\n"), Wire.available());
        while (Wire.available()) Wire.read();
        return false;
      }
      (void)Wire.read();                   // pinstrap_address — unused
      uint8_t lo = Wire.read();
      uint8_t hi = Wire.read();
      pressed = Wire.read() != 0;
      position = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
      return true;
    }

    bool resetKnobPosition() {
      Wire.beginTransmission(wireAddr());
      // 4 bytes: int16 position (zero) + 2 bytes padding
      Wire.write((uint8_t)0); Wire.write((uint8_t)0);
      Wire.write((uint8_t)0); Wire.write((uint8_t)0);
      return Wire.endTransmission() == 0;
    }

    bool probe() {
      Wire.beginTransmission(wireAddr());
      return Wire.endTransmission() == 0;
    }

    // Returns true if the input should be swallowed (display was asleep and just woke).
    bool wakeOrKeepAwake() {
#ifdef USERMOD_FOUR_LINE_DISPLAY
      if (_display) {
        if (_display->wakeDisplay()) {
          _display->redraw(true);
          return true;
        }
        _display->updateRedrawTime();
      }
#endif
      return false;
    }

    void showRotationFeedback() {
#ifdef USERMOD_FOUR_LINE_DISPLAY
      if (!_display) return;
      switch (_selectedMode) {
        case MK_BRIGHTNESS: _display->updateBrightness(); break;
        case MK_SPEED:      _display->updateSpeed();      break;
        case MK_INTENSITY:  _display->updateIntensity();  break;
        case MK_PALETTE:    _display->showCurrentEffectOrPalette(effectPalette, JSON_palette_names, 2); break;
        case MK_EFFECT:     _display->showCurrentEffectOrPalette(effectCurrent, JSON_mode_names,    3); break;
      }
#endif
    }

    void showModeOverlay() {
#ifdef USERMOD_FOUR_LINE_DISPLAY
      if (!_display) return;
      // Glyph + mark line/col mirror usermod_v2_rotary_encoder_ui_ALT for consistency.
      static const char* labels[] = { "Brightness", "Speed", "Intensity", "Color Palette", "Effect" };
      static const uint8_t glyphs[] = { 1, 2, 3, 4, 5 };       // sun, skip-fwd, fire, palette, effect
      static const uint8_t markRow[] = { 1, 1, 1, 2, 3 };
      static const uint8_t markCol[] = { 0, 4, 8, 0, 0 };
      _display->overlay(labels[_selectedMode], 750, glyphs[_selectedMode]);
      _display->setMarkLine(markRow[_selectedMode], markCol[_selectedMode]);
#endif
    }

    void applyDelta(int16_t delta) {
      if (delta == 0) return;
      bool stateChange = true;
      switch (_selectedMode) {
        case MK_BRIGHTNESS: {
          int v = (int)bri + delta * _brightnessStep;
          bri = (uint8_t)constrain(v, 0, 255);
          break;
        }
        case MK_SPEED: {
          int v = (int)effectSpeed + delta * _speedStep;
          effectSpeed = (uint8_t)constrain(v, 0, 255);
          for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
            Segment &seg = strip.getSegment(i);
            if (seg.isActive()) seg.speed = effectSpeed;
          }
          break;
        }
        case MK_INTENSITY: {
          int v = (int)effectIntensity + delta * _intensityStep;
          effectIntensity = (uint8_t)constrain(v, 0, 255);
          for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
            Segment &seg = strip.getSegment(i);
            if (seg.isActive()) seg.intensity = effectIntensity;
          }
          break;
        }
        case MK_PALETTE: {
          int max = (int)getPaletteCount() + (int)customPalettes.size() - 1;
          int v = (int)effectPalette + delta;
          effectPalette = (uint8_t)constrain(v, 0, max);
          for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
            Segment &seg = strip.getSegment(i);
            if (seg.isActive()) seg.setPalette(effectPalette);
          }
          break;
        }
        case MK_EFFECT: {
          int max = (int)strip.getModeCount() - 1;
          int v = (int)effectCurrent + delta;
          if (v < 0) v = 0;
          if (v > max) v = max;
          effectCurrent = (uint8_t)v;
          for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
            Segment &seg = strip.getSegment(i);
            if (seg.isActive()) seg.setMode(effectCurrent);
          }
          break;
        }
        default: stateChange = false; break;
      }
      if (stateChange) {
        stateChanged = true;
        stateUpdated(CALL_MODE_BUTTON);
        updateInterfaces(CALL_MODE_BUTTON);
      }
    }

    void onShortPress() {
      _selectedMode = (_selectedMode + 1) % MK_NUM_MODES;
      static const char *modeNames[] = {"Brightness", "Speed", "Intensity", "Palette", "Effect"};
      MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] short press -> mode=%s\n"), modeNames[_selectedMode]);
      showModeOverlay();
    }

    void onLongPress() {
      MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] long press -> toggle on/off\n"));
      toggleOnOff();
      stateUpdated(CALL_MODE_BUTTON);
      updateInterfaces(CALL_MODE_BUTTON);
    }

  public:
    void setup() override {
      if (!_enabled) return;
      if (i2c_sda < 0 || i2c_scl < 0) {
        DEBUG_PRINTLN(F("Modulino Knob: I2C pins not configured, disabling."));
        _enabled = false;
        return;
      }
      MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] setup: probing addr 0x%02X (wire 0x%02X) on SDA=%d SCL=%d\n"),
                       _i2cAddress, wireAddr(), i2c_sda, i2c_scl);
      _present = probe();
      if (!_present) {
        DEBUG_PRINTF_P(PSTR("Modulino Knob: no device at 0x%02X\n"), _i2cAddress);
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] device NOT found at 0x%02X (wire 0x%02X)\n"), _i2cAddress, wireAddr());
#ifdef USERMOD_MODULINO_KNOB_DEBUG
        DEBUGOUT.print(F("[ModulinoKnob] scanning I2C bus, found 7-bit addresses:"));
        uint8_t found = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
          Wire.beginTransmission(a);
          if (Wire.endTransmission() == 0) {
            DEBUGOUT.printf(" 0x%02X", a);
            found++;
          }
        }
        if (found == 0) DEBUGOUT.print(F(" (none — check wiring, pull-ups, power)"));
        DEBUGOUT.println();
#endif
      } else {
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] device found at 0x%02X\n"), _i2cAddress);
        resetKnobPosition();
        _lastPosition = 0;
      }
#ifdef USERMOD_FOUR_LINE_DISPLAY
      _display = (FourLineDisplayUsermod*) UsermodManager::lookup(USERMOD_ID_FOUR_LINE_DISP);
      if (_display) {
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] FourLineDisplay attached\n"));
        _display->setMarkLine(1, 0);
      }
#endif
      _initDone = true;
    }

    void loop() override {
      if (!_enabled || !_initDone || !_present) return;
      unsigned long now = millis();
      if (now - _lastPollAt < _pollIntervalMs) return;
      _lastPollAt = now;
      if (strip.isUpdating()) return;

      int16_t position;
      bool pressed;
      if (!readKnob(position, pressed)) return;

      int16_t delta = position - _lastPosition;
      _lastPosition = position;
      if (delta != 0) {
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] rotate delta=%d pos=%d mode=%u\n"), delta, position, _selectedMode);
        if (!wakeOrKeepAwake()) {
          applyDelta(delta);
          showRotationFeedback();
        }
      }

      if (pressed && !_lastButton) {
        MK_DEBUG_PRINTF_P(PSTR("[ModulinoKnob] button pressed\n"));
        _buttonPressedAt = now;
        if (wakeOrKeepAwake()) {
          // Display was asleep — swallow this press so it acts as wake-only.
          _buttonHandled = true;
        } else {
          _buttonHandled = false;
        }
      } else if (pressed && !_buttonHandled && (now - _buttonPressedAt) >= _longPressMs) {
        onLongPress();
        _buttonHandled = true;
      } else if (!pressed && _lastButton && !_buttonHandled) {
        onShortPress();
      }
      _lastButton = pressed;
    }

    uint16_t getId() override { return USERMOD_ID_MODULINO_KNOB; }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray arr = user.createNestedArray(F("Modulino Knob"));
      if (!_enabled) {
        arr.add(F("disabled"));
      } else if (!_present) {
        arr.add(F("not detected"));
      } else {
        static const char *names[] = {"Brightness", "Speed", "Intensity", "Palette", "Effect"};
        arr.add(names[_selectedMode]);
      }
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabledKey)]   = _enabled;
      top[FPSTR(_addressKey)]   = addressToHex(_i2cAddress);
      top[FPSTR(_pollKey)]      = _pollIntervalMs;
      top[FPSTR(_longPressKey)] = _longPressMs;
      top[FPSTR(_briStepKey)]   = _brightnessStep;
      top[FPSTR(_spdStepKey)]   = _speedStep;
      top[FPSTR(_intStepKey)]   = _intensityStep;
    }

    void appendConfigData() override {
      oappend(F("addInfo('"));
      oappend(String(FPSTR(_name)).c_str());
      oappend(F(":"));
      oappend(String(FPSTR(_addressKey)).c_str());
      oappend(F("',1,'<i>(hex: 0x76 or 0x74)</i>');"));
      oappend(F("addInfo('"));
      oappend(String(FPSTR(_name)).c_str());
      oappend(F(":"));
      oappend(String(FPSTR(_briStepKey)).c_str());
      oappend(F("',1,'<i>per detent (4 ≈ full range in 2 turns)</i>');"));
      oappend(F("addInfo('"));
      oappend(String(FPSTR(_name)).c_str());
      oappend(F(":"));
      oappend(String(FPSTR(_spdStepKey)).c_str());
      oappend(F("',1,'<i>per detent</i>');"));
      oappend(F("addInfo('"));
      oappend(String(FPSTR(_name)).c_str());
      oappend(F(":"));
      oappend(String(FPSTR(_intStepKey)).c_str());
      oappend(F("',1,'<i>per detent</i>');"));
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;
      bool wasEnabled = _enabled;
      uint8_t oldAddr = _i2cAddress;

      _enabled        = top[FPSTR(_enabledKey)]   | _enabled;
      // Address is stored as a string ("0x76") so the UI renders a text field
      // accepting hex; fall back to legacy decimal numbers for cfg.json upgrade.
      JsonVariant addrVar = top[FPSTR(_addressKey)];
      if (addrVar.is<const char*>())   _i2cAddress = parseAddress(addrVar.as<const char*>(), _i2cAddress);
      else if (addrVar.is<uint32_t>()) _i2cAddress = (uint8_t)addrVar.as<uint32_t>();
      _pollIntervalMs = top[FPSTR(_pollKey)]      | _pollIntervalMs;
      _longPressMs    = top[FPSTR(_longPressKey)] | _longPressMs;
      _brightnessStep = top[FPSTR(_briStepKey)]   | _brightnessStep;
      _speedStep      = top[FPSTR(_spdStepKey)]   | _speedStep;
      _intensityStep  = top[FPSTR(_intStepKey)]   | _intensityStep;

      if (_initDone && (wasEnabled != _enabled || oldAddr != _i2cAddress)) {
        _present = false;
        if (_enabled) setup();
      }
      return !top[FPSTR(_intStepKey)].isNull();
    }
};

const char ModulinoKnobUsermod::_name[]         PROGMEM = "Modulino-Knob";
const char ModulinoKnobUsermod::_enabledKey[]   PROGMEM = "enabled";
const char ModulinoKnobUsermod::_addressKey[]   PROGMEM = "I2C-address";
const char ModulinoKnobUsermod::_pollKey[]      PROGMEM = "poll-interval-ms";
const char ModulinoKnobUsermod::_longPressKey[] PROGMEM = "long-press-ms";
const char ModulinoKnobUsermod::_briStepKey[]   PROGMEM = "brightness-step";
const char ModulinoKnobUsermod::_spdStepKey[]   PROGMEM = "speed-step";
const char ModulinoKnobUsermod::_intStepKey[]   PROGMEM = "intensity-step";

static ModulinoKnobUsermod modulino_knob_usermod;
REGISTER_USERMOD(modulino_knob_usermod);
