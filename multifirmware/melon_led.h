#pragma once
#include <Adafruit_NeoPixel.h>
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#define MELON_LED_PIN 5
#define MELON_BUTTON_PIN 6
#define MELON_BOOTLOADER_MAGIC 0xCAFEAFFE
#define MELON_HOLD_TO_MENU_MS 5000

static Adafruit_NeoPixel __melon_neo(1, MELON_LED_PIN, NEO_GRB + NEO_KHZ800);
static bool __melon_neo_ready = false;

static uint32_t __melon_slot_color = 0;
static uint32_t __melon_hold_start = 0;
static bool     __melon_btn_was_down = false;

inline void melon_led_begin() {
    __melon_neo.begin();
    __melon_neo.setBrightness(255);
    __melon_neo.clear();
    __melon_neo.show();
    __melon_neo_ready = true;
}

inline void melon_led_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!__melon_neo_ready) return;
    __melon_neo.setPixelColor(0, __melon_neo.Color(r, g, b));
    __melon_neo.show();
}

inline void melon_led_color32(uint32_t c) {
    if (!__melon_neo_ready) return;
    __melon_neo.setPixelColor(0, c);
    __melon_neo.show();
}

inline void melon_led_brightness(uint32_t base_color, uint8_t brightness) {
    if (!__melon_neo_ready) return;
    uint8_t r = ((base_color >> 16) & 0xFF) * brightness / 255;
    uint8_t g = ((base_color >> 8) & 0xFF) * brightness / 255;
    uint8_t b = (base_color & 0xFF) * brightness / 255;
    __melon_neo.setPixelColor(0, __melon_neo.Color(r, g, b));
    __melon_neo.show();
}

inline void melon_led_off() {
    if (!__melon_neo_ready) return;
    __melon_neo.clear();
    __melon_neo.show();
}

inline void melon_led_dim() {
    melon_led_brightness(__melon_slot_color, 30);
}

inline void melon_led_slot_init(uint32_t color) {
    __melon_slot_color = color;
    melon_led_color32(color);
}

inline void melon_reboot_to_bootloader() {
    watchdog_hw->scratch[0] = MELON_BOOTLOADER_MAGIC;
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
}

inline void melon_check_bootloader_hold() {
    bool down = (digitalRead(MELON_BUTTON_PIN) == LOW);
    uint32_t now = millis();

    if (down && !__melon_btn_was_down) {
        __melon_hold_start = now;
    }

    if (down && __melon_btn_was_down) {
        if (now - __melon_hold_start >= MELON_HOLD_TO_MENU_MS) {
            melon_reboot_to_bootloader();
        }
    }

    if (!down) {
        __melon_hold_start = 0;
    }

    __melon_btn_was_down = down;
}
