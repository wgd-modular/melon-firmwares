/*
STRING MOUTH v0.2-test — HAGIWO MOD2 / XIAO RP2350

Triggered plucked-string physical-model voice with a body/formant colour stage.

Hardware:
POT1     A0       Pitch
POT2     A1       Decay / damping
POT3+CV  A2       Body / formant / material, CV-modulatable through the MOD2 CV path
IN1      GPIO7    Trigger input
IN2      GPIO0    Accent / harder excitation
BUTTON   GPIO6    Short press: voice model
OUT      GPIO1    Audio PWM output. Normal AC-coupled output path recommended.
LED      GPIO5    Legacy PWM LED or MELON RGB LED, selected at boot.

Models:
0 STRING   clean plucked string
1 MOUTH    vowel-ish body resonances
2 METAL    bright inharmonic body
3 BASS     round low string with longer body
4 GLASS    bright ringing resonator

CC0 1.0. Use, fork, mangle.
*/

#include <Arduino.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"

static const uint MOD2_AUDIO  = 1;
static const uint MOD2_IRQ    = 2;
static const uint MOD2_LED    = 5;
static const uint MOD2_BUTTON = 6;
static const uint MOD2_IN2    = 0;
static const uint MOD2_IN1    = 7;

static const float SYS_CLK = 150000000.0f;
static const int IRQ_WRAP = 4095;
static const float FS = SYS_CLK / (IRQ_WRAP + 1);       // 36,621.09 Hz
static const int PWM_WRAP = 1023;

static const int BUF_LEN = 2048;
static float g_delay[BUF_LEN];

static const uint8_t MODEL_STRING = 0;
static const uint8_t MODEL_MOUTH  = 1;
static const uint8_t MODEL_METAL  = 2;
static const uint8_t MODEL_BASS   = 3;
static const uint8_t MODEL_GLASS  = 4;
static const uint8_t MODEL_COUNT  = 5;

static const uint8_t LED_MODE_LEGACY = 0;
static const uint8_t LED_MODE_MELON = 1;
static const int EEPROM_SIZE = 64;
static const int EEPROM_MAGIC_ADDR = 0;
static const int EEPROM_MODEL_ADDR = 1;
static const int EEPROM_LED_MAGIC_ADDR = 48;
static const int EEPROM_LED_MODE_ADDR = 49;
static const uint8_t EEPROM_MAGIC = 0x53;     // 'S'
static const uint8_t EEPROM_LED_MAGIC = 0xB7;

static uint sliceAudio, sliceIRQ, sliceLED, chanLED;
static uint8_t g_led_mode = LED_MODE_MELON;
static Adafruit_NeoPixel g_rgb(1, MOD2_LED, NEO_GRB + NEO_KHZ800);
static uint32_t g_last_rgb_ms = 0;
static uint32_t g_last_rgb_color = 0xFFFFFFFFu;

volatile int g_delay_samples = 240;
volatile float g_damp = 0.992f;
volatile float g_body = 0.35f;
volatile float g_level = 0.0f;
volatile uint8_t g_model = MODEL_STRING;
volatile bool g_trigger_req = false;
volatile bool g_active = false;
volatile bool g_accent = false;
volatile bool g_note_accent = false;
volatile uint32_t g_rng = 0x51F00D5u;

static bool g_last_trig = false;
static bool btn_last_raw = true;
static bool btn_stable = true;
static uint32_t btn_changed_ms = 0;
static uint32_t btn_down_ms = 0;
static uint32_t g_model_blink_start = 0;
static uint8_t g_model_blink_count = 0;
static uint32_t g_save_until_ms = 0;

static float bp1_low = 0.0f, bp1_band = 0.0f;
static float bp2_low = 0.0f, bp2_band = 0.0f;
static float tone_lp = 0.0f;
volatile float g_bp1_f = 0.13f;
volatile float g_bp2_f = 0.21f;
volatile float g_bp_q = 0.65f;

static inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static inline float smooth01(float x) {
  x = clampf(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

static inline float body_from_a2(float raw) {
  /* POT3 shares A2 with the CV input on MOD2. On this analogue mixer the pot is
     more useful as an offset/range shaper than as a full-range absolute ADC
     control, so fold the A2 voltage into a repeated musical morph. This makes
     the physical pot audible even when nothing is patched to CV, while an LFO
     on CV still sweeps the same body/tone parameter. */
  float x = clampf(raw, 0.0f, 1.0f) * 4.0f;
  x = x - floorf(x);
  float tri = (x < 0.5f) ? (x * 2.0f) : (2.0f - x * 2.0f);
  return smooth01(tri);
}

static inline uint32_t rng_next() {
  g_rng = g_rng * 1664525u + 1013904223u;
  return g_rng;
}

static inline float noise_bipolar() {
  return ((int32_t)(rng_next() >> 9) - 4194304) * (1.0f / 4194304.0f);
}

static inline float pot_norm(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(pin);
  return (float)(sum >> 3) * (1.0f / 4095.0f);
}

static inline uint16_t audio_to_pwm(float x) {
  x = clampf(x, -1.0f, 1.0f);
  return (uint16_t)(x * 430.0f + 512.0f);
}

static void save_state() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  EEPROM.write(EEPROM_MODEL_ADDR, g_model);
  EEPROM.commit();
  g_save_until_ms = millis() + 450;
}

static void load_state() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC) {
    uint8_t m = EEPROM.read(EEPROM_MODEL_ADDR);
    if (m < MODEL_COUNT) g_model = m;
  }
  g_led_mode = LED_MODE_MELON;
}

static void save_led_mode() {
  EEPROM.write(EEPROM_LED_MAGIC_ADDR, EEPROM_LED_MAGIC);
  EEPROM.write(EEPROM_LED_MODE_ADDR, g_led_mode);
  EEPROM.commit();
}

static uint32_t model_color() {
  switch (g_model) {
    case MODEL_MOUTH: return 0xFF40A0;
    case MODEL_METAL: return 0x80C0FF;
    case MODEL_BASS: return 0x30FF70;
    case MODEL_GLASS: return 0xD0F0FF;
    default: return 0xFFB000;
  }
}

static void write_led(uint16_t level, uint32_t color) {
  level = (level > 1023) ? 1023 : level;
  if (g_led_mode == LED_MODE_LEGACY) {
    pwm_set_chan_level(sliceLED, chanLED, level);
    return;
  }

  uint32_t now = millis();
  uint8_t brightness = (uint8_t)((uint32_t)level * 160u / 1023u);
  uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
  uint8_t g = ((color >> 8) & 0xFF) * brightness / 255;
  uint8_t b = (color & 0xFF) * brightness / 255;
  uint32_t scaled = g_rgb.Color(r, g, b);
  if (scaled == g_last_rgb_color && (now - g_last_rgb_ms) < 45) return;
  if ((now - g_last_rgb_ms) < 20) return;
  g_rgb.setPixelColor(0, scaled);
  g_rgb.show();
  g_last_rgb_color = scaled;
  g_last_rgb_ms = now;
}

static void boot_led_menu() {
  if (digitalRead(MOD2_BUTTON) != LOW) return;
  delay(800);
  if (digitalRead(MOD2_BUTTON) != LOW) return;

  uint8_t selected = g_led_mode;
  uint32_t start = millis();
  while (digitalRead(MOD2_BUTTON) == LOW) {
    selected = (analogRead(A1) < 2048) ? LED_MODE_LEGACY : LED_MODE_MELON;
    uint8_t pulse = (uint8_t)(32 + ((millis() / 6) & 0x7F));
    if (selected == LED_MODE_LEGACY) {
      gpio_set_function(MOD2_LED, GPIO_FUNC_PWM);
      sliceLED = pwm_gpio_to_slice_num(MOD2_LED);
      chanLED = pwm_gpio_to_channel(MOD2_LED);
      pwm_set_wrap(sliceLED, 1023);
      pwm_set_enabled(sliceLED, true);
      pwm_set_chan_level(sliceLED, chanLED, (uint16_t)pulse * 8);
    } else {
      g_rgb.begin();
      g_rgb.setPixelColor(0, g_rgb.Color(pulse, 0, 180 - pulse));
      g_rgb.show();
    }
    delay(10);
    if (millis() - start > 10000) break;
  }

  g_led_mode = selected;
  save_led_mode();
  if (g_led_mode == LED_MODE_MELON) {
    g_rgb.begin();
    g_rgb.setPixelColor(0, g_rgb.Color(0, 80, 255));
    g_rgb.show();
    delay(180);
    g_rgb.clear();
    g_rgb.show();
  }
}

