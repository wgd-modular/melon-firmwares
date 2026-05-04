/*
 * Melon Multi-Firmware Bootloader
 *
 * Manages 8 firmware slots on a Seeed XIAO RP2350 (Melon / MOD2 module).
 * On boot, checks for a 5-second button hold on GPIO6 to enter the
 * slot selection menu. Otherwise jumps directly to the active firmware.
 *
 * Flash layout (128 KB bootloader + 8 x 240 KB slots = 2 MB):
 *   0x10000000  Bootloader  (128 KB)
 *   0x10020000  Slot 0 — Braids
 *   0x1005C000  Slot 1 — Kick
 *   0x10098000  Slot 2 — Clap
 *   0x100D4000  Slot 3 — Hi-Hat
 *   0x10110000  Slot 4 — VCO
 *   0x1014C000  Slot 5 — FM Drum
 *   0x10188000  Slot 6 — Claves
 *   0x101C4000  Slot 7 — MOD303
 *
 * Bootloader config at 0x1001F000 (last 4 KB of bootloader region):
 *   byte 0: active slot index (0-7)
 *   byte 1: magic marker (0xA5)
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/resets.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

/* ------------------------------------------------------------------
   Hardware pins (Melon / MOD2 PCB)
   ------------------------------------------------------------------ */
#define PIN_BUTTON      6       // Tactile switch (active LOW, internal pullup)
#define PIN_WS2812B     5       // WS2812B data line
#define PIN_POT1        A0      // Potentiometer 1 (used for direct slot select)

/* ------------------------------------------------------------------
   Flash layout constants
   ------------------------------------------------------------------ */
#define FLASH_BASE          0x10000000
#define BOOTLOADER_SIZE     0x00020000  // 128 KB
#define SLOT_SIZE           0x0003C000  // 240 KB
#define NUM_SLOTS           8
#define VTOR_OFFSET         0x3000     // vector table sits after the OTA section

#define CONFIG_FLASH_ADDR   (FLASH_BASE + BOOTLOADER_SIZE - FLASH_SECTOR_SIZE)
#define CONFIG_FLASH_OFFSET (BOOTLOADER_SIZE - FLASH_SECTOR_SIZE)  // 0x1F000

#define CONFIG_MAGIC        0xA5

static const uint32_t SLOT_BASE[NUM_SLOTS] = {
    0x10020000,  // Slot 0 — Braids
    0x1005C000,  // Slot 1 — Kick
    0x10098000,  // Slot 2 — Clap
    0x100D4000,  // Slot 3 — Hi-Hat
    0x10110000,  // Slot 4 — VCO
    0x1014C000,  // Slot 5 — FM Drum
    0x10188000,  // Slot 6 — Claves
    0x101C4000,  // Slot 7 — MOD303
};

/* ------------------------------------------------------------------
   LED colors per slot (GRB order for WS2812B, but NeoPixel lib
   accepts RGB — it handles the conversion internally)
   ------------------------------------------------------------------ */
static const uint32_t SLOT_COLOR[NUM_SLOTS] = {
    0x0000FF,  // Slot 0 Braids  — Blue
    0xFF0000,  // Slot 1 Kick    — Red
    0xFF8000,  // Slot 2 Clap    — Orange
    0xFFFF00,  // Slot 3 Hi-Hat  — Yellow
    0x00FF00,  // Slot 4 VCO     — Green
    0x8000FF,  // Slot 5 FM Drum — Purple
    0x00FFFF,  // Slot 6 Claves  — Cyan
    0xFF00FF,  // Slot 7 MOD303  — Magenta
};

/* ------------------------------------------------------------------
   Boot hold timing (milliseconds)
   ------------------------------------------------------------------ */
#define BOOT_HOLD_MS        5000  // Hold button 5 s at boot to enter menu
#define BOOT_CHECK_INTERVAL 50    // Poll interval during boot hold detection

/* ------------------------------------------------------------------
   Menu timing
   ------------------------------------------------------------------ */
#define MENU_CONFIRM_TIMEOUT 5000  // 5 s inactivity → boot selected
#define LONG_PRESS_MS        800   // Long press to confirm

/* ------------------------------------------------------------------
   Globals
   ------------------------------------------------------------------ */
Adafruit_NeoPixel led(1, PIN_WS2812B, NEO_GRB + NEO_KHZ800);

/* ------------------------------------------------------------------
   Read active slot from flash config
   ------------------------------------------------------------------ */
