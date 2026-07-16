/*
 * ULTIMATE DUAL-BOOT MASTER FIRMWARE
 * Includes both RALPS (Sequencer/Looper) and METAL EDITION (Workstation)
 * Hardware: RP2350 (Pico 2)
 */

#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include <Arduino.h>
#include <EEPROM.h>

#ifndef F_CPU
#define F_CPU 150000000
#endif

// FPU-Safe Fast Pow (bypasses any libm errno locks or subnormal slowdowns)
inline float m_fast_powf(float base, float exp_val) {
  if (base <= 0.00001f)
    return 0.0f;
  return exp2f(exp_val * log2f(base));
}

// EEPROM Map:
// 0: Legacy LED (0 = NeoPixel, 1 = Legacy)
// 1: OS Mode (0 = METAL EDITION, 1 = RALPS)
// 2: METAL_EDITION current Engine
// 10+: RALPS Configuration

uint8_t system_boot_mode = 1; // 0 for Metal, 1 for Ralps
bool is_boot_complete = false;
bool system_hw_is_legacy = false; // DEFINITIVE GLOBAL

#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel systemStrip(1, 5, NEO_GRB + NEO_KHZ800);

void runMasterBootMenu() {
  EEPROM.begin(512);

  // Signature check to guarantee defaults on fresh flash or dirty EEPROM
  uint8_t signature = EEPROM.read(3);
  if (signature != 0x5B) {
    EEPROM.write(0, 0);    // Default: NeoPixel (0)
    EEPROM.write(1, 1);    // Default: RALPS OS (1)
    EEPROM.write(3, 0x5B); // Store signature
    EEPROM.commit();
  }

  uint8_t storedLed = EEPROM.read(0);
  uint8_t storedOs = EEPROM.read(1);
  system_boot_mode = storedOs;
  pinMode(6, INPUT_PULLUP);
  delay(50);

  if (digitalRead(6) == HIGH)
    return;

  uint32_t startTime = millis();
  bool selectionMade = false;
  bool selectingLegacy = (storedLed == 1);
  bool selectingRalps = (storedOs == 1);

  bool lastLegacyState = !selectingLegacy; // Force init

  uint32_t sysSliceLED = pwm_gpio_to_slice_num(5);

  while (digitalRead(6) == LOW || (millis() - startTime < 3000)) {
    watchdog_update();
    delay(10);

    // EarlePhilhower defaultet standardmäßig auf 10-bit! Daher 1023.0f
    float p1 = analogRead(A0) / 1023.0f;
    selectingRalps = (p1 > 0.5f);

    float p2 = analogRead(A1) / 1023.0f;
    // KORREKTUR: Laut User ist links (<0.5) Legacy und rechts (>0.5) NeoPixel!
    selectingLegacy = (p2 <= 0.5f);

    if (selectingLegacy != lastLegacyState) {
      if (selectingLegacy) {
        // Initialize PWM for Legacy sicher via Register
        gpio_set_function(5, GPIO_FUNC_PWM);
        pwm_set_wrap(sysSliceLED, 4095);
        pwm_set_enabled(sysSliceLED, true);
      } else {
        // Initialize NeoPixel
        systemStrip.begin();
      }
      lastLegacyState = selectingLegacy;
    }

    uint32_t color = 0;
    int brightness = 0;

    if (selectingRalps) {
      // RALPS: 8-Color Sequential Cycle
      uint32_t palette[] = {0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF,
                            0x0000FF, 0xFF00FF, 0xFFFFFF, 0xFF8000};
      int idx = (millis() / 150) % 8;
      color = palette[idx];
      brightness = 255;
    } else {
      // METAL: Pulsing Blue
      color = 0x0000FF;
      float pulse = 0.5f + 0.5f * sinf(millis() * 0.01f);
      brightness = (int)(pulse * 255.0f);
    }

    if (selectingLegacy) {
      // Legacy: Simple brightness mapping
      pwm_set_chan_level(sysSliceLED, PWM_CHAN_B, brightness * 16);
    } else {
      // NeoPixel: Full color rendering
      uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
      uint8_t g = ((color >> 8) & 0xFF) * brightness / 255;
      uint8_t b = ((color) & 0xFF) * brightness / 255;
      systemStrip.setPixelColor(0, systemStrip.Color(r, g, b));
      systemStrip.show();
    }

    if (digitalRead(6) == HIGH && millis() - startTime > 1000) {
      selectionMade = true;
      break;
    }
  }

  if (selectingLegacy) {
    pwm_set_chan_level(sysSliceLED, PWM_CHAN_B, 0);
  } else {
    systemStrip.setPixelColor(0, 0);
    systemStrip.show();
    systemStrip.clear();
  }

  if (selectingLegacy != (storedLed == 1))
    EEPROM.write(0, selectingLegacy ? 1 : 0);
  if (selectingRalps != (system_boot_mode == 1)) {
    system_boot_mode = selectingRalps ? 1 : 0;
    EEPROM.write(1, system_boot_mode);
  }
  EEPROM.commit();
}

// ----------------------------------------------------
// INCLUDE THE ENGINES
#include "BOUNCE_Engine.h"
#include "METAL_Engine.h"
#include "MULTI_Engine.h"
#include "PLONK_Engine.h"
#include "VOX_Engine.h"

// INCLUDE THE CORES
#include "METAL_Core.h"
#include "RALPS_Core.h"

// ----------------------------------------------------
// CORE 0 STARTUP
void setup() {
  runMasterBootMenu();
  is_boot_complete = true;

  if (system_boot_mode == 0) {
    m_setup();
  } else {
    r_setup();
  }
}

void loop() {
  if (system_boot_mode == 0) {
    m_loop();
  } else {
    r_loop();
  }
}

// ----------------------------------------------------
// CORE 1 STARTUP
void setup1() {
  while (!is_boot_complete) {
    delay(1);
  }
}

void loop1() {
  if (system_boot_mode == 0) {
    m_loop1();
  } else {
    r_loop1();
  }
}
