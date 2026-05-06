/*
DRUMS Ver 1.2 - Generative Drum Synthesizer for RP2350

  • 15 Factory Kits (Synthesized & Sampled) + Optional Master User Kit
  • 3-Voice Architecture: Independent Kick, Snare, and Hihat synthesis/playback
  • Grids Sequencer: Probabilistic X/Y pattern generation inspired by Mutable Instruments
  • Master Bus: SSL-style Glue Compressor, DJ Filter, Tanh Saturation, and DC Blocker
  • Auto-Save: All parameters automatically persisted to EEPROM after 5s of inactivity

  --Pin assign---
POT1     A0       Current Page Parameter 1 (Pickup logic enabled)
POT2     A1       Current Page Parameter 2 (Pickup logic enabled)
IN1      GPIO7    Trigger 1 (Kick) / Grids Clock Input
IN2      GPIO0    Trigger 2 (Snare) / Grids Fill Trigger
CV       A2       Trigger 3 (Hihat) / Bassline 1V/Oct / Ext Audio Input
BUTTON   GPIO6    Navigation (Short=Next, Med=Prev, Long=Toggle Grids)
OUT      GPIO1    10-bit PWM audio output @ 36.6 kHz
LED      GPIO5    WS2812B — Color = Page, Flash = Trigger

CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial purposes, all without asking permission.
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/adc.h"
#include <math.h>
#include <EEPROM.h>

// ADC Globals
volatile uint16_t global_a0_val = 2048;
volatile uint16_t global_a1_val = 2048;
volatile uint16_t global_a2_val = 2048;
#include "drum_samples.h"
#define HAS_USER_KIT (USER_K_LEN > 1)

#include <Adafruit_NeoPixel.h>
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

bool ledTrigOn = false;
uint32_t ledTrigOffAt = 0;
volatile bool ledTrigReq = false;
volatile uint8_t grids_fill_steps = 0;

const float AUDIO_FS = 36619.305f;
const float INV_FS = 1.0f / AUDIO_FS;

// Auto-save logic
uint32_t last_param_change_time = 0;
bool needs_save = false;
float out_dc_block_z = 0.0f;
float out_dc_y = 0.0f;

#ifndef TWO_PI
#define TWO_PI 6.283185307179586f
#endif

// ── PolyBLEP Anti-Aliasing ──
float poly_blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

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

struct DrumParams { float p1, p2; };
DrumParams params[8]; // 8 pages now!
int currentPage = 0;

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

// ── Kick ──
volatile float kick_env = 0.0f;
volatile float kick_pitch_env1 = 0.0f;
volatile float kick_pitch_env2 = 0.0f;
volatile float kick_rnd_pitch = 1.0f;
volatile float kick_vel = 1.0f;
float kick_phase = 0.0f;
float kick_click_phase = 0.0f;
float kick_base_inc;
float kick_decay_coeff;

// ── Snare ──
volatile float sn_exciter_env = 0.0f;
volatile float sn_noise_env = 0.0f;
volatile float sn_rnd_pitch = 1.0f;
volatile float sn_vel = 1.0f;
float sn_phase1 = 0.0f, sn_phase2 = 0.0f;
float sn_inc1, sn_inc2;
float sn_noise_coeff;
Resonator sn_wire_filter;

// ── Hihat (Waveguide Cymbal) ──
volatile float hh_exciter_env = 0.0f;
volatile float hh_gate_env = 0.0f;
volatile float hh_rnd_pitch = 1.0f;
volatile float hh_vel = 1.0f;
float hh_decay_coeff;
float hh_gate_coeff;
Resonator hh_color_filter;

#define HH_COMB_MAX 64
float hc1[HH_COMB_MAX], hc2[HH_COMB_MAX], hc3[HH_COMB_MAX], hc4[HH_COMB_MAX], hc5[HH_COMB_MAX], hc6[HH_COMB_MAX];
int hc_ptr = 0;
int hl1=11, hl2=17, hl3=23, hl4=31, hl5=41, hl6=47;

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

// Master Bus Compressor State
float comp_env = 0.0f;
float comp_thresh = 0.3f;
float comp_ratio = 4.0f;
float comp_attack, comp_release;

// Master DJ FX
Resonator master_filter;
float master_drive = 1.0f;

// ── Glitch Engine removed ──
// Global Kit Selection
// 0=808, 1=909, 2=Realistic, 3=LinnDrum (samples), 4=Acoustic (samples)
volatile int current_kit = 2;
float hh_sq_phase[6] = {0};
float hh_sq_inc[6] = {0};

// Modal Bell State
Resonator bell1, bell2, bell3;
float bell1_env = 0, bell2_env = 0, bell3_env = 0;
float bell1_decay = 0.99f, bell2_decay = 0.99f, bell3_decay = 0.99f;

// FM / Tom State
float fm_p1=0, fm_m1=0, fm_p2=0, fm_m2=0, fm_p3=0, fm_m3=0;
float fm_inc1=0, fm_inc2=0, fm_inc3=0;
float fm_ratio1=2.0f, fm_ratio2=2.0f, fm_ratio3=2.0f;
float fm_amt1=0, fm_amt2=0, fm_amt3=0;
Resonator tom_f1, tom_f2, tom_f3;

// VA Bassline State (Kit 13)
float bass_phase = 0.0f;
float bass_phase2 = 0.0f;
float bass_inc = 0.0f;
float target_bass_inc = 0.0f;
bool  bass_slide = false;
float bass_env = 0.0f;
float bass_vel = 1.0f;
bool bass_accent = false;
float bass_filter_env = 0.0f;
Resonator bass_filter;

// External Audio FX Loop (Kit 14)
float ext_dc_block_z = 0.0f;            // DC blocking HPF state (one-pole)

// Sample Playback State
volatile float smp_kick_pos = -1.0f, smp_snare_pos = -1.0f, smp_hihat_pos = -1.0f;
volatile float smp_kick_vel = 1.0f, smp_snare_vel = 1.0f, smp_hihat_vel = 1.0f;
volatile float smp_kick_inc = 0.6f, smp_snare_inc = 0.6f, smp_hihat_inc = 0.6f;
volatile float smp_kick_env = 0.0f, smp_snare_env = 0.0f, smp_hihat_env = 0.0f;
volatile float smp_k_dec = 0.999f, smp_s_dec = 0.999f, smp_h_dec = 0.999f;
const float SMP_BASE_RATIO = (float)SAMPLE_RATE / AUDIO_FS; // ~0.602

uint sliceAudio, sliceIRQ;

// Fast ISR-safe RNG to prevent deadlocks
volatile uint32_t fast_rand_seed = 12345;
uint32_t fast_rand() {
    fast_rand_seed = (1103515245 * fast_rand_seed + 12345);
    return fast_rand_seed;
}

void trigKick() { 
  kick_vel = 0.85f + ((fast_rand() % 150) / 1000.0f); 
  if (current_kit <= 2) {
    kick_env = kick_vel; kick_pitch_env1 = kick_vel; kick_pitch_env2 = kick_vel; 
    kick_phase = 0.0f;
  } else if (current_kit >= 3 && current_kit <= 9) {
    smp_kick_pos = 0.0f; smp_kick_vel = kick_vel; smp_kick_env = 1.0f;
  } else if (current_kit == 10) {
    bell1_env = 1.0f;
  } else if (current_kit == 11 || current_kit == 12) {
    kick_env = kick_vel; fm_p1 = 0; fm_m1 = 0;
  } else if (current_kit == 13) {
    bass_env = 1.0f; bass_filter_env = 1.0f; bass_vel = kick_vel;
  }
  ledTrigReq = true; 
}
void trigSnare() { 
  sn_vel = 0.85f + ((fast_rand() % 150) / 1000.0f);
  if (current_kit <= 2) {
    sn_exciter_env = sn_vel; sn_noise_env = sn_vel; 
    sn_phase1 = 0.0f; sn_phase2 = 0.0f;
  } else if (current_kit >= 3 && current_kit <= 9) {
    smp_snare_pos = 0.0f; smp_snare_vel = sn_vel; smp_snare_env = 1.0f;
  } else if (current_kit == 10) {
    bell2_env = 1.0f;
  } else if (current_kit == 11 || current_kit == 12) {
    sn_exciter_env = sn_vel; fm_p2 = 0; fm_m2 = 0;
  } else if (current_kit == 13) {
    bass_accent = true;
  }
  ledTrigReq = true; 
}
void trigHihat() { 
  hh_vel = 0.85f + ((fast_rand() % 150) / 1000.0f);
  if (current_kit <= 2) {
    hh_exciter_env = 1.0f; hh_gate_env = 1.0f;
  } else if (current_kit >= 3 && current_kit <= 9) {
    smp_hihat_pos = 0.0f; smp_hihat_vel = hh_vel; smp_hihat_env = 1.0f;
  } else if (current_kit == 10) {
    bell3_env = 1.0f;
  } else if (current_kit == 11 || current_kit == 12) {
    hh_exciter_env = hh_vel; fm_p3 = 0; fm_m3 = 0;
  } else if (current_kit == 13) {
    // Used for CV Pitch
  }
  ledTrigReq = true; 
}

// ── Grids Sequencer ──
volatile int grids_step = 15;
bool is_grids_mode = false;
uint32_t btn_press_start = 0;
bool btn_long_pressed = false;
uint32_t kit_color_timer = 0; // Timer for temp kit color preview

const uint8_t k_map[4][16] = {
    {255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0},
    {255, 0, 0, 150, 0, 0, 200, 0, 255, 0, 100, 0, 0, 0, 200, 0},
    {255, 0, 150, 0, 200, 0, 0, 150, 255, 0, 0, 0, 200, 100, 0, 50},
    {255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 100, 0, 0, 0, 0}
};
const uint8_t s_map[4][16] = {
    {0, 0, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0},
    {0, 0, 0, 0, 255, 0, 0, 150, 0, 100, 0, 0, 255, 0, 150, 0},
    {0, 0, 100, 0, 255, 0, 100, 0, 0, 0, 100, 0, 255, 0, 0, 150},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0}
};
const uint8_t h_map[4][16] = {
    {255, 0, 200, 0, 255, 0, 200, 0, 255, 0, 200, 0, 255, 0, 200, 0},
    {200, 100, 255, 100, 200, 100, 255, 100, 200, 100, 255, 100, 200, 100, 255, 100},
    {0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0},
    {255, 50, 150, 100, 255, 50, 150, 100, 255, 50, 150, 100, 255, 50, 150, 100}
};

float get_prob(const uint8_t map[4][16], float x, int step) {
    float x_scaled = x * 3.0f;
    int idx = (int)x_scaled;
    float frac = x_scaled - idx;
    if (idx >= 3) return map[3][step] * 0.0039215f;
    float val1 = map[idx][step] * 0.0039215f;
    float val2 = map[idx+1][step] * 0.0039215f;
    return val1 * (1.0f - frac) + val2 * frac;
}

const int scales[4][7] = {
    {0, 3, 5, 7, 10, 12, 15}, // Minor Pentatonic
    {0, 2, 3, 5, 7,  9, 10},  // Dorian
    {0, 1, 3, 5, 7,  8, 10},  // Phrygian
    {0, 7, 12, 19, 0, 7, 12}  // Fifths
};

volatile uint32_t last_clock_time = 0;
volatile uint32_t clock_period_ms = 0;

void advanceGrids() {
    uint32_t now = millis();
    if (last_clock_time > 0) {
        uint32_t diff = now - last_clock_time;
        if (clock_period_ms == 0) clock_period_ms = diff;
        else clock_period_ms = (clock_period_ms * 3 + diff) / 4; // Smooth jitter
    }
    last_clock_time = now;
    
    grids_step = (grids_step + 1) & 15;
    
    if (current_kit == 13) {
        // --- Expressive Generative Bassline ---
        float x = params[7].p1; // Complexity/Style
        float y = params[7].p2; // Density
        
        float thresh = 0.99f - (y * 0.95f);
        float k_prob = get_prob(k_map, x, grids_step); // Trigger probability
        float s_prob = get_prob(s_map, x, grids_step); // Slide/Tie probability
        float h_prob = get_prob(h_map, x, grids_step); // Note/Octave probability
        
        static bool prev_note_active = false;
        
        if (k_prob > thresh) {
            // Decide if we re-trigger the envelope or 'Tie' (legato)
            bool is_tie = (prev_note_active && s_prob > 0.5f);
            
            if (!is_tie) {
                bass_env = 1.0f;
                bass_filter_env = 1.0f;
            }
            
            // Slide logic
            bass_slide = (s_prob > 0.7f);
            
            // Accent logic
            bass_accent = (h_prob > 0.85f);
            
            // Note selection logic
            int scale_idx = (int)(x * 3.99f); // X map also influences scale choice
            int note_idx  = (int)(h_prob * 6.99f); 
            int note = scales[scale_idx][note_idx];
            
            // Octave jumps (up or down based on map regions)
            if (h_prob > 0.7f) note += 12;
            else if (h_prob < 0.3f && x > 0.5f) note -= 12;
            
            float base_pitch = 30.0f + params[0].p1 * 40.0f; // Tuning Page 0
            target_bass_inc = (base_pitch * powf(1.059463094f, (float)note)) * INV_FS;
            
            if (!bass_slide && !is_tie) {
                bass_inc = target_bass_inc; // Instant jump if no slide/tie
            }
            prev_note_active = true;
        } else {
            // Note off / Rest
            prev_note_active = false;
        }
    } else {
        // --- Standard Grids Mode ---
        float x = params[7].p1; // Grids style
        float y = params[7].p2; // Density
        
        if (grids_fill_steps > 0) {
            y = 1.0f; // Force max density for the fill
            grids_fill_steps--;
        }
        
        float thresh = 0.99f - (y * 0.99f);
        
        float k_prob = get_prob(k_map, x, grids_step);
        float s_prob = get_prob(s_map, x, grids_step);
        float h_prob = get_prob(h_map, x, grids_step);
        
        if (k_prob > thresh && k_prob > 0.01f) trigKick();
        if (s_prob > thresh && s_prob > 0.01f) trigSnare();
        if (h_prob > thresh && h_prob > 0.01f) trigHihat();
    }
}

void isr_gpio7() {
    if (current_kit == 14) {
        // In Ext Audio mode, IN1 acts as a clock/sync input for the Tape Delay!
        uint32_t now = millis();
        if (last_clock_time > 0) {
            uint32_t diff = now - last_clock_time;
            if (clock_period_ms == 0) clock_period_ms = diff;
            else clock_period_ms = (clock_period_ms * 3 + diff) / 4;
        }
        last_clock_time = now;
        return;
    }
    if (is_grids_mode) grids_step = 15; // Reset seq
    else trigKick();
}
void isr_gpio0() {
    if (current_kit == 14) return; // Ext Audio: ignore all triggers
    if (is_grids_mode) { 
        grids_fill_steps = 4; // Trigger a perfectly quantized 4-step max density fill
    } else trigSnare();
}
void isr_gpio28() {
    if (current_kit == 14) return; // Ext Audio: ignore all triggers
    if (is_grids_mode) advanceGrids(); // Clock
    else if (current_kit == 13) return; // VA Bass CV mode
    else trigHihat();
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
  
  if (current_kit == 14 || (current_kit == 13 && !is_grids_mode)) {
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
  } else {
      adc_cycle++;
      if (adc_cycle == 100) {
          adc_select_input(0); adc_read(); global_a0_val = adc_read();
      } else if (adc_cycle == 200) {
          adc_select_input(1); adc_read(); global_a1_val = adc_read();
      } else if (adc_cycle == 300) {
          adc_select_input(2); adc_read(); global_a2_val = adc_read();
          adc_cycle = 0;
      }
  }

  // --- EXTERNAL AUDIO FX LOOP (Kit 14) ---
  if (current_kit == 14) {
    // Convert 12-bit ADC (0..4095) centred at midpoint to bipolar float
    float raw = ((float)raw_a2 - 2047.5f) * (1.0f / 2048.0f);
    
    // One-pole DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]
    static float ext_dc_y = 0.0f;
    ext_dc_y = raw - ext_dc_block_z + 0.995f * ext_dc_y;
    ext_dc_block_z = raw;
    
    // 4-point Moving Average filter to aggressively notch out high-frequency ADC switching whine
    static float ma[4] = {0};
    static int ma_idx = 0;
    ma[ma_idx] = ext_dc_y;
    ma_idx = (ma_idx + 1) & 3;
    float ext_lp = (ma[0] + ma[1] + ma[2] + ma[3]) * 0.25f;
    
    // Smooth Downward Expander (Noise Gate) to silence ADC hiss when no audio is playing
    static float gate_env = 0.0f;
    float rect = fabsf(ext_lp);
    if (rect > gate_env) gate_env += (rect - gate_env) * 0.1f;   // fast attack (approx 1ms)
    else                 gate_env += (rect - gate_env) * 0.001f; // slow release (approx 100ms)
    
    float clean_sig = ext_lp;
    if (gate_env < 0.006f) { // Noise floor threshold (~12 LSBs of noise)
        float fade = gate_env * 166.66f; // 1.0f / 0.006f
        clean_sig *= fade * fade; // smooth cubic-like fade out into silence
    }
    
    out = clean_sig;
  }

  // --- KICK ---
  if (kick_env > 0.001f && current_kit <= 2) {
    float k_out = 0.0f;
    
    if (current_kit == 0) { // 808
        float pitch_mod = kick_pitch_env1 * kick_pitch_env1 * 100.0f * INV_FS;
        kick_phase += kick_base_inc + pitch_mod;
        if (kick_phase >= 1.0f) kick_phase -= 1.0f;
        k_out = sinf(kick_phase * TWO_PI) * kick_env;
        k_out = tanhf(k_out * 1.5f); 
    } else if (current_kit == 1) { // 909
        float pitch_mod = kick_pitch_env1 * kick_pitch_env1 * 300.0f * INV_FS;
        kick_phase += kick_base_inc + pitch_mod;
        if (kick_phase >= 1.0f) kick_phase -= 1.0f;
        float k_body = sinf(kick_phase * TWO_PI);
        
        kick_click_phase += 5000.0f * INV_FS;
        if (kick_click_phase >= 1.0f) kick_click_phase -= 1.0f;
        float click = (kick_click_phase > 0.5f ? 1.0f : -1.0f) * kick_pitch_env2 * 0.5f; 
        k_out = (k_body + click) * kick_env;
        k_out = tanhf(k_out * 2.5f);
    } else { // Realistic
        kick_phase += kick_base_inc;
        if (kick_phase >= 1.0f) kick_phase -= 1.0f;
        float k_body = sinf(kick_phase * TWO_PI);
        
        kick_click_phase += 4000.0f * INV_FS;
        if (kick_click_phase >= 1.0f) kick_click_phase -= 1.0f;
        float click = sinf(kick_click_phase * TWO_PI) * kick_pitch_env1;
        k_out = (k_body + click * 0.4f) * kick_env;
        k_out = tanhf(k_out * 3.5f);
    }
    
    out += k_out * 0.7f;
    kick_env *= kick_decay_coeff;
    kick_pitch_env1 *= 0.985f;
    kick_pitch_env2 *= 0.998f;
  }

  // --- SNARE ---
  if (sn_noise_env > 0.001f && current_kit <= 2) {
    float s_out = 0.0f;
    
    // Gated pink noise - only generate when envelope is active (fixes aliasing at low levels)
    static float b0=0, b1=0, b2=0;
    float pink = 0.0f;
    if (sn_noise_env > 0.01f) {
      b0 = 0.99765f * b0 + white * 0.0990460f;
      b1 = 0.96300f * b1 + white * 0.2965164f;
      b2 = 0.57000f * b2 + white * 1.0526913f;
      pink = (b0 + b1 + b2 + white * 0.1848f) * 0.15f;
    } else {
      b0 *= 0.999f; b1 *= 0.999f; b2 *= 0.999f; // Drain filters silently
    }
    sn_wire_filter.process(pink);
    
    if (current_kit == 0) { // 808
        sn_phase1 += sn_inc1; if(sn_phase1 >= 1.0f) sn_phase1 -= 1.0f;
        sn_phase2 += sn_inc1 * 1.83f; if(sn_phase2 >= 1.0f) sn_phase2 -= 1.0f; 
        float tones = (sinf(sn_phase1 * TWO_PI) + sinf(sn_phase2 * TWO_PI) * 0.5f) * sn_exciter_env;
        float noise = sn_wire_filter.hp * sn_noise_env;
        s_out = (tones + noise * 1.5f) * 0.8f;
    } else if (current_kit == 1) { // 909
        float pitch_mod = sn_exciter_env * 200.0f * INV_FS;
        sn_phase1 += sn_inc1 + pitch_mod; if(sn_phase1 >= 1.0f) sn_phase1 -= 1.0f;
        float tone = sinf(sn_phase1 * TWO_PI) * sn_exciter_env;
        float noise = sn_wire_filter.bp * sn_noise_env; 
        s_out = (tone + noise * 1.2f) * 0.8f;
    } else { // Realistic
        float pitch_mod = sn_exciter_env * 150.0f * INV_FS;
        sn_phase1 += sn_inc1 + pitch_mod; if(sn_phase1 >= 1.0f) sn_phase1 -= 1.0f;
        sn_phase2 += sn_inc2 + pitch_mod; if(sn_phase2 >= 1.0f) sn_phase2 -= 1.0f;
        float membrane = (sinf(sn_phase1 * TWO_PI) + sinf(sn_phase2 * TWO_PI)) * 0.5f * sn_exciter_env;
        float noise = (sn_wire_filter.hp + sn_wire_filter.bp * 0.6f) * sn_noise_env;
        s_out = (membrane * 1.2f + noise * 1.8f) * 0.6f;
    }
    
    out += s_out;
    sn_exciter_env *= 0.997f;
    sn_noise_env *= sn_noise_coeff;
  }

  // --- HIHAT ---
  if (hh_gate_env > 0.001f && current_kit <= 2) {
    float h_out = 0.0f;
    
    if (current_kit == 0 || current_kit == 1) { // 808/909
        float metal = 0.0f;
        for(int i=0; i<6; i++) {
            hh_sq_phase[i] += hh_sq_inc[i];
            if (hh_sq_phase[i] >= 1.0f) hh_sq_phase[i] -= 1.0f;
            metal += (hh_sq_phase[i] > 0.5f) ? 1.0f : -1.0f;
        }
        hh_color_filter.process(metal * 0.166f);
        if (current_kit == 0) h_out = hh_color_filter.hp * hh_exciter_env * 1.5f;
        else                  h_out = hh_color_filter.bp * hh_exciter_env * 1.5f;
        
        hh_exciter_env *= hh_decay_coeff;
        hh_gate_env *= hh_gate_coeff;
    } else { // Realistic Waveguide
        float exciter = white * hh_exciter_env * hh_vel;
        if (hh_exciter_env > 0.95f) exciter += 5.0f * hh_vel;

    // Read the 6 parallel short delay lines
    float o1 = hc1[(hc_ptr + HH_COMB_MAX - hl1) & 63];
    float o2 = hc2[(hc_ptr + HH_COMB_MAX - hl2) & 63];
    float o3 = hc3[(hc_ptr + HH_COMB_MAX - hl3) & 63];
    float o4 = hc4[(hc_ptr + HH_COMB_MAX - hl4) & 63];
    float o5 = hc5[(hc_ptr + HH_COMB_MAX - hl5) & 63];
    float o6 = hc6[(hc_ptr + HH_COMB_MAX - hl6) & 63];
    
    // High feedback traps the noise, phasing it into a dense metallic clang
    float fb = 0.85f;
    hc1[hc_ptr] = exciter + o1 * fb;
    hc2[hc_ptr] = exciter + o2 * fb;
    hc3[hc_ptr] = exciter + o3 * fb;
    hc4[hc_ptr] = exciter + o4 * fb;
    hc5[hc_ptr] = exciter + o5 * fb;
    hc6[hc_ptr] = exciter + o6 * fb;
    hc_ptr = (hc_ptr + 1) & 63;
    
    float raw_metal = (o1 + o2 + o3 + o4 + o5 + o6) * 0.166f;
        hh_color_filter.process(raw_metal);
        h_out = hh_color_filter.hp;
        
        hh_exciter_env *= hh_decay_coeff;
        hh_gate_env *= hh_gate_coeff; 
    }
    
    out += h_out * 0.8f;
  } else if (current_kit >= 3 && current_kit <= 9) {
    // Sample-based hihat playback with linear interpolation
    const int16_t* tbl;
    int len;
    if      (current_kit == 3) { tbl = hihat_808s; len = HIHAT_808S_LEN; }
    else if (current_kit == 4) { tbl = tr909_h;    len = TR909_H_LEN; }
    else if (current_kit == 5) { tbl = tr606_h;    len = TR606_H_LEN; }
    else if (current_kit == 6) { tbl = linn_h;     len = LINN_H_LEN; }
    else if (current_kit == 7) { tbl = simmons_h;  len = SIMMONS_H_LEN; }
    else if (current_kit == 8) { tbl = hihat_rock; len = HIHAT_ROCK_LEN; }
    else if (current_kit == 9) { tbl = hihat_grit; len = HIHAT_GRIT_LEN; }
#if HAS_USER_KIT
    else if (current_kit == 15){ tbl = user_h;     len = USER_H_LEN; }
#endif
    else                       { tbl = hihat_grit; len = HIHAT_GRIT_LEN; } // Fallback
    
    if (smp_hihat_pos >= 0.0f && smp_hihat_pos < (float)(len - 1) && smp_hihat_env > 0.001f) {
      int idx = (int)smp_hihat_pos;
      float frac = smp_hihat_pos - idx;
      float s = ((float)tbl[idx] * (1.0f - frac) + (float)tbl[idx+1] * frac) * (1.0f / 32768.0f);
      out += s * smp_hihat_vel * smp_hihat_env * 0.6f;
      smp_hihat_pos += smp_hihat_inc;
      smp_hihat_env *= smp_h_dec;
    } else {
      smp_hihat_pos = -1.0f;
    }
  } else if (current_kit != 14) {
    // Kit 10: No hihat waveguide logic needed, just keep buffers stable
    hc_ptr = (hc_ptr + 1) & 63;
  }

  // Sample-based kick (kits 3-9)
  if (current_kit >= 3 && current_kit <= 9 && smp_kick_pos >= 0.0f && smp_kick_env > 0.001f) {
    const int16_t* tbl;
    int len;
    if      (current_kit == 3) { tbl = kick_808s; len = KICK_808S_LEN; }
    else if (current_kit == 4) { tbl = tr909_k;    len = TR909_K_LEN; }
    else if (current_kit == 5) { tbl = tr606_k;    len = TR606_K_LEN; }
    else if (current_kit == 6) { tbl = linn_k;     len = LINN_K_LEN; }
    else if (current_kit == 7) { tbl = simmons_k;  len = SIMMONS_K_LEN; }
    else if (current_kit == 8) { tbl = kick_rock;  len = KICK_ROCK_LEN; }
    else if (current_kit == 9) { tbl = kick_grit;  len = KICK_GRIT_LEN; }
#if HAS_USER_KIT
    else if (current_kit == 15){ tbl = user_k;     len = USER_K_LEN; }
#endif
    else                       { tbl = kick_grit;  len = KICK_GRIT_LEN; } // Fallback
    
    if (smp_kick_pos < (float)(len - 1)) {
      int idx = (int)smp_kick_pos;
      float frac = smp_kick_pos - idx;
      float s = ((float)tbl[idx] * (1.0f - frac) + (float)tbl[idx+1] * frac) * (1.0f / 32768.0f);
      out += s * smp_kick_vel * smp_kick_env * 0.8f;
      smp_kick_pos += smp_kick_inc;
      smp_kick_env *= smp_k_dec;
    } else { smp_kick_pos = -1.0f; }
  }

  // Sample-based snare (kits 3-9)
  if (current_kit >= 3 && current_kit <= 9 && smp_snare_pos >= 0.0f && smp_snare_env > 0.001f) {
    const int16_t* tbl;
    int len;
    if      (current_kit == 3) { tbl = snare_808s; len = SNARE_808S_LEN; }
    else if (current_kit == 4) { tbl = tr909_s;    len = TR909_S_LEN; }
    else if (current_kit == 5) { tbl = tr606_s;    len = TR606_S_LEN; }
    else if (current_kit == 6) { tbl = linn_s;     len = LINN_S_LEN; }
    else if (current_kit == 7) { tbl = simmons_s;  len = SIMMONS_S_LEN; }
    else if (current_kit == 8) { tbl = snare_rock; len = SNARE_ROCK_LEN; }
    else if (current_kit == 9) { tbl = snare_grit; len = SNARE_GRIT_LEN; }
#if HAS_USER_KIT
    else if (current_kit == 15){ tbl = user_s;     len = USER_S_LEN; }
#endif
    else                       { tbl = snare_grit; len = SNARE_GRIT_LEN; } // Fallback
    
    if (smp_snare_pos < (float)(len - 1)) {
      int idx = (int)smp_snare_pos;
      float frac = smp_snare_pos - idx;
      float s = ((float)tbl[idx] * (1.0f - frac) + (float)tbl[idx+1] * frac) * (1.0f / 32768.0f);
      out += s * smp_snare_vel * smp_snare_env * 0.7f;
      smp_snare_pos += smp_snare_inc;
      smp_snare_env *= smp_s_dec;
    } else { smp_snare_pos = -1.0f; }
  }

  // --- MODAL BELLS (Kit 10) ---
  if (current_kit == 10) {
    // Strike is a short burst of noise gated by the trigger envelope
    float s1 = (bell1_env > 0.005f) ? (white * bell1_env * 0.2f) : 0.0f;
    float s2 = (bell2_env > 0.005f) ? (white * bell2_env * 0.2f) : 0.0f;
    float s3 = (bell3_env > 0.005f) ? (white * bell3_env * 0.2f) : 0.0f;
    
    // Process filters always to allow ringing tails
    bell1.process(s1);
    bell2.process(s2);
    bell3.process(s3);
    
    out += (bell1.bp * 2.5f + bell2.bp * 2.0f + bell3.bp * 1.5f);
    
    // Decay the exciter/strike envelope quickly
    bell1_env *= 0.985f;
    bell2_env *= 0.985f;
    bell3_env *= 0.985f;
  } else if (current_kit == 11) {
    // --- FM PERCUSSION (Kit 11) ---
    if (kick_env > 0.001f) {
      fm_m1 += fm_inc1 * fm_ratio1; if (fm_m1 > 1.0f) fm_m1 -= 1.0f;
      float mod = sinf(fm_m1 * TWO_PI) * fm_amt1 * kick_env;
      fm_p1 += fm_inc1 * (1.0f + mod); if (fm_p1 > 1.0f) fm_p1 -= 1.0f;
      out += sinf(fm_p1 * TWO_PI) * kick_env * kick_vel * 1.2f;
      kick_env *= 0.999f;
    }
    if (sn_exciter_env > 0.001f) {
      fm_m2 += fm_inc2 * fm_ratio2; if (fm_m2 > 1.0f) fm_m2 -= 1.0f;
      float mod = sinf(fm_m2 * TWO_PI) * fm_amt2 * sn_exciter_env;
      fm_p2 += fm_inc2 * (1.0f + mod); if (fm_p2 > 1.0f) fm_p2 -= 1.0f;
      out += sinf(fm_p2 * TWO_PI) * sn_exciter_env * sn_vel * 1.0f;
      sn_exciter_env *= 0.996f;
    }
    if (hh_exciter_env > 0.001f) {
      fm_m3 += fm_inc3 * fm_ratio3; if (fm_m3 > 1.0f) fm_m3 -= 1.0f;
      float mod = sinf(fm_m3 * TWO_PI) * fm_amt3 * hh_exciter_env;
      fm_p3 += fm_inc3 * (1.0f + mod); if (fm_p3 > 1.0f) fm_p3 -= 1.0f;
      out += sinf(fm_p3 * TWO_PI) * hh_exciter_env * hh_vel * 1.0f;
      hh_exciter_env *= 0.99f;
    }
  } else if (current_kit == 12) {
    // --- ANALOG TOMS (Kit 12) ---
    // Resonators pinged with very short impulses
    float s1 = (kick_env > 0.01f) ? kick_env : 0.0f;
    float s2 = (sn_exciter_env > 0.01f) ? sn_exciter_env : 0.0f;
    float s3 = (hh_exciter_env > 0.01f) ? hh_exciter_env : 0.0f;
    
    tom_f1.process(s1);
    tom_f2.process(s2);
    tom_f3.process(s3);
    
    out += (tom_f1.bp * 4.0f + tom_f2.bp * 3.5f + tom_f3.bp * 3.0f);
    
    kick_env *= 0.95f;
    sn_exciter_env *= 0.95f;
    hh_exciter_env *= 0.95f;
  } else if (current_kit == 13) {
    // --- VA BASSLINE (Kit 13) ---
    // Single Oscillator with Saw/Square Mix (Page 0 Pot 2)
    // Pitch Slew (Slide)
    if (bass_slide) {
        bass_inc += (target_bass_inc - bass_inc) * 0.0008f;
    } else {
        bass_inc = target_bass_inc;
    }
    
    bass_phase += bass_inc;
    if (bass_phase > 1.0f) bass_phase -= 1.0f;
    
    float shape = params[0].p2;
    float osc_saw = (bass_phase * 2.0f - 1.0f) - poly_blep(bass_phase, bass_inc);
    float osc_sq  = (bass_phase < 0.5f ? 1.0f : -1.0f) + poly_blep(bass_phase, bass_inc) - poly_blep(fmodf(bass_phase + 0.5f, 1.0f), bass_inc);
    float osc = osc_saw * (1.0f - shape) + osc_sq * shape;
    
    // Subtle detuned sub-oscillator for thickness
    static float sub_phase = 0.0f;
    sub_phase += bass_inc * 0.5f; if (sub_phase > 1.0f) sub_phase -= 1.0f;
    osc += ((sub_phase < 0.5f ? 0.3f : -0.3f)) * (1.0f - shape * 0.5f); // Square sub
    
    float amp_env = bass_env * (bass_accent ? 1.4f : 1.0f);
    float out_val = osc * amp_env * 0.7f;
    
    // Lowpass Filter
    float env_amt = params[1].p2;
    float cutoff = 30.0f + params[1].p1 * 6000.0f + (bass_filter_env * env_amt * 9000.0f) * (bass_accent ? 1.4f : 1.0f);
    float res = 0.6f + params[2].p1 * 25.0f; // More usable resonance range
    
    bass_filter.set(cutoff, res, AUDIO_FS);
    bass_filter.process(out_val);
    
    out += bass_filter.lp * 2.5f;
    
    // Variable Decay (Page 2 Pot 2)
    float decay_coeff = 0.995f + params[2].p2 * 0.0048f; // 0.995 to 0.9998
    bass_env *= decay_coeff;
    bass_filter_env *= decay_coeff;
    if (bass_accent && bass_env < 0.01f) bass_accent = false;
  }
  
  // Gate all drum synthesis when in Ext Audio mode
  if (current_kit == 14) {
    // out already set from ADC path above — nothing to add here
  }
  
  // --- TAPE DELAY ---
  float drum_bus = out;
  
  // Smooth slew rate for delay time creates analog tape pitch-shifting!
  current_delay_samples += (target_delay_samples - current_delay_samples) * 0.0005f;
  
  float d_read_pos = (float)delay_ptr + (float)DELAY_SIZE - current_delay_samples;
  int d_idx = (int)d_read_pos;
  float d_frac = d_read_pos - d_idx;
  float d_out = delay_buf[d_idx & (DELAY_SIZE-1)] * (1.0f - d_frac) + delay_buf[(d_idx + 1) & (DELAY_SIZE-1)] * d_frac;
  
  // Tape degradation: Lowpass in the feedback loop
  static float d_dmp = 0.0f;
  d_dmp += (d_out - d_dmp) * 0.4f; // Dull tape sound
  
  float d_in = drum_bus + tanhf(d_dmp * delay_fb); // Soft sat prevents explosion
  delay_buf[delay_ptr] = d_in;
  delay_ptr = (delay_ptr + 1) & (DELAY_SIZE-1);
  
  float delay_mixed = drum_bus + d_out * delay_mix;
  
  // --- ROOM REVERB (Freeverb) ---
  float dry = delay_mixed;
  float rv_out = 0.0f;
  
  // Parallel Schroeder-Moorer Comb Filters
  for(int i=0; i<8; i++) {
      float delayed = fv_cb[i][fv_cp[i]];
      fv_dmp[i] = delayed * (1.0f - rev_damp) + fv_dmp[i] * rev_damp;
      fv_cb[i][fv_cp[i]] = dry + fv_dmp[i] * rev_room_size;
      fv_cp[i] = (fv_cp[i] + 1) % fv_cl[i];
      rv_out += delayed;
  }
  
  // Series Allpass Diffusers
  for(int i=0; i<4; i++) {
      float delayed = fv_ab[i][fv_ap[i]];
      float ap_out = -rv_out * 0.5f + delayed;
      fv_ab[i][fv_ap[i]] = rv_out + delayed * 0.5f;
      fv_ap[i] = (fv_ap[i] + 1) % fv_al[i];
      rv_out = ap_out;
  }
  
  out = dry + rv_out * 0.15f * rev_mix;
  
  // Master Bus Compressor (SSL-style Glue)
  float rect = fabsf(out);
  if (rect > comp_env) comp_env = comp_attack * (comp_env - rect) + rect;
  else                 comp_env = comp_release * (comp_env - rect) + rect;
  
  float gain = 1.0f;
  if (comp_env > comp_thresh) {
      gain = comp_thresh + (comp_env - comp_thresh) / comp_ratio;
      gain /= comp_env;
  }
  
  float makeup = (current_kit == 14) ? 1.0f : 1.8f;
  out = out * gain * makeup; // Glue and selective makeup gain!
  
  // Master DJ Filter with smooth crossfade
  float dj = params[5].p1;
  master_filter.process(out);
  
  float mix_lp = 0.0f, mix_hp = 0.0f, mix_dry = 0.0f;
  if (dj < 0.45f) {
      mix_lp = 1.0f;
  } else if (dj < 0.5f) {
      mix_lp = (0.5f - dj) * 20.0f; // Crossfade LP to Dry
      mix_dry = 1.0f - mix_lp;
  } else if (dj < 0.55f) {
      mix_hp = (dj - 0.5f) * 20.0f; // Crossfade Dry to HP
      mix_dry = 1.0f - mix_hp;
  } else {
      mix_hp = 1.0f;
  }
  
  out = (master_filter.lp * mix_lp) + (master_filter.hp * mix_hp) + (out * mix_dry);
  
  out = tanhf(out * master_drive); // Master Saturation
  
  // Master DC Blocker (y[n] = x[n] - x[n-1] + 0.995 * y[n-1])
  out_dc_y = out - out_dc_block_z + 0.995f * out_dc_y;
  out_dc_block_z = out;
  out = out_dc_y;

  // --- 1st-order Sigma-Delta Noise Shaper + TPDF Dither ---
  // This pushes PWM quantization noise into the ultrasonic range,
  // effectively increasing the resolution for low-volume signals.
  static float pwm_err = 0.0f;
  float PWM_MID = 511.5f;
  
  float target_val = PWM_MID + out * PWM_MID * 0.9f;
  
  // Add a tiny bit of high-frequency dither (0.5 LSB)
  float dither = (white * 0.5f); 
  float val_to_quantize = target_val + pwm_err + dither;
  
  int val_int = (int)val_to_quantize;
  if(val_int < 0) val_int = 0;
  if(val_int > 1023) val_int = 1023;
  
  // Update error for the next sample
  pwm_err = val_to_quantize - (float)val_int;
  
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)val_int);
}

void updatePageLED() {
  if (ledTrigOn) return;
  // Spectral Rainbow Palette (ROYGBIV-ish)
  if      (currentPage == 0) led.setPixelColor(0, led.Color(255, 0, 0));   // 0: Red (Kick)
  else if (currentPage == 1) led.setPixelColor(0, led.Color(255, 120, 0)); // 1: Orange (Snare)
  else if (currentPage == 2) led.setPixelColor(0, led.Color(255, 255, 0)); // 2: Yellow (Hihat)
  else if (currentPage == 3) led.setPixelColor(0, led.Color(0, 255, 255)); // 3: Cyan (Delay)
  else if (currentPage == 4) led.setPixelColor(0, led.Color(0, 255, 0));   // 4: Green (Room)
  else if (currentPage == 5) led.setPixelColor(0, led.Color(0, 0, 255));   // 5: Blue (Master DJ)
  else if (currentPage == 6) { 
      // 6: Kit Config - Dynamic color based on selected kit, but only temporarily
      if (kit_color_timer > 0 && (millis() - kit_color_timer < 600)) {
          switch(current_kit) {
              case 0:  led.setPixelColor(0, led.Color(255, 0, 0)); break;     // 808: Red
              case 1:  led.setPixelColor(0, led.Color(255, 100, 0)); break;   // 909: Orange
              case 2:  led.setPixelColor(0, led.Color(255, 255, 0)); break;   // Realistic: Yellow
              case 3:  led.setPixelColor(0, led.Color(150, 255, 0)); break;   // TR-808 (s): Lime
              case 4:  led.setPixelColor(0, led.Color(0, 255, 0)); break;     // TR-909 (s): Green
              case 5:  led.setPixelColor(0, led.Color(0, 255, 150)); break;   // TR-606 (s): Spring Green
              case 6:  led.setPixelColor(0, led.Color(0, 255, 255)); break;   // LinnDrum: Cyan
              case 7:  led.setPixelColor(0, led.Color(0, 100, 255)); break;   // SDS-V: Light Blue
              case 8:  led.setPixelColor(0, led.Color(0, 0, 255)); break;     // Rock: Blue
              case 9:  led.setPixelColor(0, led.Color(100, 0, 255)); break;   // Grit: Purple
              case 10: led.setPixelColor(0, led.Color(200, 0, 255)); break;   // Modal Bells: Violet
              case 11: led.setPixelColor(0, led.Color(255, 0, 255)); break;   // FM: Magenta
              case 12: led.setPixelColor(0, led.Color(255, 0, 100)); break;   // Toms: Hot Pink
              case 13: led.setPixelColor(0, led.Color(255, 255, 255)); break; // VA Bass: White
              case 14: led.setPixelColor(0, led.Color(255, 80,  0)); break;    // Ext Audio: Amber
              default: led.setPixelColor(0, led.Color(160, 0, 255)); break;
          }
      } else {
          led.setPixelColor(0, led.Color(160, 0, 255)); // Normal Purple
      }
  }
  else if (currentPage == 7) led.setPixelColor(0, led.Color(200, 220, 255)); // 7: Ice White (Grids)
  
  else led.setPixelColor(0, led.Color(50, 50, 50)); // Fallback
  led.show();
}

void updateSynthesisParams() {
  // Kick
  float k_p2 = params[0].p2 * params[0].p2; // Squared for exponential resolution
  kick_base_inc = (40.0f + params[0].p1 * 60.0f) * kick_rnd_pitch * INV_FS;
  kick_decay_coeff = expf(-1.0f / ((0.05f + k_p2 * 1.5f) * AUDIO_FS));

  // Snare
  float sn_p2 = params[1].p2 * params[1].p2;
  float sn_f0 = (150.0f + params[1].p1 * 200.0f) * sn_rnd_pitch;
  sn_inc1 = sn_f0 * INV_FS;
  sn_inc2 = sn_f0 * 1.59f * INV_FS; // Membrane mode
  
  float sn_decay = 0.03f + sn_p2 * 0.40f;
  sn_noise_coeff = expf(-1.0f / (sn_decay * AUDIO_FS));
  sn_wire_filter.set(2000.0f + params[1].p1 * 2000.0f, 2.5f, AUDIO_FS); // Higher Q for crispy wires

  // Hihat
  float hh_p2 = params[2].p2 * params[2].p2;
  float hh_decay_sec = 0.05f + hh_p2 * 0.9f;
  hh_decay_coeff = expf(-1.0f / (hh_decay_sec * AUDIO_FS));
  hh_gate_coeff = expf(-1.0f / (hh_decay_sec * 3.0f * AUDIO_FS));
  
  float hh_cutoff = 1000.0f + params[2].p1 * 8000.0f; 
  hh_color_filter.set(hh_cutoff, 0.707f, AUDIO_FS);
  
  // 808 Hex Incs
  float hh_base_808 = 200.0f + params[2].p1 * 400.0f;
  hh_sq_inc[0] = hh_base_808 * 1.000f * INV_FS;
  hh_sq_inc[1] = hh_base_808 * 1.482f * INV_FS;
  hh_sq_inc[2] = hh_base_808 * 1.800f * INV_FS;
  hh_sq_inc[3] = hh_base_808 * 2.546f * INV_FS;
  hh_sq_inc[4] = hh_base_808 * 2.630f * INV_FS;
  hh_sq_inc[5] = hh_base_808 * 3.896f * INV_FS;
  
  // Tape Delay (Now Page 3)
  float d_time;
  // Use clock sync if in Grids mode OR Ext Audio mode, BUT only if the clock is actively running
  bool clock_active = (clock_period_ms > 0 && clock_period_ms < 2000 && (millis() - last_clock_time < 2000));
  
  if ((is_grids_mode || current_kit == 14) && clock_active) {
      // Snap to clock division
      int div_idx = (int)(params[3].p1 * 6.99f); // 0 to 6
      float mults[7] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}; // 1/32 up to 1/4 note
      d_time = (clock_period_ms * mults[div_idx]) / 1000.0f;
  } else {
      d_time = 0.02f + params[3].p1 * 0.85f; // 20ms to 870ms (Manual Pot Control)
  }
  
  target_delay_samples = d_time * AUDIO_FS;
  delay_fb = params[3].p2 * 0.95f; // up to self-oscillation
  delay_mix = params[3].p2 * 0.8f;

  // Reverb (Freeverb) (Now Page 4)
  rev_room_size = 0.7f + params[4].p1 * 0.28f; // 0.7 to 0.98
  rev_mix = params[4].p2 * 1.5f;
  rev_damp = 0.4f; // Fixed damp for drum room
  
  // Master DJ FX
  float dj = params[5].p1;
  if (dj < 0.5f) {
      // 0 to 0.5: Lowpass from 100Hz up to 20kHz
      float val = dj * 2.0f;
      float cutoff = 100.0f * powf(200.0f, val);
      master_filter.set(cutoff, 0.707f, AUDIO_FS);
  } else {
      // 0.5 to 1.0: Highpass from 20Hz up to 10kHz
      float val = (dj - 0.5f) * 2.0f;
      float cutoff = 20.0f * powf(500.0f, val);
      master_filter.set(cutoff, 0.707f, AUDIO_FS);
  }
  
  float drive_knob = params[5].p2;
  if (drive_knob <= 0.5f) {
      // 0.0 to 0.5 maps to Gain 0.0 to 1.0 (Attenuation)
      master_drive = drive_knob * 2.0f;
  } else {
      // 0.5 to 1.0 maps to Gain 1.0 to 12.0 (Distortion)
      float d_up = (drive_knob - 0.5f) * 2.0f;
      master_drive = 1.0f + d_up * d_up * 11.0f; 
  }
  
  // Kit Configuration (dynamically expands if user kit is present)
  static int last_kit = -1;
  float kv = params[6].p1; 
  // master_volume = params[6].p2 * 2.0f; // Leaving open for now
  
  int max_kits = HAS_USER_KIT ? 16 : 15;
  int raw_kit = (int)(kv * (max_kits - 0.001f));
  
  if (HAS_USER_KIT && raw_kit >= 14) {
      // Keep Ext Audio FX at the very end of the knob
      if (raw_kit == 14) current_kit = 15; // User Sample Kit
      else current_kit = 14;               // Ext Audio FX
  } else {
      current_kit = raw_kit;
  }

  if (current_kit != last_kit) {
    // Zero all envelopes on kit switch to prevent ghost kits
    kick_env = 0; sn_noise_env = 0; hh_gate_env = 0;
    smp_kick_env = 0; smp_snare_env = 0; smp_hihat_env = 0;
    bell1_env = 0; bell2_env = 0; bell3_env = 0;
    // Clear waveguides and pink filters to stop DC/Noise leaks
    for(int i=0; i<HH_COMB_MAX; i++) { hc1[i]=0; hc2[i]=0; hc3[i]=0; hc4[i]=0; hc5[i]=0; hc6[i]=0; }
    last_kit = current_kit;
    
    // Update the UI immediately so the user can see the new kit's color while turning the knob
    kit_color_timer = millis();
    if (currentPage == 6 && !ledTrigOn) updatePageLED();
  }

  // Bell Resonator update
  if (current_kit == 10) {
    float f1 = 60.0f + params[0].p1 * 400.0f;
    float f2 = 400.0f + params[1].p1 * 2000.0f;
    float f3 = 1000.0f + params[2].p1 * 6000.0f;
    // High-Q for bell resonance
    bell1.set(f1, 100.0f + params[0].p2 * 300.0f, AUDIO_FS);
    bell2.set(f2, 80.0f + params[1].p2 * 250.0f, AUDIO_FS);
    bell3.set(f3, 60.0f + params[2].p2 * 200.0f, AUDIO_FS);
    bell1_decay = 0.999f + params[0].p2 * 0.00095f;
    bell2_decay = 0.998f + params[1].p2 * 0.00195f;
    bell3_decay = 0.997f + params[2].p2 * 0.00295f;
  }
  
  // Sample Kit tuning (Pitch/Decay)
  if ((current_kit >= 3 && current_kit <= 9) || current_kit == 15) {
    // Pitch: +/- 1 Octave, centered at 1.0x original pitch when knob is at 12 o'clock
    smp_kick_inc = powf(2.0f, (params[0].p1 - 0.5f) * 2.0f) * SMP_BASE_RATIO;
    smp_snare_inc = powf(2.0f, (params[1].p1 - 0.5f) * 2.0f) * SMP_BASE_RATIO;
    smp_hihat_inc = powf(2.0f, (params[2].p1 - 0.5f) * 2.0f) * SMP_BASE_RATIO;
    
    // Decay: Match the analog synth decay ranges perfectly
    float k_decay_sec = 0.1f + params[0].p2 * params[0].p2 * 1.5f; // 100ms to 1.6s
    float s_decay_sec = 0.1f + params[1].p2 * params[1].p2 * 0.8f; // 100ms to 900ms
    float h_decay_sec = 0.05f + params[2].p2 * params[2].p2 * 0.9f; // 50ms to 950ms
    
    smp_k_dec = expf(-1.0f / (k_decay_sec * AUDIO_FS));
    smp_s_dec = expf(-1.0f / (s_decay_sec * AUDIO_FS));
    smp_h_dec = expf(-1.0f / (h_decay_sec * AUDIO_FS));
  }
  
  // FM Percussion Update
  if (current_kit == 11) {
    float fm1_f0 = 40.0f + params[0].p1 * 200.0f;
    float fm2_f0 = 200.0f + params[1].p1 * 1000.0f;
    float fm3_f0 = 1000.0f + params[2].p1 * 8000.0f;
    fm_inc1 = fm1_f0 * INV_FS;
    fm_inc2 = fm2_f0 * INV_FS;
    fm_inc3 = fm3_f0 * INV_FS;
    
    fm_amt1 = params[0].p2 * 3.0f; // Sub mod
    fm_amt2 = params[1].p2 * 5.0f; // Woodblock mod
    fm_amt3 = params[2].p2 * 8.0f; // Metallic mod
    
    fm_ratio1 = 2.0f; // Harmonic
    fm_ratio2 = 2.4f; // Inharmonic (Wood)
    fm_ratio3 = 3.7f; // Highly Inharmonic (Metal)
  }
  
  // Analog Toms Update
  if (current_kit == 12) {
    float f1 = 60.0f + params[0].p1 * 150.0f;
    float f2 = 100.0f + params[1].p1 * 250.0f;
    float f3 = 150.0f + params[2].p1 * 350.0f;
    
    // Low Q for fat toms (resonance 10 to 40)
    tom_f1.set(f1, 10.0f + params[0].p2 * 30.0f, AUDIO_FS);
    tom_f2.set(f2, 10.0f + params[1].p2 * 30.0f, AUDIO_FS);
    tom_f3.set(f3, 10.0f + params[2].p2 * 30.0f, AUDIO_FS);
  }
  
  // VA Bassline & Ext Audio Interrupt Guard
  static bool cv_mode_active = false;
  // Detach interrupt on Pin 28 if it's being used as an Analog CV/Audio input
  bool should_be_cv = (current_kit == 13 && !is_grids_mode) || (current_kit == 14);
  
  if (should_be_cv != cv_mode_active) {
      if (should_be_cv) {
          detachInterrupt(digitalPinToInterrupt(28));
      } else {
          attachInterrupt(digitalPinToInterrupt(28), isr_gpio28, RISING);
      }
      cv_mode_active = should_be_cv;
  }
  
  if (current_kit == 13 && !is_grids_mode) {
      // Manual CV tracking from A2 for Bassline
      float cv = (float)global_a2_val;
      float base_pitch = 20.0f + params[0].p1 * 100.0f; // Base pitch (Page 0)
      float f0 = base_pitch * powf(2.0f, (cv / 4095.0f) * 4.0f); // 4 octave range from CV
      bass_inc = f0 * INV_FS;
  }
}

void setup() {
  vreg_set_voltage(VREG_VOLTAGE_1_20); // Boost voltage for overclock stability
  delay(10);
  set_sys_clock_khz(250000, true);     // Overclock to 250MHz for massive DSP headroom

  adc_init();
  adc_gpio_init(26); // A0
  adc_gpio_init(27); // A1
  adc_gpio_init(28); // A2
  adc_select_input(2);

  comp_attack = expf(-1.0f / (0.005f * AUDIO_FS)); // 5ms punchy attack
  comp_release = expf(-1.0f / (0.100f * AUDIO_FS)); // 100ms release for pumping glue

  EEPROM.begin(256);
  for (int i=0; i<8; i++) {
    EEPROM.get(i * sizeof(DrumParams), params[i]);
  }
  
  uint8_t grids_val;
  EEPROM.get(150, grids_val); // Shifted grids save slot
  is_grids_mode = (grids_val == 1);

  for(int i=0; i<8; i++) {
    if (isnan(params[i].p1) || params[i].p1 < 0.0f || params[i].p1 > 1.0f) params[i].p1 = 0.5f;
    if (isnan(params[i].p2) || params[i].p2 < 0.0f || params[i].p2 > 1.0f) params[i].p2 = 0.5f;
  }

  ps[0].locked = false;
  ps[1].locked = false;

  updateSynthesisParams();

  led.begin(); led.setBrightness(180); updatePageLED();

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
  pinMode(0, INPUT); attachInterrupt(digitalPinToInterrupt(0), isr_gpio0, RISING);
  pinMode(28, INPUT); attachInterrupt(digitalPinToInterrupt(28), isr_gpio28, RISING);
  
  pinMode(6, INPUT_PULLUP);
}

void loop() {
  static bool currBtn = HIGH;
  static uint32_t last_debounce = 0;
  bool rawBtn = digitalRead(6);
  
  if (rawBtn != currBtn && (millis() - last_debounce > 20)) {
      currBtn = rawBtn;
      last_debounce = millis();
      
      if (currBtn == LOW) { // Button physically pressed
          btn_press_start = millis();
          btn_long_pressed = false;
      } else { // Button physically released
          if (!btn_long_pressed) {
              uint32_t duration = millis() - btn_press_start;
              
              // Ext Audio mode: only FX pages (3-6) are accessible
              if (current_kit == 14) {
                  // Pages 3=Delay, 4=Room, 5=MasterDJ, 6=Kit — 4 pages
                  int fx_pages = 4;
                  int fx_offset = 3; // starts at page 3
                  int rel = currentPage - fx_offset;
                  if (duration > 350) {
                      rel = (rel - 1 + fx_pages) % fx_pages;
                  } else {
                      rel = (rel + 1) % fx_pages;
                  }
                  currentPage = fx_offset + rel;
              } else {
                  int max_pages = is_grids_mode ? 8 : 7;
                  if (duration > 350) {
                      // Medium Press (Go Back)
                      currentPage = (currentPage - 1 + max_pages) % max_pages;
                  } else {
                      // Short Press (Go Forward)
                      currentPage = (currentPage + 1) % max_pages;
                  }
                  if (!is_grids_mode && currentPage >= 7) currentPage = 0;
              }
              
              float p1_current = global_a0_val / 4095.0f;
              float p2_current = global_a1_val / 4095.0f;
              ps[0].locked_val = p1_current; ps[0].locked = true;
              ps[1].locked_val = p2_current; ps[1].locked = true;
              
              updatePageLED();
          }
      }
  }
  
  // Continuous check for Long Press (so it triggers exactly at 1 second)
  // Ext Audio mode: long press does nothing (no grids/drum mode to toggle)
  if (currBtn == LOW && !btn_long_pressed && (millis() - btn_press_start > 1000)) {
      btn_long_pressed = true;
      if (current_kit == 14) { updatePageLED(); } // absorb without toggling
      else is_grids_mode = !is_grids_mode;
      
      // Save all parameters
      for(int i=0; i<8; i++) EEPROM.put(i * sizeof(DrumParams), params[i]);
      EEPROM.put(150, (uint8_t)(is_grids_mode ? 1 : 0));
      EEPROM.commit();
      needs_save = false;

      if (!is_grids_mode && currentPage >= 7) currentPage = 0; // fallback from grids page
      
      for(int i=0; i<3; i++) {
          led.setPixelColor(0, led.Color(255,255,255)); led.show(); delay(80);
          led.setPixelColor(0, led.Color(0,0,0)); led.show(); delay(80);
      }
      updatePageLED();
  }

  // Auto-save check
  if (needs_save && (millis() - last_param_change_time > 5000)) {
      for(int i=0; i<8; i++) EEPROM.put(i * sizeof(DrumParams), params[i]);
      EEPROM.put(150, (uint8_t)(is_grids_mode ? 1 : 0));
      EEPROM.commit();
      needs_save = false;
  }

  float p1_val = global_a0_val / 4095.0f;
  float p2_val = global_a1_val / 4095.0f;
  checkPickup(0, p1_val, params[currentPage].p1);
  checkPickup(1, p2_val, params[currentPage].p2);

  updateSynthesisParams();

  if (ledTrigReq && current_kit != 14) { // Ext Audio: suppress trigger flash
    ledTrigReq = false;
    ledTrigOn = true;
    ledTrigOffAt = millis() + 40;
    led.setPixelColor(0, led.Color(50, 50, 50)); // Dimmer white flash
    led.show();
  } else if (current_kit == 14) {
    ledTrigReq = false; // drain without showing
  } else if (ledTrigOn && millis() >= ledTrigOffAt) {
    ledTrigOn = false;
    updatePageLED();
  }
  
  if (kit_color_timer > 0 && (millis() - kit_color_timer > 600)) {
      kit_color_timer = 0;
      if (currentPage == 6 && !ledTrigOn) updatePageLED();
  }

  delay(10);
}
