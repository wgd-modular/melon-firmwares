#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$PROJECT_DIR")"
MOD2_DIR="${MOD2_DIR:-$REPO_ROOT}"
HEADER="$PROJECT_DIR/melon_led.h"

SLOT_COLORS_HEX=(0x0000FF 0xFF0000 0xFF8000 0xFFFF00 0x00FF00 0x8000FF 0x00FFFF 0xFF00FF)
SLOT_DIRS=(braids kick clap hihat vco fm_drum claves mod303)

for i in "${!SLOT_DIRS[@]}"; do
    dir="${SLOT_DIRS[$i]}"
    color="${SLOT_COLORS_HEX[$i]}"
    fw_dir="$MOD2_DIR/$dir"
    cp "$HEADER" "$fw_dir/melon_led.h"
done

patch_kick() {
    local f="$MOD2_DIR/kick/kick.ino"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\n#define MY_COLOR 0xFF0000' "$f"
    sed -i 's/pinMode(5,\s*OUTPUT);.*/melon_led_begin(); melon_led_slot_init(MY_COLOR);/' "$f"
    sed -i 's/digitalWrite(5, selectMode ? HIGH : LOW);/if (selectMode) melon_led_color32(MY_COLOR); else melon_led_dim();/' "$f"
}

patch_fm_drum() {
    local f="$MOD2_DIR/fm_drum/fm_drum.ino"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\n#define MY_COLOR 0x8000FF' "$f"
    sed -i 's/pinMode(5,\s*OUTPUT);.*/melon_led_begin(); melon_led_slot_init(MY_COLOR);/' "$f"
    sed -i 's/digitalWrite(5,.*HIGH.*/melon_led_color32(MY_COLOR);/' "$f"
    sed -i 's/digitalWrite(5,.*LOW.*/melon_led_dim();/' "$f"
    sed -i 's/digitalWrite(5, editMode);/if (editMode) melon_led_color32(MY_COLOR); else melon_led_dim();/' "$f"
}

patch_envelope_led() {
    local f="$1"
    local color="$2"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\nstatic uint32_t _my_color = '"$color"';\nvolatile uint8_t melon_env_brightness = 0;' "$f"
    sed -i '/LED PWM channel/,/Timer PWM channel/{/LED PWM channel/d; /Timer PWM channel/!d}' "$f"
    sed -i '/gpio_set_function(5/d' "$f"
    sed -i '/pwm_gpio_to_slice_num(5/d' "$f"
    sed -i '/pwm_set_clkdiv(sliceLED/d' "$f"
    sed -i '/pwm_set_wrap(sliceLED/d' "$f"
    sed -i '/pwm_set_enabled(sliceLED/d' "$f"
    sed -i '/pwm_set_chan_level(sliceLED/d' "$f"
    sed -i 's/, sliceLED//g; s/sliceLED, //g; s/sliceLED//g' "$f"
    sed -i 's/pinMode(5,\s*OUTPUT);.*/melon_led_begin(); melon_led_slot_init('"$color"');/' "$f"
}

patch_vco() {
    local f="$MOD2_DIR/vco/vco.ino"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\n#define MY_COLOR 0x00FF00' "$f"
    sed -i '/void setup/,/^}/ s|pinMode(6,  INPUT_PULLUP);|pinMode(6, INPUT_PULLUP);\n  melon_led_begin();\n  melon_led_slot_init(MY_COLOR);|' "$f"
}

patch_braids() {
    local f="$MOD2_DIR/braids/braids.ino"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\n#define MY_COLOR 0x0000FF' "$f"
    sed -i '/void setup/,/^}/ {
        /Serial.begin/a\  melon_led_begin();\n  melon_led_slot_init(MY_COLOR);
    }' "$f"
}

patch_mod303() {
    local f="$MOD2_DIR/mod303/mod303.ino"
    if grep -q "melon_led.h" "$f"; then return; fi
    sed -i '/#include <Arduino.h>/a #include "melon_led.h"\n#define MY_COLOR 0xFF00FF' "$f"
    sed -i 's/pinMode(MOD2_PIN_LED, OUTPUT);/melon_led_begin(); melon_led_slot_init(MY_COLOR);/' "$f"
    sed -i 's/digitalWrite(MOD2_PIN_LED, HIGH);/melon_led_color32(MY_COLOR);/' "$f"
    sed -i 's/digitalWrite(MOD2_PIN_LED, LOW);/melon_led_dim();/' "$f"
}

inject_triple_click() {
    local f="$1"
    if grep -q "melon_check_bootloader_hold" "$f"; then return; fi
    sed -i '/^void loop/,/^}/ {
        /delay(10\|delay(20/{
            a\  melon_check_bootloader_hold();
            b
        }
    }' "$f"
    if ! grep -q "melon_check_bootloader_hold" "$f"; then
        sed -i '/^void loop/,/^}/ {
            /^}/i\  melon_check_bootloader_hold();
        }' "$f"
    fi
}

echo "Patching firmwares for WS2812B LED..."
patch_braids
echo "  braids: patched"
patch_kick
echo "  kick: patched"
patch_envelope_led "$MOD2_DIR/clap/clap.ino" "0xFF8000"
echo "  clap: patched"
patch_envelope_led "$MOD2_DIR/hihat/hihat.ino" "0xFFFF00"
echo "  hihat: patched"
patch_vco
echo "  vco: patched"
patch_fm_drum
echo "  fm_drum: patched"
patch_envelope_led "$MOD2_DIR/claves/claves.ino" "0x00FFFF"
echo "  claves: patched"
patch_mod303
echo "  mod303: patched"

echo "Injecting triple-click → bootloader for all firmwares..."
for dir in "${SLOT_DIRS[@]}"; do
    inject_triple_click "$MOD2_DIR/$dir/$dir.ino"
    echo "  $dir: triple-click injected"
done
echo "Done."