uint8_t read_active_slot() {
    const uint8_t *cfg = (const uint8_t *)CONFIG_FLASH_ADDR;
    if (cfg[1] == CONFIG_MAGIC && cfg[0] < NUM_SLOTS) {
        return cfg[0];
    }
    return 0;  // Default to slot 0
}

/* ------------------------------------------------------------------
   Write active slot to flash config
   ------------------------------------------------------------------ */
void write_active_slot(uint8_t slot) {
    if (slot >= NUM_SLOTS) return;

    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    buf[0] = slot;
    buf[1] = CONFIG_MAGIC;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

/* ------------------------------------------------------------------
   Check whether a slot contains a valid firmware image.
   A valid ARM Cortex-M vector table has:
     word[0] = initial MSP  → must be in SRAM (0x20000000-0x20082000)
     word[1] = reset handler → must be in flash with thumb bit set
   ------------------------------------------------------------------ */
bool slot_is_valid(uint8_t slot) {
    if (slot >= NUM_SLOTS) return false;

    const uint32_t *vt = (const uint32_t *)(SLOT_BASE[slot] + VTOR_OFFSET);
    uint32_t msp   = vt[0];
    uint32_t reset = vt[1];

    if (msp < 0x20000000 || msp > 0x20082000) return false;
    if ((reset & 0xF0000000) != 0x10000000) return false;
    if ((reset & 1) == 0) return false;

    return true;
}

/* ------------------------------------------------------------------
   Jump to a firmware slot — does not return
   ------------------------------------------------------------------ */
__attribute__((noreturn))
void jump_to_slot(uint8_t slot) {
    uint32_t vtor_addr = SLOT_BASE[slot] + VTOR_OFFSET;
    const uint32_t *vt = (const uint32_t *)vtor_addr;
    uint32_t msp   = vt[0];
    uint32_t reset = vt[1];

    // Turn off LED
    led.setPixelColor(0, 0);
    led.show();

    // Reset PIO state machines (NeoPixel uses PIO on RP2350)
    for (uint sm = 0; sm < 4; sm++) {
        pio_sm_set_enabled(pio0, sm, false);
        pio_sm_set_enabled(pio1, sm, false);
    }
    pio_clear_instruction_memory(pio0);
    pio_clear_instruction_memory(pio1);

    // Disable SysTick (Arduino millis timer)
    systick_hw->csr = 0;  // CTRL
    systick_hw->rvr = 0;  // LOAD
    systick_hw->cvr = 0;  // VAL

    // Disable all interrupts at the NVIC level
    for (int i = 0; i < 16; i++) {
        // NVIC ICER registers at 0xE000E180
        *(volatile uint32_t *)(0xE000E180 + i * 4) = 0xFFFFFFFF;
        // NVIC ICPR registers at 0xE000E280
        *(volatile uint32_t *)(0xE000E280 + i * 4) = 0xFFFFFFFF;
    }

    // Memory and instruction barriers
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb");

    // Set vector table to slot
    scb_hw->vtor = vtor_addr;

    // Set MSP and jump to reset handler
    __asm volatile(
        "msr msp, %0    \n"  // Set main stack pointer
        "bx  %1         \n"  // Branch to reset handler
        :
        : "r"(msp), "r"(reset)
        : "memory"
    );

    __builtin_unreachable();
}

/* ------------------------------------------------------------------
   Button helpers (active LOW with pullup)
   ------------------------------------------------------------------ */
bool button_pressed() {
    return digitalRead(PIN_BUTTON) == LOW;
}

bool button_released() {
    return digitalRead(PIN_BUTTON) == HIGH;
}

/* ------------------------------------------------------------------
   Detect 5-second button hold at startup.
   Returns true if hold detected.
   ------------------------------------------------------------------ */
void set_led_color(uint32_t color, uint8_t brightness = 255);

bool detect_boot_hold() {
    if (!button_pressed()) return false;

    uint32_t start = millis();
    while (button_pressed()) {
        uint32_t held = millis() - start;
        if (held >= BOOT_HOLD_MS) return true;
        uint8_t pulse = (held / 300) % 2 ? 60 : 20;
        set_led_color(0xFFFFFF, pulse);
        delay(BOOT_CHECK_INTERVAL);
    }
    return false;
}

/* ------------------------------------------------------------------
   Set LED to a slot color with optional brightness scaling
   ------------------------------------------------------------------ */
void set_led_color(uint32_t color, uint8_t brightness) {
    led.setBrightness(brightness);
    led.setPixelColor(0, color);
    led.show();
}

void set_led_off() {
    led.setPixelColor(0, 0);
    led.show();
}

void flash_led_white(int times, int period_ms) {
    for (int i = 0; i < times; i++) {
        set_led_color(0xFFFFFF, 255);
        delay(period_ms / 2);
        set_led_off();
        delay(period_ms / 2);
    }
}

/* ------------------------------------------------------------------
   Slot selection menu
   Returns the selected slot index.
   ------------------------------------------------------------------ */
uint8_t slot_menu(uint8_t current_slot) {
    uint8_t selected = current_slot;
    uint32_t last_activity = millis();

    // Record pot position at menu entry; only accept pot input after
    // it has moved significantly (deadband prevents ADC noise jitter).
    int pot_anchor = analogRead(PIN_POT1);
    bool pot_active = false;
    const int POT_DEADBAND = 64;  // ~6% of 10-bit range

    // Initial LED: blink current slot color to show menu entry
    for (int i = 0; i < 3; i++) {
        set_led_color(SLOT_COLOR[selected], 255);
        delay(100);
        set_led_off();
        delay(100);
    }
    set_led_color(SLOT_COLOR[selected], 255);

    while (true) {
        // Check timeout
        if (millis() - last_activity > MENU_CONFIRM_TIMEOUT) {
            break;  // Auto-confirm on timeout
        }

        // Check button press
        if (button_pressed()) {
            delay(20);  // debounce
            if (!button_pressed()) continue;

            uint32_t press_start = millis();

            // Wait for release to distinguish short/long press
            while (button_pressed()) {
                // Long press: confirm immediately
                if (millis() - press_start > LONG_PRESS_MS) {
                    goto confirm;
                }
                delay(10);
            }

            // Short press: cycle to next valid slot
            last_activity = millis();
            uint8_t start = selected;
            do {
                selected = (selected + 1) % NUM_SLOTS;
            } while (!slot_is_valid(selected) && selected != start);

            set_led_color(SLOT_COLOR[selected], 255);
            // Re-anchor pot so it doesn't immediately override button selection
            pot_anchor = analogRead(PIN_POT1);
            pot_active = false;
        }

        // Pot 1 for direct slot jump — only after pot moves past deadband
        int pot_val = analogRead(PIN_POT1);
        if (!pot_active) {
            if (abs(pot_val - pot_anchor) > POT_DEADBAND) {
                pot_active = true;
            }
        }
        if (pot_active) {
            uint8_t pot_slot = (pot_val * NUM_SLOTS) / 1024;
            if (pot_slot >= NUM_SLOTS) pot_slot = NUM_SLOTS - 1;
            if (pot_slot != selected && slot_is_valid(pot_slot)) {
                selected = pot_slot;
                set_led_color(SLOT_COLOR[selected], 255);
                last_activity = millis();
            }
        }

        delay(20);
    }

confirm:
    // Flash white to confirm selection
    flash_led_white(3, 150);
    return selected;
}

/* ------------------------------------------------------------------
   Setup — runs once on power-on / reset
   ------------------------------------------------------------------ */
void setup() {
    // Init button
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // Init LED
    led.begin();
    set_led_off();

    // Read current active slot
    uint8_t active = read_active_slot();
    if (active >= NUM_SLOTS) active = 0;

    // Check if we got here via firmware button hold (watchdog scratch magic)
    bool menu_requested = (watchdog_hw->scratch[0] == 0xCAFEAFFE);
    watchdog_hw->scratch[0] = 0;

    set_led_color(SLOT_COLOR[active], 255);

    if (menu_requested || detect_boot_hold()) {
        // Enter slot selection menu
        uint8_t selected = slot_menu(active);

        if (selected != active) {
            write_active_slot(selected);
            active = selected;
        }
    }

    // Validate the active slot
    if (!slot_is_valid(active)) {
        // Try to find any valid slot
        bool found = false;
        for (uint8_t i = 0; i < NUM_SLOTS; i++) {
            if (slot_is_valid(i)) {
                active = i;
                write_active_slot(i);
                found = true;
                break;
            }
        }
        if (!found) {
            // No valid firmware — blink red indefinitely
            while (true) {
                set_led_color(0xFF0000, 255);
                delay(500);
                set_led_off();
                delay(500);
            }
        }
    }

    // Brief confirmation flash then jump
    set_led_color(SLOT_COLOR[active], 255);
    delay(50);

    jump_to_slot(active);
}

/* ------------------------------------------------------------------
   Loop — never reached (jump_to_slot does not return)
   ------------------------------------------------------------------ */
void loop() {
    // Should never get here
}