void __not_in_flash_func(audio_isr)() {
  static int idx = 0;
  pwm_clear_irq(sliceIRQ);

  if (g_trigger_req) {
    g_trigger_req = false;
    idx = 0;
    int n = g_delay_samples;
    if (n < 16) n = 16;
    if (n > BUF_LEN - 2) n = BUF_LEN - 2;

    bool accented = g_note_accent;
    float accent = accented ? 1.65f : 0.88f;
    float body = g_body;
    uint8_t model = g_model;
    for (int i = 0; i < n; i++) {
      float t = (float)i / (float)n;
      float burst = noise_bipolar();
      if (model == MODEL_MOUTH) {
        burst = burst * (0.65f + body * 0.55f) + sinf(t * 6.2831853f) * 0.18f;
      } else if (model == MODEL_METAL) {
        float alt = ((i & 1) ? 1.0f : -1.0f) * (0.08f + body * 0.16f);
        burst = burst * (0.92f - body * 0.18f) + alt;
      } else if (model == MODEL_BASS) {
        burst = burst * (0.40f + body * 0.20f) + sinf(t * 6.2831853f) * 0.42f;
      } else if (model == MODEL_GLASS) {
        float partial = sinf(t * 6.2831853f * (2.0f + body * 3.0f)) * (0.20f + body * 0.25f);
        burst = burst * (0.58f + body * 0.20f) + partial;
      }
      if (accented) {
        float click = ((i & 1) ? 1.0f : -1.0f) * (0.18f + body * 0.10f) * (1.0f - t);
        burst = burst * 1.18f + click;
      }
      float attack_taper = 1.0f - t * 0.18f;
      g_delay[i] = clampf(burst * accent * attack_taper, -1.0f, 1.0f);
    }
    for (int i = n; i < BUF_LEN; i++) g_delay[i] = 0.0f;
    bp1_low = bp1_band = bp2_low = bp2_band = tone_lp = 0.0f;
    g_level = 1.0f;
    g_active = true;
  }

  if (!g_active) {
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_WRAP / 2);
    return;
  }

  int n = g_delay_samples;
  if (n < 16) n = 16;
  if (n > BUF_LEN - 2) n = BUF_LEN - 2;
  if (idx >= n) idx = 0;

  int next_i = idx + 1;
  if (next_i >= n) next_i = 0;
  float x = g_delay[idx];
  float next = g_delay[next_i];

  float damp = g_damp;
  float body = g_body;
  uint8_t model = g_model;
  if (g_note_accent) damp *= 1.00035f;
  if (model == MODEL_METAL) damp *= 0.998f - body * 0.006f;
  else if (model == MODEL_BASS) damp *= 0.9994f;
  else if (model == MODEL_GLASS) damp *= 0.9975f - body * 0.004f;

  float rec = (x + next) * 0.5f * damp;
  if (model == MODEL_MOUTH) rec += (bp1_band + bp2_band) * (0.030f + body * 0.055f);
  else if (model == MODEL_METAL) rec = rec - next * (0.025f + body * 0.075f);
  else if (model == MODEL_BASS) rec += bp1_band * (0.012f + body * 0.020f);
  else if (model == MODEL_GLASS) rec = rec - next * (0.010f + body * 0.045f) + bp2_band * (0.018f + body * 0.035f);
  rec = clampf(rec, -1.0f, 1.0f);
  g_delay[idx] = rec;
  idx++;

  float out = x;
  float f1 = g_bp1_f;
  float f2 = g_bp2_f;
  float q = g_bp_q;
  bp1_low += f1 * bp1_band;
  float hp1 = out - bp1_low - q * bp1_band;
  bp1_band += f1 * hp1;
  bp2_low += f2 * bp2_band;
  float hp2 = out - bp2_low - q * bp2_band;
  bp2_band += f2 * hp2;

  if (model == MODEL_MOUTH) {
    float dry = 0.62f - body * 0.50f;
    out = out * dry + bp1_band * (0.55f + body * 2.35f) + bp2_band * (0.25f + body * 1.45f);
  } else if (model == MODEL_METAL) {
    out = out * (0.76f + body * 0.46f) + (bp2_band - bp1_band) * (0.42f + body * 1.70f);
  } else if (model == MODEL_BASS) {
    out = out * (0.80f - body * 0.25f) + bp1_band * (0.80f + body * 1.35f);
  } else if (model == MODEL_GLASS) {
    out = out * (0.55f + body * 0.20f) + bp1_band * (0.35f + body * 0.70f) + bp2_band * (0.95f + body * 1.80f);
  } else {
    out = out * (0.96f - body * 0.42f) + bp1_band * (body * 1.15f);
  }

  float tone_alpha = 0.025f + body * 0.72f;
  tone_lp += (out - tone_lp) * tone_alpha;
  float bright = out - tone_lp;
  if (model == MODEL_STRING) {
    out = tone_lp * (1.05f - body * 0.20f) + bright * (0.12f + body * 1.45f);
  } else if (model == MODEL_MOUTH) {
    out = tone_lp * (1.15f - body * 0.55f) + bright * (0.05f + body * 0.75f);
  } else if (model == MODEL_BASS) {
    out = tone_lp * (1.25f - body * 0.25f) + bright * (0.03f + body * 0.35f);
  } else if (model == MODEL_GLASS) {
    out = tone_lp * (0.35f + body * 0.10f) + bright * (1.35f + body * 1.65f);
  } else {
    out = tone_lp * (0.45f - body * 0.20f) + bright * (1.10f + body * 2.20f);
  }

  float ax = out < 0.0f ? -out : out;
  if (ax > 0.82f) {
    float over = ax - 0.82f;
    float lim = 0.82f + over / (1.0f + over * 3.0f);
    out = out < 0.0f ? -lim : lim;
  }

  float lev = g_level;
  float abs_out = out < 0.0f ? -out : out;
  lev += (abs_out - lev) * ((abs_out > lev) ? 0.06f : 0.0008f);
  g_level = lev;

  if (lev < 0.0009f) {
    for (int i = 0; i < BUF_LEN; i++) g_delay[i] = 0.0f;
    bp1_low = bp1_band = bp2_low = bp2_band = tone_lp = 0.0f;
    g_active = false;
    g_level = 0.0f;
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_WRAP / 2);
    return;
  }

  float out_gain = g_note_accent ? 1.34f : 1.18f;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, audio_to_pwm(out * out_gain));
}

