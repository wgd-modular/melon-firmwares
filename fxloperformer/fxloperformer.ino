/*
FX LoPerformer Ver 1.0 - Dedicated FX Module for RP2350

  • External Audio Input via CV (A2)
  • Tape Delay with Clock Sync (IN1)
  • Room Reverb
  • Flanger
  • Bitcrusher / Downsampler
  • Master Bus: SSL-style Glue Compressor, DJ Filter, Master Dry/Wet, and DC Blocker
  • Pseudo-CV Gate Control on IN2 (GPIO0) — HIGH = 100% Wet Mix
  • Auto-Save: All parameters automatically persisted to EEPROM after 10s of inactivity

  --Pin assign---
POT1     A0       Current Page Parameter 1
POT2     A1       Current Page Parameter 2
IN1      GPIO7    Clock Sync Input for Tape Delay
IN2      GPIO0    Pseudo-CV Gate Control (HIGH = 100% Wet Mix)
CV       A2       External Audio Input
BUTTON   GPIO6    Navigation (Short=Next, Med=Prev)
OUT      GPIO1    10-bit PWM audio output @ 36.6 kHz
LED      GPIO5    WS2812B / Legacy PWM LED

CC0 1.0 Universal
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include <math.h>
#include <EEPROM.h>

bool system_hw_is_legacy = false;
uint32_t sysSliceLED = 0;

#include <Adafruit_NeoPixel.h>
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ADC Globals
volatile uint16_t global_a0_val = 2048;
volatile uint16_t global_a1_val = 2048;
volatile uint16_t global_a2_val = 2048;

const float AUDIO_FS = 36619.305f;
const float INV_FS = 1.0f / AUDIO_FS;

// Auto-save logic
uint32_t last_param_change_time = 0;
bool needs_save = false;

// Master DC blocker
float out_dc_block_z = 0.0f;
float out_dc_y = 0.0f;
float ext_dc_block_z = 0.0f; // Input DC blocker

#ifndef TWO_PI
#define TWO_PI 6.283185307179586f
#endif

// ── Exact Cytomic Simper SVF Resonator ──
struct Resonator {
    float g, R, a1, a2, a3;
    float ic1eq, ic2eq;
    float bp, lp, hp;
    
    void set(float f0, float Q, float fs) {
        if (f0 > fs * 0.48f) f0 = fs * 0.48f; 
        float wc = 2.0f * PI * f0 / fs;
        g = tanf(wc / 2.0f);
        R = 1.0f / Q;
        a1 = 1.0f / (1.0f + g * (g + R));
        a2 = g * a1;
        a3 = g * a2;
    }
    
    void process(float in) {
        float v3 = in - ic2eq;
        float v1 = a1 * ic1eq + a2 * v3; 
        float v2 = ic2eq + a2 * ic1eq + a3 * v3; 
        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;
        bp = v1;
        lp = v2;
        hp = in - R * v1 - v2;
    }
};

struct FXParams { float p1, p2; };
FXParams params[5]; // Page 0: Bitcrusher, Page 1: Flanger, Page 2: Tape Delay, Page 3: Reverb, Page 4: Master DJ
int currentPage = 4; // Start on Page 4 (Master DJ)

struct PotState { float locked_val; bool locked; };
PotState ps[2];

void checkPickup(int idx, float current, float &param) {
  if (ps[idx].locked) {
    if (fabs(current - ps[idx].locked_val) > 0.05f) {
      ps[idx].locked = false;
      if (fabs(current - param) > 0.005f) {
          param = current;
          last_param_change_time = millis();
          needs_save = true;
      }
    }
  } else {
    if (fabs(current - param) > 0.005f) {
        param = current;
        last_param_change_time = millis();
        needs_save = true;
    }
  }
}

// ── Reverb & FX ──
int fv_cl[8] = {911, 970, 1042, 1107, 1160, 1217, 1271, 1320};
int fv_al[4] = {184, 126, 95, 68};
float fv_cb[8][1350];
int fv_cp[8] = {0};
float fv_ab[4][200];
int fv_ap[4] = {0};
float fv_dmp[8] = {0};
float rev_room_size = 0.84f;
float rev_damp = 0.2f;
float rev_mix = 0.0f;

// ── Tape Delay ──
#define DELAY_SIZE 32768 // ~0.89 seconds
float delay_buf[DELAY_SIZE];
int delay_ptr = 0;
float target_delay_samples = 10000.0f;
float current_delay_samples = 10000.0f;
float delay_fb = 0.0f;
float delay_mix = 0.0f;

// ── Flanger ──
#define FLANGER_SIZE 1024
float flanger_buf[FLANGER_SIZE];
int flanger_ptr = 0;
float flanger_lfo_phase = 0.0f;
float flanger_rate = 0.0f;
float flanger_depth = 0.0f;
float flanger_fb = 0.0f;

// ── Bitcrusher / Downsampler ──
volatile float bc_steps = 65536.0f;
volatile float bc_inv_steps = 1.0f / 65536.0f;
volatile int ds_period = 1;
volatile int ds_counter = 0;
volatile float ds_held_val = 0.0f;

// Master Bus Compressor State
float comp_env = 0.0f;
float comp_thresh = 0.3f;
float comp_ratio = 4.0f;
float comp_attack, comp_release;

// Master DJ FX & Global Mix
Resonator master_filter;
float master_wet = 1.0f;

uint sliceAudio, sliceIRQ;

volatile uint32_t last_clock_time = 0;
volatile uint32_t clock_period_ms = 0;

void isr_gpio7() {
    // IN1 acts as a clock/sync input for the Tape Delay
    uint32_t now = millis();
    if (last_clock_time > 0) {
        uint32_t diff = now - last_clock_time;
        if (clock_period_ms == 0) clock_period_ms = diff;
        else clock_period_ms = (clock_period_ms * 3 + diff) / 4;
    }
    last_clock_time = now;
}

// Fast ISR-safe RNG to prevent deadlocks
volatile uint32_t fast_rand_seed = 12345;
uint32_t fast_rand() {
    fast_rand_seed = (1103515245 * fast_rand_seed + 12345);
    return fast_rand_seed;
}

void on_pwm_wrap() {
  pwm_clear_irq(sliceIRQ);
  float out = 0.0f;
  
  static uint32_t lcg = 1;
  lcg = lcg * 1664525 + 1013904223;
  float white = ((float)(lcg >> 8) * (1.0f / 8388608.0f) - 1.0f);

  // --- HARDWARE ADC READS ---
  static uint16_t adc_cycle = 0;
  uint16_t raw_a2 = 2048;
  
  // A2 is our audio input, sample it every tick
  raw_a2 = adc_read();
  global_a2_val = raw_a2;
  
  adc_cycle++;
  if (adc_cycle == 100) {
      adc_select_input(0); adc_read(); global_a0_val = adc_read();
      adc_select_input(2); adc_read(); // switch back to A2 immediately
  } else if (adc_cycle == 200) {
      adc_select_input(1); adc_read(); global_a1_val = adc_read();
      adc_select_input(2); adc_read(); // switch back to A2 immediately
      adc_cycle = 0;
  }

  // --- EXTERNAL AUDIO IN ---
  float raw = ((float)raw_a2 - 2047.5f) * (1.0f / 2048.0f);
  
  // One-pole DC blocker
  static float ext_dc_y = 0.0f;
  ext_dc_y = raw - ext_dc_block_z + 0.995f * ext_dc_y;
  ext_dc_block_z = raw;
  
  // 4-point Moving Average filter
  static float ma[4] = {0};
  static int ma_idx = 0;
  ma[ma_idx] = ext_dc_y;
  ma_idx = (ma_idx + 1) & 3;
  float ext_lp = (ma[0] + ma[1] + ma[2] + ma[3]) * 0.25f;
  
  // Noise Gate
  static float gate_env = 0.0f;
  float rect_in = fabsf(ext_lp);
  if (rect_in > gate_env) gate_env += (rect_in - gate_env) * 0.1f;   // fast attack
  else                 gate_env += (rect_in - gate_env) * 0.001f; // slow release
  
  float clean_sig = ext_lp;
  if (gate_env < 0.006f) {
      float fade = gate_env * 166.66f;
      clean_sig *= fade * fade; 
  }
  
  // --- BITCRUSHER / DOWNSAMPLER ---
  float bc_in = clean_sig;
  ds_counter++;
  if (ds_counter >= ds_period) {
      ds_counter = 0;
      ds_held_val = bc_in;
  }
  float bc_processed = ds_held_val;
  if (bc_steps < 65000.0f) {
      bc_processed = ((float)(int)(bc_processed * bc_steps + (bc_processed >= 0.0f ? 0.5f : -0.5f))) * bc_inv_steps;
  }

  // --- FLANGER ---
  flanger_lfo_phase += flanger_rate;
  if (flanger_lfo_phase >= 1.0f) flanger_lfo_phase -= 1.0f;
  
  // Sine LFO 0 to 1
  float lfo = (sinf(flanger_lfo_phase * TWO_PI) + 1.0f) * 0.5f;
  float fl_delay_samples = 10.0f + lfo * flanger_depth;
  
  float fl_read_pos = (float)flanger_ptr + (float)FLANGER_SIZE - fl_delay_samples;
  int fl_idx = (int)fl_read_pos;
  float fl_frac = fl_read_pos - fl_idx;
  float fl_out = flanger_buf[fl_idx & (FLANGER_SIZE-1)] * (1.0f - fl_frac) + 
                 flanger_buf[(fl_idx + 1) & (FLANGER_SIZE-1)] * fl_frac;
                 
  float fl_in = bc_processed + fl_out * flanger_fb;
  flanger_buf[flanger_ptr] = fl_in;
  flanger_ptr = (flanger_ptr + 1) & (FLANGER_SIZE-1);
  
  // Crossfade for flanger mix (up to 50/50 mix based on depth/fb macro)
  float fl_mix_amt = params[1].p2 > 0.01f ? 0.5f : 0.0f; 
  float flanger_mixed = bc_processed * (1.0f - fl_mix_amt) + (bc_processed + fl_out) * fl_mix_amt;

  // --- TAPE DELAY ---
  float input_bus = flanger_mixed;
  
  current_delay_samples += (target_delay_samples - current_delay_samples) * 0.0005f;
  
  float d_read_pos = (float)delay_ptr + (float)DELAY_SIZE - current_delay_samples;
  int d_idx = (int)d_read_pos;
  float d_frac = d_read_pos - d_idx;
  float d_out = delay_buf[d_idx & (DELAY_SIZE-1)] * (1.0f - d_frac) + delay_buf[(d_idx + 1) & (DELAY_SIZE-1)] * d_frac;
  
  static float d_dmp = 0.0f;
  d_dmp += (d_out - d_dmp) * 0.4f; 
  
  float d_in = input_bus + tanhf(d_dmp * delay_fb);
  delay_buf[delay_ptr] = d_in;
  delay_ptr = (delay_ptr + 1) & (DELAY_SIZE-1);
  
  float delay_mixed = input_bus + d_out * delay_mix;
  
  // --- ROOM REVERB (Freeverb) ---
  float dry = delay_mixed;
  float rv_out = 0.0f;
  
  for(int i=0; i<8; i++) {
      float delayed = fv_cb[i][fv_cp[i]];
      fv_dmp[i] = delayed * (1.0f - rev_damp) + fv_dmp[i] * rev_damp;
      fv_cb[i][fv_cp[i]] = dry + fv_dmp[i] * rev_room_size;
      fv_cp[i] = (fv_cp[i] + 1) % fv_cl[i];
      rv_out += delayed;
  }
  
  for(int i=0; i<4; i++) {
      float delayed = fv_ab[i][fv_ap[i]];
      float ap_out = -rv_out * 0.5f + delayed;
      fv_ab[i][fv_ap[i]] = rv_out + delayed * 0.5f;
      fv_ap[i] = (fv_ap[i] + 1) % fv_al[i];
      rv_out = ap_out;
  }
  
  float rev_mixed = dry + rv_out * 0.15f * rev_mix;

  // --- GLOBAL DRY/WET ---
  float fx_chain_out = rev_mixed;
  
  static float slew_wet = 1.0f;
  float target_wet = gpio_get(0) ? 1.0f : master_wet;
  slew_wet += (target_wet - slew_wet) * 0.05f; // Slew to prevent clicks (0.5ms time constant)
  float global_mixed = clean_sig * (1.0f - slew_wet) + fx_chain_out * slew_wet;
  
  // Master Bus Compressor
  float rect = fabsf(global_mixed);
  if (rect > comp_env) comp_env = comp_attack * (comp_env - rect) + rect;
  else                 comp_env = comp_release * (comp_env - rect) + rect;
  
  float gain = 1.0f;
  if (comp_env > comp_thresh) {
      gain = comp_thresh + (comp_env - comp_thresh) / comp_ratio;
      gain /= comp_env;
  }
  
  out = global_mixed * gain * 1.5f; 
  
  // Master DJ Filter
  float dj = params[4].p1; // Master DJ is Page 4
  master_filter.process(out);
  
  float mix_lp = 0.0f, mix_hp = 0.0f, mix_dry = 0.0f;
  if (dj < 0.45f) {
      mix_lp = 1.0f;
  } else if (dj < 0.5f) {
      mix_lp = (0.5f - dj) * 20.0f; 
      mix_dry = 1.0f - mix_lp;
  } else if (dj < 0.55f) {
      mix_hp = (dj - 0.5f) * 20.0f; 
      mix_dry = 1.0f - mix_hp;
  } else {
      mix_hp = 1.0f;
  }
  
  out = (master_filter.lp * mix_lp) + (master_filter.hp * mix_hp) + (out * mix_dry);
  
  // Master DC Blocker
  out_dc_y = out - out_dc_block_z + 0.995f * out_dc_y;
  out_dc_block_z = out;
  out = out_dc_y;

  // --- 1st-order Sigma-Delta Noise Shaper + TPDF Dither ---
  static float pwm_err = 0.0f;
  float PWM_MID = 511.5f;
  float target_val = PWM_MID + out * PWM_MID * 0.9f;
  
  float dither = (white * 0.5f); 
  float val_to_quantize = target_val + pwm_err + dither;
  
  int val_int = (int)val_to_quantize;
  if(val_int < 0) val_int = 0;
  if(val_int > 1023) val_int = 1023;
  
  pwm_err = val_to_quantize - (float)val_int;
  
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)val_int);
}

void updatePageLED() {
  if (system_hw_is_legacy) {
    return; // Legacy LED is updated dynamically in runLegacyLEDAnimation()
  }
  if      (currentPage == 0) led.setPixelColor(0, led.Color(255, 255, 0)); // 0: Yellow (Bitcrusher)
  else if (currentPage == 1) led.setPixelColor(0, led.Color(255, 0, 255)); // 1: Magenta (Flanger)
  else if (currentPage == 2) led.setPixelColor(0, led.Color(0, 255, 255)); // 2: Cyan (Tape Delay)
  else if (currentPage == 3) led.setPixelColor(0, led.Color(0, 255, 0));   // 3: Green (Reverb)
  else if (currentPage == 4) led.setPixelColor(0, led.Color(0, 0, 255));   // 4: Blue (Master DJ)
  else led.setPixelColor(0, led.Color(50, 50, 50)); // Fallback
  led.show();
}

void runLegacyLEDAnimation() {
  uint16_t pwm_val = 0;
  
  if (currentPage == 0) { // Bitcrusher
    // Jittery digital flicker proportional to crushing intensity
    float noise = (float)(fast_rand() & 255) / 255.0f;
    float intensity = (params[0].p1 + params[0].p2) * 0.5f;
    float brightness = 0.1f + noise * 0.7f * intensity;
    pwm_val = (uint16_t)(brightness * 4095.0f);
  }
  else if (currentPage == 1) { // Flanger
    // Pulse in sync with the flanger LFO
    float lfo = (sinf(flanger_lfo_phase * TWO_PI) + 1.0f) * 0.5f;
    float brightness = 0.05f + lfo * 0.75f;
    pwm_val = (uint16_t)(brightness * 4095.0f);
  }
  else if (currentPage == 2) { // Tape Delay
    // Blink at the rate of the current delay time
    float delay_ms = 1000.0f * (current_delay_samples / AUDIO_FS);
    if (delay_ms < 10.0f) delay_ms = 10.0f;
    uint32_t t = millis() % (uint32_t)delay_ms;
    pwm_val = (t < 80) ? 3500 : 100; // Bright flash, dim background
  }
  else if (currentPage == 3) { // Reverb
    // Slow pulsing breath
    float brightness = 0.4f + 0.35f * sinf(millis() * 0.002f);
    pwm_val = (uint16_t)(brightness * 4095.0f);
  }
  else if (currentPage == 4) { // Master DJ
    // Solid steady light to easily distinguish the Master page
    pwm_val = 3000;
  }
  
  pwm_set_chan_level(sysSliceLED, PWM_CHAN_B, pwm_val);
}

void runBootMenu() {
  EEPROM.begin(256);
  
  // Signature check to guarantee defaults on fresh flash or dirty EEPROM
  uint8_t signature = EEPROM.read(101);
  if (signature != 0x7C) {
    EEPROM.write(100, 0);    // Default: NeoPixel (0)
    EEPROM.write(101, 0x7C); // Store signature
    EEPROM.commit();
  }
  
  system_hw_is_legacy = (EEPROM.read(100) == 1);
  pinMode(6, INPUT_PULLUP);
  delay(50);
  
  // If button is not held down, bypass the boot menu
  if (digitalRead(6) == HIGH) {
    return;
  }
  
  // Enter boot menu configuration
  uint32_t startTime = millis();
  bool selectingLegacy = system_hw_is_legacy;
  bool lastLegacyState = !selectingLegacy; // Force initial hardware setup
  
  sysSliceLED = pwm_gpio_to_slice_num(5);
  
  while (digitalRead(6) == LOW || (millis() - startTime < 3000)) {
    // Read POT 2 (A1) from analogRead
    float p2 = analogRead(A1) / 1023.0f;
    
    // Left side selects Legacy LED, Right side selects NeoPixel
    selectingLegacy = (p2 <= 0.5f);
    
    if (selectingLegacy != lastLegacyState) {
      if (selectingLegacy) {
        gpio_set_function(5, GPIO_FUNC_PWM);
        pwm_set_wrap(sysSliceLED, 4095);
        pwm_set_enabled(sysSliceLED, true);
      } else {
        led.begin();
      }
      lastLegacyState = selectingLegacy;
    }
    
    int brightness = 0;
    uint32_t color = 0;
    
    if (selectingLegacy) {
      // Legacy: Slow pulse
      float pulse = 0.5f + 0.5f * sinf(millis() * 0.005f);
      brightness = (int)(pulse * 255.0f);
      pwm_set_chan_level(sysSliceLED, PWM_CHAN_B, brightness * 16);
    } else {
      // NeoPixel: Cycle colors to show RGB NeoPixel mode is selected
      uint32_t palette[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF};
      int idx = (millis() / 200) % 6;
      color = palette[idx];
      uint8_t r = (color >> 16) & 0xFF;
      uint8_t g = (color >> 8) & 0xFF;
      uint8_t b = color & 0xFF;
      led.setPixelColor(0, led.Color(r, g, b));
      led.show();
    }
    
    delay(10);
  }
  
  // Turn off output visual indicators after boot menu finishes
  if (selectingLegacy) {
    pwm_set_chan_level(sysSliceLED, PWM_CHAN_B, 0);
  } else {
    led.setPixelColor(0, 0);
    led.show();
    led.clear();
  }
  
  system_hw_is_legacy = selectingLegacy;
  EEPROM.write(100, system_hw_is_legacy ? 1 : 0);
  EEPROM.commit();
}

void updateFXParams() {
  // Bitcrusher / Downsampler (Page 0)
  float bc_knob = params[0].p1;
  if (bc_knob < 0.01f) {
      bc_steps = 65536.0f;
      bc_inv_steps = 1.0f / 65536.0f;
  } else {
      // Quadratic curve for a more responsive and musical control range
      float inv_k = 1.0f - bc_knob;
      float bits = 1.0f + inv_k * inv_k * 15.0f; // 16 bits down to 1 bit
      bc_steps = powf(2.0f, bits);
      bc_inv_steps = 1.0f / bc_steps;
  }
  float ds_knob = params[0].p2;
  ds_period = 1 + (int)(ds_knob * ds_knob * 63.0f); // 1 to 64 samples downsampling period

  // Flanger (Page 1)
  float rate_hz = 0.05f + params[1].p1 * 5.0f; // 0.05Hz to 5.05Hz
  flanger_rate = rate_hz * INV_FS;
  flanger_depth = params[1].p2 * 200.0f; // max ~5.5ms modulation depth
  flanger_fb = params[1].p2 * 0.90f; // up to 90% feedback for strong resonance

  // Tape Delay (Page 2)
  float d_time;
  bool clock_active = (clock_period_ms > 0 && clock_period_ms < 2000 && (millis() - last_clock_time < 2000));
  
  if (clock_active) {
      int div_idx = (int)(params[2].p1 * 6.99f);
      float mults[7] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
      d_time = (clock_period_ms * mults[div_idx]) / 1000.0f;
  } else {
      d_time = 0.02f + params[2].p1 * 0.85f; // 20ms to 870ms
  }
  
  target_delay_samples = d_time * AUDIO_FS;
  delay_fb = params[2].p2 * 0.95f; // up to self-oscillation
  delay_mix = params[2].p2 * 0.8f;

  // Reverb (Page 3)
  rev_room_size = 0.7f + params[3].p1 * 0.28f; // 0.7 to 0.98
  rev_mix = params[3].p2 * 1.5f;
  rev_damp = 0.4f;

  // Master DJ FX & Global Wet (Page 4)
  float dj = params[4].p1;
  if (dj < 0.5f) {
      float val = dj * 2.0f;
      float cutoff = 100.0f * powf(200.0f, val);
      master_filter.set(cutoff, 0.707f, AUDIO_FS);
  } else {
      float val = (dj - 0.5f) * 2.0f;
      float cutoff = 20.0f * powf(500.0f, val);
      master_filter.set(cutoff, 0.707f, AUDIO_FS);
  }
  
  master_wet = params[4].p2; 
}

void setup() {
  vreg_set_voltage(VREG_VOLTAGE_1_20); // Boost voltage for overclock stability
  delay(10);
  set_sys_clock_khz(250000, true);     // Overclock to 250MHz for massive DSP headroom

  // Run the hardware selection boot menu
  runBootMenu();

  adc_init();
  adc_gpio_init(26); // A0
  adc_gpio_init(27); // A1
  adc_gpio_init(28); // A2
  adc_select_input(2);

  pinMode(0, INPUT); // Initialize IN2 (GPIO0) as input for pseudo-CV

  comp_attack = expf(-1.0f / (0.005f * AUDIO_FS)); // 5ms punchy attack
  comp_release = expf(-1.0f / (0.100f * AUDIO_FS)); // 100ms release for pumping glue
  
  for(int i=0; i<FLANGER_SIZE; i++) flanger_buf[i] = 0.0f;

  EEPROM.begin(256);
  for (int i=0; i<5; i++) {
    EEPROM.get(i * sizeof(FXParams), params[i]);
  }
  
  for(int i=0; i<5; i++) {
    if (isnan(params[i].p1) || params[i].p1 < 0.0f || params[i].p1 > 1.0f) params[i].p1 = 0.5f;
    if (isnan(params[i].p2) || params[i].p2 < 0.0f || params[i].p2 > 1.0f) params[i].p2 = 0.5f;
  }

  ps[0].locked = false;
  ps[1].locked = false;

  updateFXParams();

  sysSliceLED = pwm_gpio_to_slice_num(5);
  if (system_hw_is_legacy) {
    gpio_set_function(5, GPIO_FUNC_PWM);
    pwm_set_wrap(sysSliceLED, 4095);
    pwm_set_enabled(sysSliceLED, true);
  } else {
    led.begin(); led.setBrightness(180); updatePageLED();
  }

  pinMode(1, OUTPUT); gpio_set_function(1, GPIO_FUNC_PWM);
  sliceAudio = pwm_gpio_to_slice_num(1);
  pwm_set_clkdiv(sliceAudio, 1); pwm_set_wrap(sliceAudio, 1023); pwm_set_enabled(sliceAudio, true);

  pinMode(2, OUTPUT); gpio_set_function(2, GPIO_FUNC_PWM);
  sliceIRQ = pwm_gpio_to_slice_num(2);
  pwm_set_clkdiv(sliceIRQ, 1); pwm_set_wrap(sliceIRQ, 6826); pwm_set_enabled(sliceIRQ, true);
  pwm_clear_irq(sliceIRQ); pwm_set_irq_enabled(sliceIRQ, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  pinMode(7, INPUT); attachInterrupt(digitalPinToInterrupt(7), isr_gpio7, RISING);
  
  pinMode(6, INPUT_PULLUP);
}

void loop() {
  static bool currBtn = HIGH;
  static uint32_t last_debounce = 0;
  bool rawBtn = digitalRead(6);
  
  static uint32_t btn_press_start = 0;

  if (rawBtn != currBtn && (millis() - last_debounce > 20)) {
      currBtn = rawBtn;
      last_debounce = millis();
      
      if (currBtn == LOW) { // Button physically pressed
          btn_press_start = millis();
      } else { // Button physically released
          uint32_t duration = millis() - btn_press_start;
          
          // Save pending changes immediately on page change
          if (needs_save) {
              for(int i=0; i<5; i++) EEPROM.put(i * sizeof(FXParams), params[i]);
              EEPROM.commit();
              needs_save = false;
          }

          int max_pages = 5;
          if (duration > 350) {
              // Medium Press (Go Back)
              currentPage = (currentPage - 1 + max_pages) % max_pages;
          } else {
              // Short Press (Go Forward)
              currentPage = (currentPage + 1) % max_pages;
          }
          
          float p1_current = global_a0_val / 4095.0f;
          float p2_current = global_a1_val / 4095.0f;
          ps[0].locked_val = p1_current; ps[0].locked = true;
          ps[1].locked_val = p2_current; ps[1].locked = true;
          
          updatePageLED();
      }
  }
  
  // Auto-save check (10 seconds of inactivity)
  if (needs_save && (millis() - last_param_change_time > 10000)) {
      for(int i=0; i<5; i++) EEPROM.put(i * sizeof(FXParams), params[i]);
      EEPROM.commit();
      needs_save = false;
  }

  float p1_val = global_a0_val / 4095.0f;
  float p2_val = global_a1_val / 4095.0f;
  checkPickup(0, p1_val, params[currentPage].p1);
  checkPickup(1, p2_val, params[currentPage].p2);

  updateFXParams();

  if (system_hw_is_legacy) {
      runLegacyLEDAnimation();
  }

  delay(10);
}