static void setup_pwm() {
  gpio_set_function(MOD2_AUDIO, GPIO_FUNC_PWM);
  sliceAudio = pwm_gpio_to_slice_num(MOD2_AUDIO);
  pwm_set_clkdiv(sliceAudio, 1.0f);
  pwm_set_wrap(sliceAudio, PWM_WRAP);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_WRAP / 2);
  pwm_set_enabled(sliceAudio, true);

  gpio_set_function(MOD2_IRQ, GPIO_FUNC_PWM);
  sliceIRQ = pwm_gpio_to_slice_num(MOD2_IRQ);
  pwm_clear_irq(sliceIRQ);
  pwm_set_irq_enabled(sliceIRQ, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_isr);
  irq_set_priority(PWM_IRQ_WRAP, 0x00);
  irq_set_enabled(PWM_IRQ_WRAP, true);
  pwm_set_clkdiv(sliceIRQ, 1.0f);
  pwm_set_wrap(sliceIRQ, IRQ_WRAP);
  pwm_set_chan_level(sliceIRQ, PWM_CHAN_A, IRQ_WRAP / 2);
  pwm_set_enabled(sliceIRQ, true);
}

static void setup_led() {
  if (g_led_mode == LED_MODE_LEGACY) {
    gpio_set_function(MOD2_LED, GPIO_FUNC_PWM);
    sliceLED = pwm_gpio_to_slice_num(MOD2_LED);
    chanLED = pwm_gpio_to_channel(MOD2_LED);
    pwm_set_clkdiv(sliceLED, 8.0f);
    pwm_set_wrap(sliceLED, 1023);
    pwm_set_chan_level(sliceLED, chanLED, 0);
    pwm_set_enabled(sliceLED, true);
  } else {
    g_rgb.begin();
    g_rgb.setBrightness(255);
    g_rgb.clear();
    g_rgb.show();
  }
}

static void announce_model() {
  g_model_blink_start = millis();
  g_model_blink_count = g_model + 1;
}

static void update_controls() {
  float p1 = smooth01(pot_norm(A0));
  float p2 = smooth01(pot_norm(A1));
  float body = body_from_a2(pot_norm(A2));

  float freq = 36.0f * powf(1500.0f / 36.0f, p1);
  int delay = (int)(FS / freq + 0.5f);
  if (delay < 16) delay = 16;
  if (delay > BUF_LEN - 2) delay = BUF_LEN - 2;
  g_delay_samples = delay;

  float p2_long = p2 * p2;
  float damp = 0.900f + p2_long * 0.0986f;
  if (g_model == MODEL_MOUTH) damp -= 0.006f * body * (1.0f - p2_long * 0.65f);
  if (g_model == MODEL_METAL) damp -= (0.006f + 0.006f * body) * (1.0f - p2_long * 0.85f);
  if (g_model == MODEL_BASS) damp += 0.0008f + p2_long * 0.0004f;
  if (g_model == MODEL_GLASS) damp -= (0.002f + 0.004f * body) * (1.0f - p2_long * 0.60f);
  g_damp = clampf(damp, 0.84f, 0.9989f);
  g_body = body;

  float f1 = 260.0f + body * 2400.0f;
  float f2 = 820.0f + body * 5600.0f;
  if (g_model == MODEL_METAL) {
    f1 = 780.0f + body * 6200.0f;
    f2 = 1900.0f + body * 9800.0f;
  } else if (g_model == MODEL_BASS) {
    f1 = 120.0f + body * 620.0f;
    f2 = 420.0f + body * 1450.0f;
  } else if (g_model == MODEL_GLASS) {
    f1 = 1600.0f + body * 5200.0f;
    f2 = 3600.0f + body * 9400.0f;
  }
  g_bp1_f = clampf(2.0f * sinf((float)M_PI * f1 / FS), 0.01f, 0.95f);
  g_bp2_f = clampf(2.0f * sinf((float)M_PI * f2 / FS), 0.01f, 0.95f);
  g_bp_q = 0.28f + body * 0.54f;
}

static void handle_button() {
  bool raw = digitalRead(MOD2_BUTTON);
  uint32_t now = millis();
  if (raw != btn_last_raw) {
    btn_last_raw = raw;
    btn_changed_ms = now;
  }
  if ((now - btn_changed_ms) < 18) return;
  if (raw == btn_stable) return;

  btn_stable = raw;
  if (!btn_stable) {
    btn_down_ms = now;
  } else {
    uint32_t held = now - btn_down_ms;
    if (held >= 700) {
      save_state();
    } else {
      g_model = (uint8_t)((g_model + 1) % MODEL_COUNT);
      announce_model();
    }
  }
}

static void update_led() {
  uint32_t now = millis();
  uint16_t level = (uint16_t)(clampf(g_level * 1.8f, 0.0f, 1.0f) * 1023.0f);
  if (now < g_save_until_ms) level = 1023;
  if (g_model_blink_count > 0) {
    const uint32_t on_ms = 90;
    const uint32_t off_ms = 130;
    const uint32_t period = on_ms + off_ms;
    uint32_t elapsed = now - g_model_blink_start;
    uint8_t idx = elapsed / period;
    if (idx >= g_model_blink_count) {
      g_model_blink_count = 0;
    } else {
      level = (elapsed % period) < on_ms ? 1023 : 0;
    }
  }
  write_led(level, model_color());
}

void setup() {
  analogReadResolution(12);
  pinMode(MOD2_BUTTON, INPUT_PULLUP);
  pinMode(MOD2_IN1, INPUT);
  pinMode(MOD2_IN2, INPUT);

  load_state();
  boot_led_menu();
  setup_pwm();
  setup_led();
  update_controls();
}

void loop() {
  update_controls();

  bool trig = digitalRead(MOD2_IN1) == HIGH;
  g_accent = digitalRead(MOD2_IN2) == HIGH;
  if (trig && !g_last_trig) {
    g_rng ^= micros();
    g_note_accent = g_accent;
    g_trigger_req = true;
  }
  g_last_trig = trig;

  handle_button();
  update_led();
  delay(1);
}
