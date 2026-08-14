/* 
 * RALPS - Random Algorithmic Looping Percussion Synthesizer
 * Version 1.0 (Release Milestone - 2026)
 * Hardware: RP2350 (Pico 2)
 */

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h" // Spinlocks for thread-safe memory access
#include "pico/multicore.h"
#include <math.h>
#include <EEPROM.h>

#include <Adafruit_NeoPixel.h>

extern bool system_hw_is_legacy; // Shared global defined in DUAL_CORE_OS.ino

#define R_NEOPIXEL_PIN 5
extern Adafruit_NeoPixel systemStrip;
#define r_strip systemStrip
const uint32_t r_enginePalette[16] = {
    0xFF0000, 0xB40000, 0xFF8000, 0xFFFF00, 
    0x8000FF, 0xFF7F50, 0x00FFFF, 0x00FF00, 
    0x80FF00, 0xFF00FF, 0x0000FF, 0x008080, 
    0xFFFFFF, 0xFFBF00, 0xEE82EE, 0x87CEEB  
};
volatile int r_currentVU = 0;
volatile uint8_t r_activeLED_Engine = 0;
uint32_t r_lastNeopixelUpdate = 0;

/* --- CONFIG & PIN DEFINITIONS --- */
const int R_PIN_RAND_TRIG  = 0; 
const int R_PIN_BTN        = 6;
const int R_PIN_SOUND_TRIG = 7;

#ifndef F_CPU
#define F_CPU 150000000
#endif
const uint32_t R_SYSTEM_CLK = F_CPU; 
const uint32_t R_PWM_TOP    = 4095; 
const float R_AUDIO_FS      = (float)R_SYSTEM_CLK / (float)(R_PWM_TOP + 1); 
const uint32_t R_BUFFER_SZ  = 16000; 
const int16_t R_PWM_MID     = 2048; 
const uint32_t R_JIT_CHUNK = 512;

/* --- CORE SYSTEM MEMORY --- */
static int16_t r_rawBuffers[5][R_BUFFER_SZ];
static int16_t* volatile r_voiceBuffers[4]; 
static int16_t* r_scratchBuffer;

static float r_ksBuffer[4096]; 
static float r_delayBuffer[4096]; 
static float r_SINE_LUT[2049]; 

struct R_Voice {
    bool active;
    uint32_t playHead;
    uint32_t stopHead;
    uint32_t timestamp; 
};
static volatile R_Voice r_voices[4];
static spin_lock_t* r_voiceLock; 

static volatile float r_jDecay = 0.5f, r_jTimbre = 0.5f, r_jFX = 0.0f;
static volatile bool r_isCalculating = false;
volatile uint32_t r_activeId = 0; 
volatile uint32_t r_globalCounter = 0;
// ... (Rest of structs)

struct Mapping { uint8_t target; float minVal, maxVal; };
struct ChaosPatch {
    uint8_t engine; 
    float fixedP[6]; 
    Mapping maps[4]; // maps target 0-5
    uint8_t cluster[6]; // [0..2] assigned to Pot 2, [3..5] assigned to Pot 3
    float lfoRate; float lfoDepth;
};
ChaosPatch slots[8];
// Active patch buffers (Ping-Pong for glitch-free updates)
ChaosPatch activePatches[2]; 
volatile int ppIdx = 0;

volatile int r_currentSlot = 0;
volatile int currentMode = 0; 
uint32_t r_lastExternalTrigTime = 0; // Tracks last activity on Trig inputs

// SYSTEM CONFIG (Shift Mode)
int sys_slot_limit = 8;
float sys_random_bias = 0.5f;
int sys_groove_select = 0;
bool r_isShiftAction = false;
volatile bool r_sys_btn_held = false;
float r_bSnap[3]; // A0, A2, A1
bool r_lastBtn = false;
uint32_t r_bTime = 0;
uint32_t r_stepCounter = 0;

bool r_lT1=0, r_lT2=0; uint32_t r_lastADC = 0; uint32_t r_debounceTrig1 = 0;
uint32_t r_lastHeartbeat = 0; 
uint8_t r_lastShiftPoti = 255;
float r_lastStablePotiVal[3] = {0, 0, 0};
uint32_t r_lastPotiMoveTime = 0;

uint32_t r_shiftBlinkTimer = 0;
bool r_isShiftBlinkActive = false;

const uint8_t grooves[16][16] = {
    {1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8}, // Linear (Fractured)
    {1,2,3,4,5,6,7,8,1,2,3,4,7,7,8,8}, // NEW 2
    {1,2,3,4,1,2,5,6,1,2,3,4,1,2,7,8}, // NEW 1
    {1,3,2,5,1,7,4,3,5,1,2,7,3,5,1,7}, // Backbeat (Fractal)
    {1,2,5,3,1,4,8,7,5,2,1,3,1,6,4,7}, // Ghost (Fractal)
    {1,2,3,5,6,7,1,4,3,5,2,7,1,4,3,5}, // Poly (Fractal)
    {1,2,3,5,6,7,1,4,1,2,7,5,8,3,4,5}, // NEW 3
    {1,2,4,3,6,8,5,2,1,4,6,7,2,8,3,4}, // Terc (Fractal)
    {1,5,2,3,1,4,6,5,7,1,2,7,5,8,2,1}, // Break 1 (Fractal)
    {1,3,5,2,7,1,3,4,5,7,1,6,3,5,7,8}, // Swing 1 (Fractal)
    {1,2,3,5,6,1,4,7,1,2,3,1,4,5,6,7}, // Odd Pulse (Fractal)
    {1,3,5,4,7,1,5,3,1,7,3,8,5,1,7,2}, // Kick Fill (Fractal)
    {1,5,6,7,1,8,2,3,5,1,4,7,1,8,6,5}, // Break 2 (Fractal)
    {1,5,3,7,1,5,7,3,1,3,5,7,1,5,8,4}, // Swing 2 (Fractal)
    {1,5,2,3,7,1,5,3,1,5,6,7,3,4,1,2}, // Odd Pulse 2 (Fractal)
    {1,5,2,6,3,7,4,8,1,5,2,6,5,1,8,4}  // Accent (Fractal)
};
// System Configuration
const bool ENABLE_SERIAL_DEBUG = true;

uint32_t r_sliceAudio, r_sliceIRQ, r_sliceLED;
uint32_t r_seed = 0;

void r_setCustomLED(uint32_t color, int brightness) {
    if(brightness > 255) brightness = 255;
    if(!system_hw_is_legacy) {
        uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
        uint8_t g = ((color >>  8) & 0xFF) * brightness / 255;
        uint8_t b = ((color      ) & 0xFF) * brightness / 255;
        r_strip.setPixelColor(0, r_strip.Color(r, g, b));
        r_strip.show();
    } else {
        pwm_set_chan_level(r_sliceLED, PWM_CHAN_B, brightness * 4); 
    }
}

void r_setSystemLED(int brightness) {
    if(!system_hw_is_legacy) {
        r_setCustomLED(r_enginePalette[r_activeLED_Engine], brightness);
    } else {
        r_setCustomLED(0, brightness);
    }
}

uint32_t lerpColor(uint32_t c1, uint32_t c2, float t) {
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r = (uint8_t)(r1 + (r2 - r1) * t);
    uint8_t g = (uint8_t)(g1 + (g2 - g1) * t);
    uint8_t b = (uint8_t)(b1 + (b2 - b1) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void r_runStartupAnimation() {
    if(!system_hw_is_legacy) {
        // Ambient Glow Startup: Cross-fade through current kit colors
        uint32_t lastColor = 0; 
        for(int s=0; s<8; s++) {
            uint32_t targetColor = r_enginePalette[slots[s].engine];
            for(int step=0; step<20; step++) {
                float t = (float)step / 20.0f;
                uint32_t c = lerpColor(lastColor, targetColor, t);
                float pulse = 0.7f + 0.3f * sinf(t * PI);
                r_setCustomLED(c, (int)(255 * pulse));
                delay(18);
            }
            lastColor = targetColor;
        }
        for(int b=255; b>=0; b-=15) { r_setCustomLED(lastColor, b); delay(10); }
        r_setSystemLED(0);
    } else {
        // Legacy Startup: Quick brightness sweep
        for(int i=0; i<255; i+=8) { r_setCustomLED(0, i); delay(15); }
        for(int i=255; i>=0; i-=8) { r_setCustomLED(0, i); delay(15); }
    }
}

void r_loadHardwareConfig() {
    EEPROM.begin(512);
    uint8_t val = EEPROM.read(0);
    if(val == 1) system_hw_is_legacy = true;
    else if(val == 0) system_hw_is_legacy = false;
    else {
        system_hw_is_legacy = false;
        EEPROM.write(0, 0);
        EEPROM.commit();
    }
}

void serviceMenu() {
    // Check if button is held at boot
    if(digitalRead(R_PIN_BTN) == HIGH) return; 

    uint32_t startTime = millis();
    bool selectionMade = false;
    
    bool last_mode_in_menu = system_hw_is_legacy;
    while(digitalRead(R_PIN_BTN) == LOW || (millis() - startTime < 1500)) {
        watchdog_update();
        float p1 = analogRead(A0) / 1023.0f;
        
        bool current_mode_selection = (p1 > 0.5f);
        if (current_mode_selection != last_mode_in_menu) {
            // Re-configure pin function on state change
            if (current_mode_selection) { // Switch to Legacy
                gpio_set_function(5, GPIO_FUNC_PWM);
                pwm_set_wrap(r_sliceLED, 1023); pwm_set_enabled(r_sliceLED, true);
            } else { // Switch to NeoPixel
                r_strip.begin();
            }
            last_mode_in_menu = current_mode_selection;
        }

        if(!current_mode_selection) {
            // NEOPIXEL MODE: Slow Blue Heartbeat
            system_hw_is_legacy = false;
            float pulse = 0.5f + 0.5f * sinf(millis() * 0.005f);
            r_setCustomLED(0x0000FF, (int)(pulse * 255));
        } else {
            // LEGACY MODE: Rapid White Strobe
            system_hw_is_legacy = true;
            bool blink = (millis() % 100) < 50;
            r_setCustomLED(0, blink ? 255 : 0);
        }
        
        if(digitalRead(R_PIN_BTN) == HIGH && millis() - startTime > 500) {
            selectionMade = true;
            break; 
        }
        delay(10);
    }
    
    // Save Selection
    EEPROM.write(0, system_hw_is_legacy ? 1 : 0);
    EEPROM.commit();
    
    // Success Feedback
    for(int i=0; i<3; i++) {
        r_setCustomLED(0xFFFFFF, 255); delay(50);
        r_setCustomLED(0x000000, 0); delay(50);
    }
    delay(500);
}

uint32_t r_getPastelColor(uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    // Mix with 20% White for a more saturated 'premium glow' look
    r = (uint8_t)(r * 0.80f + 51); g = (uint8_t)(g * 0.80f + 51); b = (uint8_t)(b * 0.80f + 51);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ==========================================
   CORE 1: DSP WORKER
   ========================================== */
uint32_t r_xrand() { 
    if(r_seed == 0) r_seed = 1; // Prevent state lock
    r_seed ^= r_seed << 13; r_seed ^= r_seed >> 17; r_seed ^= r_seed << 5; 
    return r_seed; 
}
float r_randf() { return (float)(r_xrand() % 10000) / 10000.0f; }

inline float r_fast_sinf(float ph) {
    float idx_f = ph * (2048.0f / TWO_PI);
    int idx = (int)idx_f & 2047;
    float frac = idx_f - (float)(int)idx_f;
    return r_SINE_LUT[idx] * (1.0f - frac) + r_SINE_LUT[idx + 1] * frac;
}

inline float r_fast_tanhf(float x) {
    float ax = (x < 0) ? -x : x;
    return x / (1.0f + ax);
}

// Optimized Integer Sine (Phase 4 Zero-Branch)
inline float r_sin_u32(uint32_t ph) {
    uint32_t idx = ph >> 21; // 32 - 11 = 21 bits
    float frac = (float)(ph & 0x1FFFFF) * 4.768371582e-7f; // 1 / 2^21
    return r_SINE_LUT[idx] * (1.0f - frac) + r_SINE_LUT[idx+1] * frac;
}

float r_mapVal(float in, Mapping m) {
    return m.minVal + in * (m.maxVal - m.minVal);
}

// Deprecated in V1.2. Cluster-based logic used instead in loop() for Shift Mode.

int16_t* r_activateVoice(uint32_t len) {
    uint32_t save = spin_lock_blocking(r_voiceLock); // LOCK (V8.1)
    
    int oldestIdx = 0; uint32_t minTime = 0xFFFFFFFF; int targetIdx = -1;
    for(int i=0; i<4; i++) { if(!r_voices[i].active) { targetIdx = i; break; } }
    if(targetIdx == -1) {
        for(int i=0; i<4; i++) { if(r_voices[i].timestamp < minTime) { minTime=r_voices[i].timestamp; oldestIdx=i; } }
        targetIdx = oldestIdx;
    }
    int16_t* oldBuf = r_voiceBuffers[targetIdx];
    r_voiceBuffers[targetIdx] = r_scratchBuffer; r_scratchBuffer = oldBuf;
    r_voices[targetIdx].playHead = 0; r_voices[targetIdx].stopHead = len;
    r_voices[targetIdx].timestamp = ++r_globalCounter; 
    r_voices[targetIdx].active = true;
    
    spin_unlock(r_voiceLock, save); // UNLOCK
    return r_voiceBuffers[targetIdx];
}
#ifndef PI
#define PI 3.14159265359f
#endif

// TURBO-DSP: Fast Math Helpers
void generateJIT(int16_t* &bufferPtr, ChaosPatch p, float pD, float pT, float pF, uint32_t myId) {
    float P[6]; for(int i=0; i<6; i++) P[i] = p.fixedP[i];
    
    // AUDIO SANITIZATION
    // Zero the entire target buffer at the start. Since generateJIT calculates sequentially,
    // this ensures that any early abort (r_activeId mismatch) or shorter sound length
    // results in silence instead of 'fragments' of old sounds left in the buffer.
    memset(bufferPtr, 0, R_BUFFER_SZ * sizeof(int16_t));

    // Pot Mapping: Tmb (pT) -> maps[0,1], FX (pF) -> maps[2,3]
    for(int k=0; k<2; k++) {
        if(p.maps[k].target < 6) P[p.maps[k].target] = r_mapVal(pT, p.maps[k]);
    }
    for(int k=2; k<4; k++) {
        if(p.maps[k].target < 6) P[p.maps[k].target] = r_mapVal(pF, p.maps[k]);
    }
    
    uint32_t len = 600 + (uint32_t)(pD * (R_BUFFER_SZ - 601));
    if(len > R_BUFFER_SZ) len = R_BUFFER_SZ; 
    // Minimum duration safety
    if(len < 800) len = 800; 

    float invFs = 1.0f / R_AUDIO_FS;
    float ph = 0, ph2 = 0, ph3 = 0, ph4 = 0;
    
    
    // V8.5: Delay buffer resets handled per-engine if(i==0)

    bool swapped = false;
    float lastS = 0.0f; 

    // V2.9 FIX: Engine State Variables (Must be outside loop)
    // NOISE State
    float b0=0, b1=0, b2=0; 
    float currentL = 0; // Filter state
    float lastS_Redux = 0; // Downsample hold

    // BYTE State
    uint32_t sr = 0xACE1; 

    for(uint32_t i=0; i<len; ++i) {
        // Stability tuning for complex engines
        bool heavy = (p.engine == 7 || p.engine == 9);
        if(!heavy && r_activeId != myId) return; 
        if(!swapped && i == R_JIT_CHUNK) { bufferPtr = r_activateVoice(len); swapped = true; }
        if(i == 0) { memset((void*)r_ksBuffer, 0, sizeof(r_ksBuffer)); }

        float invLen = 1.0f / (float)len;
        float prog = (float)i * invLen; 
        float env = 1.0f - prog; if(env < 0.001f) env = 0.0f;
        // Quadratic envelope for musical decay tails
        float envE = env * env; 
        
        float warp = 0; 
        float lfo = 0.0f; // sinf(i*p.lfoRate*TWO_PI*invFs)*p.lfoDepth; // DEACTIVATED (Test)
        float pitchMod = 1.0f; 
        
        // PITCH SCALING: Convert normalized mapping to Hz
        float baseFreq = P[0];
        if(baseFreq < 20.0f) {
            if(p.engine == 3) baseFreq = 30.0f * powf(100.0f, baseFreq); // TUBE: 30Hz to 3000Hz Exp
            else baseFreq = 40.0f + baseFreq * 2000.0f;
        }

        float f = constrain((baseFreq + warp) * pitchMod, 20.0f, 12000.0f);
        float inc = f * TWO_PI * invFs;
        float s = 0.0f;
        
        switch(p.engine) {
            case 0: { // PLONK: Modal Resonator (Physical Model)
                        if(i==0) { 
                            float f0 = (baseFreq * 0.8f) * invFs; 
                            float structure = P[5]; 
                            float stiffness = (structure * 2.0f - 1.0f) * 0.2f; 
                            float harmonic = f0;
                            float stretch_factor = 1.0f;
                            
                            float q = 500.0f * powf(10.0f, P[4] * 3.5f); 
                            
                            float brightness_att = 1.0f - structure;
                            brightness_att *= brightness_att * brightness_att * brightness_att;
                            float brightness = P[1] * (1.0f - 0.2f * brightness_att);
                            float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
                            float q_loss_damping_rate = structure * (2.0f - structure) * 0.1f;
                            
                            float position = P[2];
                            
                            for(int m=0; m<6; m++) {
                                float partial_frequency = harmonic * stretch_factor;
                                if (partial_frequency >= 0.49f) partial_frequency = 0.49f; 
                                
                                float resonance = 1.0f + partial_frequency * q;
                                float gq = 1.0f / resonance;
                                float gf = 2.0f * sinf(PI * partial_frequency); 
                                if(gf > 1.4f) gf = 1.4f;
                                
                                float phase = PI * position * (float)(m + 1);
                                float amplitude = cosf(phase);
                                
                                r_ksBuffer[1040 + m] = gf;
                                r_ksBuffer[1046 + m] = gq;
                                r_ksBuffer[1052 + m] = amplitude;
                                
                                stretch_factor += stiffness;
                                if (stiffness < 0.0f) {
                                    stiffness *= 0.93f;
                                } else {
                                    stiffness *= 0.98f;
                                }
                                q_loss += q_loss_damping_rate * (1.0f - q_loss);
                                harmonic += f0;
                                q *= q_loss;
                            }
                        }
                        
                        float pBase = baseFreq * 0.8f;
                        int pIdx = (int)r_ksBuffer[14]; 
                        
                        float pDelay = 36621.0f / pBase;
                        if (pDelay > 1023.0f) pDelay = 1023.0f;
                        if (pDelay < 2.0f) pDelay = 2.0f;
                        
                        int noiseLength = 10 + (int)(P[3] * 60.0f); 
                        float in = (i < noiseLength) ? (r_randf() * 2.0f - 1.0f) * 0.8f : 0.0f;
                        
                        int dInt = (int)pDelay;
                        float dFrac = pDelay - (float)dInt;
                        int rIdx1 = (pIdx - dInt + 1024) & 1023;
                        int rIdx2 = (rIdx1 - 1 + 1024) & 1023;
                        float dRead = r_ksBuffer[16 + rIdx1] * (1.0f - dFrac) + r_ksBuffer[16 + rIdx2] * dFrac;
                        
                        float pluckCutoff = 0.05f + (1.0f - P[3]) * 0.95f; 
                        float lpfPluck = r_ksBuffer[15];
                        lpfPluck += pluckCutoff * (dRead - lpfPluck);
                        r_ksBuffer[15] = lpfPluck;
                        
                        float pluckFeedback = 0.9f - P[3] * 0.5f; 
                        float outPluck = in + lpfPluck * pluckFeedback;
                        r_ksBuffer[16 + pIdx] = outPluck;
                        
                        pIdx = (pIdx + 1) & 1023;
                        r_ksBuffer[14] = (float)pIdx;
                        
                        float exciter = outPluck * 1.0f;
                        if (i < 2) exciter += 0.5f; 
                        
                        float input = exciter * 0.75f;
                        float tot = 0.0f;
                        
                        for(int m=0; m<6; m++) {
                            float gf = r_ksBuffer[1040 + m];
                            float gq = r_ksBuffer[1046 + m];
                            float amplitude = r_ksBuffer[1052 + m];
                            
                            float bp = r_ksBuffer[m*2]; 
                            float lp = r_ksBuffer[m*2+1];
                            float nEx = input - (bp * gq) - lp; 
                            bp += nEx * gf; 
                            lp += bp * gf;
                            
                            if(isnan(bp) || isinf(bp)) bp = 0; 
                            if(isnan(lp) || isinf(lp)) lp = 0;
                            
                            r_ksBuffer[m*2] = bp; 
                            r_ksBuffer[m*2+1] = lp; 
                            
                            tot += bp * amplitude;
                        }
                        
                        s = tot * 3.5f; 
                        s = tanhf(s) * 0.7f;
                      } break;
            case 1: { // PLUCK: Karplus-Strong Physical String
                        if(i==0) {
                            r_ksBuffer[2043] = 1.0f + (r_randf() * 2.0f - 1.0f) * 0.003f; // +/- 0.3% Pitch Drift
                            r_ksBuffer[2044] = r_randf(); // Wow Phase
                        }
                        float wowMod = sinf(r_ksBuffer[2044] * TWO_PI) * P[4] * 0.005f;
                        r_ksBuffer[2044] += 1.8f * invFs; if(r_ksBuffer[2044] >= 1.0f) r_ksBuffer[2044] -= 1.0f;
                        float instFreq = f * r_ksBuffer[2043] * (1.0f + wowMod);
                        float stiffness = 0.1f + P[1]*0.89f; float pick = 0.95f - P[2]*0.9f; float damping = 0.96f + P[3]*0.0395f;
                        float delayTime = R_AUDIO_FS / instFreq; if(delayTime < 2.0f) delayTime = 2.0f; if(delayTime > 1023.0f) delayTime = 1023.0f;
                        float exciter = 0.0f; if(i < 120) {
                            float ramp = 1.0f - (float)i/120.0f; float noise = (r_randf()*2.0f-1.0f); float p_z = r_ksBuffer[2042];
                            float p_out = (1.0f-pick)*noise + pick*p_z; r_ksBuffer[2042] = p_out; 
                            float pulse = sinf((float)i*0.5f)*ramp*0.25f; exciter = (p_out+pulse)*ramp;
                        }
                        int wHead = (int)r_ksBuffer[2045]; float rPos = (float)wHead - delayTime; while(rPos < 0.0f) rPos += 1024.0f;
                        int rInt = (int)rPos; float rFrac = rPos - rInt;
                        float delayed = r_ksBuffer[rInt]*(1.0f-rFrac) + r_ksBuffer[(rInt+1)&1023]*rFrac;
                        float l_z = r_ksBuffer[2041]; float l_out = tanhf((l_z + stiffness*(delayed-l_z))*1.02f); r_ksBuffer[2041] = l_out;
                        float sig = exciter + l_out * damping; if(sig > 1.8f) sig = 1.8f; if(sig < -1.8f) sig = -1.8f;
                        r_ksBuffer[wHead] = sig * 0.98f; r_ksBuffer[2045] = (float)((wHead+1)&1023);
                        float hpf_z = r_ksBuffer[2040]; float lpf = hpf_z + (0.01f + P[5]*0.25f)*(sig - hpf_z); r_ksBuffer[2040] = lpf;
                        s = (sig - lpf) * (2.0f + P[5]*2.5f);
                      } break;
            case 2: { // WOODY: Resonant Wood Model
                       if(i==0) { /* state already cleared */ }
                       float exciter = 0.0f; if(i < 30) { float envHit = (1.0f-(float)i/30.0f); exciter = (r_randf()*2.0f-1.0f)*envHit*(0.15f + P[2]*0.85f); }
                       float r[4] = {1.0f, 3.9f, 9.2f, 16.0f};
                       float detune = P[4] * 0.05f; 
                       float a[4]; a[0] = 1.0f - P[3]*0.7f; a[1] = 0.2f + P[3]*0.5f; a[2] = 0.05f + P[3]*0.4f; a[3] = 0.01f + P[3]*0.2f;
                       float dampQ = 300.0f + P[5] * 1200.0f; float tot = 0.0f;
                       for(int m=0; m<4; m++) {
                           float modeF = f * r[m] * (1.0f + (m>0?detune*m:0.0f)); if(modeF > 9000.0f) modeF = 9000.0f;
                           float modeQ = dampQ / (1.0f + m * 2.0f); float gf = 2.0f * sinf(PI * modeF * invFs); if(gf > 1.4f) gf = 1.4f;
                           float gq = 1.0f / modeQ; float bp = r_ksBuffer[m*2]; float lp = r_ksBuffer[m*2+1];
                           float nEx = exciter * a[m] - (bp * gq) - lp; bp += nEx*gf; lp += bp*gf;
                           if(isnan(bp) || isinf(bp)) bp = 0; if(isnan(lp) || isinf(lp)) lp = 0;
                           r_ksBuffer[m*2] = bp; r_ksBuffer[m*2+1] = lp; tot += bp;
                       }
                       s = tot * 2.0f;
                     } break;
            case 3: { // TUBE: Physical Pipe Simulation
                       if(i==0) { r_ksBuffer[2044]=0; }
                       float stiffness = 0.02f + P[1]*P[1]*0.35f; float damping = 0.94f+P[2]*0.059f; float geometry = 0.05f+P[3]*0.9f;
                       float exciter = 0.0f; if(i < 300) { // Powerful Exciter (~8ms at FS)
                           float envE = (1.0f - (float)i/300.0f);
                           float body = sinf((float)i * PI / 300.0f) * 0.8f; // Bass/Body
                           float noise = (r_randf()*2.0f-1.0f) * envE * 0.6f;
                           float p_z = r_ksBuffer[2044]; 
                           // P[4] controls Noise mix (soft to hard)
                           float p_out = p_z + (0.05f + (1.0f-P[4])*0.5f)*(noise - p_z);
                           r_ksBuffer[2044] = p_out; 
                           exciter = (body * P[4] + p_out * (1.0f - P[4])) * envE * 2.0f;
                       }
                       // Increased Max Delay for deeper resonances (down to ~18Hz)
                       float delayTime = R_AUDIO_FS / f; if(delayTime < 2.0f) delayTime = 2.0f; if(delayTime > 2040) delayTime = 2040;
                       int wHead = i % 2048; float rPos = (float)wHead - delayTime; while(rPos < 0.0f) rPos += 2048.0f;
                       int rInt = (int)rPos; float rFrac = rPos - rInt;
                       float delayed = r_ksBuffer[rInt]*(1.0f-rFrac) + r_ksBuffer[(rInt+1)%2048]*rFrac;
                       float l_z = r_ksBuffer[2041]; float l_out = l_z + stiffness*(delayed-l_z); r_ksBuffer[2041] = l_out;
                       float sig = exciter + l_out * damping;
                       if(geometry > 0.5f) { float drive = 1.0f + (geometry-0.5f)*4.0f; sig = tanhf(sig*drive); }
                       if(sig > 2.0f) sig = 2.0f; if(sig < -2.0f) sig = -2.0f;
                       r_ksBuffer[wHead] = sig; float dcY = r_ksBuffer[2042]; float dcX = r_ksBuffer[2043];
                       float dcOut = sig - dcX + 0.995f * dcY; r_ksBuffer[2042] = dcOut; r_ksBuffer[2043] = sig;
                       s = (dcOut * (1.0f - P[5]) + (exciter * P[5] * 2.0f)) * 2.5f; // Extra gain for impact
                     } break;
            case 4: { // PWM: Pulse-Width Modulation Oscillator
                       ph+=inc; ph2+=inc*(0.5f+(P[1]+P[5])*12); 
                       float pw = P[3] + sinf(i*0.001f)*P[4]*0.2f;
                       s=sinf(ph)*(sinf(ph2)>(pw-0.5f)?1.0f:-1.0f); 
                       if(P[2] > 0.01f) { float sub = (sinf(ph*0.5f)>0)?1.0f:-1.0f; s += sub * P[2] * 0.7f; }
                     } break; 
            case 5: { // BOUNCE: Bouncing Ball Physics Model
                        if(i==0) {
                            float fastStart = 60.0f + P[1] * 240.0f; 
                            float slowStart = 2.0f + P[1] * 40.0f;
                            float initHz = (P[2] < 0.5f) ? (slowStart + P[2]*2.0f*(10.0f-slowStart)) : (10.0f + (P[2]-0.5f)*2.0f*(fastStart-10.0f));
                            r_ksBuffer[3] = R_AUDIO_FS / initHz; // Initial Interval
                            r_ksBuffer[4] = 0.0f; // Fire first ping immediately
                            r_ksBuffer[2046] = 0.96f + r_randf() * 0.08f; 
                            for(int k=0; k<4; k++) { r_ksBuffer[14+k]=0; r_ksBuffer[10+k]=0; r_ksBuffer[20+k]=0; r_ksBuffer[24+k]=0; }
                        }

                        float seqBaseFreq = f * r_ksBuffer[2046];
                        float gravity = 1.0f - (P[2] - 0.5f) * 0.25f; 
                        float pDamping = 0.997f - (1.0f - P[5]) * 0.035f; 

                        for(uint32_t i=0; i<len; ++i) {
                            if(r_activeId != myId) break; 
                            float prog = (float)i * invLen; float env = 1.0f - prog;
                            if(env < 0.001f) env = 0.0f;

                            // SCHEDULER: Check for sub-trigger
                            if(r_ksBuffer[4] <= 0.0f && env > 0.03f) {
                                for(int v=0; v<4; v++) {
                                    if(r_ksBuffer[14+v] < 0.01f) {
                                        r_ksBuffer[14+v] = 1.0f; // Env
                                        r_ksBuffer[10+v] = 0; // Phase
                                        r_ksBuffer[20+v] = 0; r_ksBuffer[24+v] = 0; // Filter
                                        r_ksBuffer[28+v] = 0.994f + r_randf() * 0.012f; // Jitter
                                        break;
                                    }
                                }
                                r_ksBuffer[4] = r_ksBuffer[3]; r_ksBuffer[3] *= gravity;
                                if(r_ksBuffer[3] < 25.0f) r_ksBuffer[3] = 25.0f;
                                if(r_ksBuffer[3] > R_AUDIO_FS) r_ksBuffer[3] = R_AUDIO_FS;
                            }
                            r_ksBuffer[4] -= 1.0f;

                            float out = 0.0f;
                            float seqPitchDrop = 1.0f - (1.0f - env) * P[3] * 0.6f;

                            for(int v=0; v<4; v++) {
                                if(r_ksBuffer[14+v] > 0.005f) {
                                    float vEnv = r_ksBuffer[14+v];
                                    float vEnvE = vEnv * vEnv; // Quadratic for body
                                    float instFreq = seqBaseFreq * seqPitchDrop * (0.94f + vEnv * 0.06f) * r_ksBuffer[28+v];
                                    
                                    // Update Voice Parameters decimated
                                    if((i & 15) == 0 || i == 0) {
                                         float gf = 2.0f * r_fast_sinf(PI * instFreq * invFs); if(gf > 1.4f) gf = 1.4f;
                                         r_ksBuffer[32+v] = gf;
                                         r_ksBuffer[36+v] = 1.0f / (1.5f + P[4] * 18.0f); // Reso
                                    }

                                    // Integer Phase
                                    uint32_t ph_u = (uint32_t)(r_ksBuffer[10+v]);
                                    ph_u += (uint32_t)(instFreq * invFs * 4294967296.0f);
                                    r_ksBuffer[10+v] = (float)ph_u;

                                    // Meatier OSC: Sine + Sub-Sine
                                    float osc = r_sin_u32(ph_u) + 0.4f * r_sin_u32(ph_u >> 1);
                                    float noise = (r_randf() * 2.0f - 1.0f) * (i < 240 ? 0.3f : 0.0f);
                                    float input = (osc + noise) * vEnvE;

                                    // Resonant SVF
                                    float lp = r_ksBuffer[20+v], bp = r_ksBuffer[24+v];
                                    float nEx = input - (bp * r_ksBuffer[36+v]) - lp;
                                    bp += nEx * r_ksBuffer[32+v]; lp += bp * r_ksBuffer[32+v];
                                    r_ksBuffer[20+v] = lp; r_ksBuffer[24+v] = bp;

                                    out += bp;
                                    r_ksBuffer[14+v] *= pDamping;
                                }
                            }
                            float sFinal = out * 2.8f;
                            s = r_fast_tanhf(sFinal) * 0.95f;

                            float outVal = s * env * env; // Final master envelope
                            if(outVal > 1.0f) outVal = 1.0f; if(outVal < -1.0f) outVal = -1.0f;
                            if(!swapped && i == R_JIT_CHUNK) { bufferPtr = r_activateVoice(len); swapped = true; }
                            bufferPtr[i] = (int16_t)(outVal * 2040.0f);
                        }
                        goto jit_done;
                      } break;
            case 6: { // RESO2: Dual Resonant Filter Bank
                        if(i==0) { /* state already cleared */ }
                        
                        float drive = 1.0f + P[5] * 19.0f; 
                        float noiseMix = P[4]; 
                        float freq1 = f;
                        float freq2 = f * (0.5f + P[2] * 4.0f); 
                        if(freq1 > 16000.0f) freq1 = 16000.0f;
                        if(freq2 > 16000.0f) freq2 = 16000.0f;

                        float f1 = 2.0f * sinf(PI * freq1 * invFs);
                        float f2 = 2.0f * sinf(PI * freq2 * invFs);
                        float q = 0.0001f + powf(1.0f - P[1], 4.0f) * 0.4f;

                        float noise = (r_randf()*2.0f-1.0f);
                        float pulse = (i < 60) ? (1.0f - (float)i/60.0f) : 0.0f;
                        float exciter = (pulse * (1.0f - noiseMix) + noise * pulse * noiseMix) * drive * 5.0f;

                        float lp1 = r_ksBuffer[0], bp1 = r_ksBuffer[1];
                        float hp1 = exciter - lp1 - q * bp1; 
                        bp1 += f1 * tanhf(hp1); lp1 += f1 * bp1;
                        r_ksBuffer[0] = lp1; r_ksBuffer[1] = bp1;

                        float lp2 = r_ksBuffer[2], bp2 = r_ksBuffer[3];
                        float hp2 = exciter - lp2 - q * bp2; 
                        bp2 += f2 * tanhf(hp2); lp2 += f2 * bp2;
                        r_ksBuffer[2] = lp2; r_ksBuffer[3] = bp2;
                        
                        float mix2 = P[3];
                        s = (bp1 * (1.0f - mix2) + bp2 * mix2) * 1.5f;
                      } break;
            case 7: { // VOX: Vowel Morphing Synthesizer
                        if(i==0) {
                            r_ksBuffer[12] = r_randf(); // Random Vowel LFO Phase
                            r_ksBuffer[13] = 1.0f + (r_randf() * 2.0f - 1.0f) * 0.06f; // +/- 6% Format Shift
                            r_ksBuffer[14] = 1.0f + (r_randf() * 2.0f - 1.0f) * 0.01f; // +/- 1% Pitch Drift
                        }
                        float gq = 1.0f / 12.0f; float outPulse = 0.0f;
                        float shift = (0.4f + P[2] * 2.0f) * r_ksBuffer[13]; 
                        float pitchEnv = envE * env; // env^3 approach
                        float instFreq = f * (1.0f + P[4] * pitchEnv * 2.0f) * r_ksBuffer[14]; 
                        
                        // Phase 4: Integer Phasing for Pulse
                        uint32_t phPulse_u = (uint32_t)(r_ksBuffer[10] * 4294967296.0f);
                        uint32_t phLFO_u = (uint32_t)(r_ksBuffer[11] * 4294967296.0f);
                        float startStab = 0.2f; // Branchless start noise

                        for(uint32_t i=0; i<len; ++i) {
                            if(r_activeId != myId) break; 
                            float prog = (float)i * invLen; 
                            float env = 1.0f - prog; if(env < 0.001f) env = 0.0f;
                            float envE = env * env; 

                            float shift = (0.4f + P[2] * 2.0f) * r_ksBuffer[13]; 
                            float pitchEnv = envE * env; 
                            float instFreq = f * (1.0f + P[4] * pitchEnv * 2.0f) * r_ksBuffer[14]; 
                            
                            // Integer increment
                            phPulse_u += (uint32_t)(instFreq * invFs * 4294967296.0f);
                            float phase = (float)phPulse_u * 2.3283064365e-10f; // 1/2^32

                            float pInv = 1.0f - phase;
                            float pulseEnv = (pInv * pInv); pulseEnv *= pulseEnv; pulseEnv *= pulseEnv;
                            float pulse = pulseEnv * r_sin_u32(phPulse_u >> 1); // phase * PI = phase/2 * 2PI
                            
                            if(i == 200) startStab = 0.0f;
                            float exciter = pulse * 0.9f + (r_randf() * 2.0f - 1.0f) * (0.05f * env + startStab);

                            if((i & 15) == 0 || i == 0) {
                                float lfoHz = 0.5f + (P[3] * P[3] * P[3]) * 19.5f; 
                                phLFO_u += (uint32_t)(lfoHz * invFs * 16.0f * 4294967296.0f);
                                float lfoVal = r_sin_u32(phLFO_u + (uint32_t)(r_ksBuffer[12] * 4294967296.0f)); 

                                float vIdx = P[1] * 4.0f + lfoVal * P[5] * 2.0f; 
                                vIdx = constrain(vIdx, 0.0f, 3.999f);
                                int v0 = (int)vIdx; int v1 = v0 + 1; if(v1>4) v1=4;
                                float vFrac = vIdx - v0;

                                float vf[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {400, 840, 2800}, {300, 870, 2240}};
                                for(int m=0; m<3; m++) {
                                    float Fm = (vf[v0][m] * (1.0f - vFrac) + vf[v1][m] * vFrac) * shift;
                                    float g_val = 2.0f * r_fast_sinf(PI * Fm * invFs); 
                                    if(g_val > 1.4f) g_val = 1.4f;
                                    r_ksBuffer[20+m] = g_val; 
                                }
                            }

                            float g0 = r_ksBuffer[20], g1 = r_ksBuffer[21], g2 = r_ksBuffer[22];
                            float lp0 = r_ksBuffer[0], bp0 = r_ksBuffer[1], nEx0 = exciter - (bp0 * gq) - lp0;
                            bp0 += nEx0 * g0; lp0 += bp0 * g0; r_ksBuffer[0] = lp0; r_ksBuffer[1] = bp0;
                            float lp1 = r_ksBuffer[2], bp1 = r_ksBuffer[3], nEx1 = exciter - (bp1 * gq) - lp1;
                            bp1 += nEx1 * g1; lp1 += bp1 * g1; r_ksBuffer[2] = lp1; r_ksBuffer[3] = bp1;
                            float lp2 = r_ksBuffer[4], bp2 = r_ksBuffer[5], nEx2 = exciter - (bp2 * gq) - lp2;
                            bp2 += nEx2 * g2; lp2 += bp2 * g2; r_ksBuffer[4] = lp2; r_ksBuffer[5] = bp2;
                            
                            outPulse = bp0 + bp1 + bp2;
                            s = r_fast_tanhf(outPulse * 3.0f) * 0.9f;

                            float outVal = s * envE;
                            if(outVal > 1.0f) outVal = 1.0f; if(outVal < -1.0f) outVal = -1.0f;
                            if(!swapped && i == R_JIT_CHUNK) { bufferPtr = r_activateVoice(len); swapped = true; }
                            bufferPtr[i] = (int16_t)(outVal * 2040.0f);
                        }
                        r_ksBuffer[10] = (float)phPulse_u * 2.3283064365e-10f;
                        goto jit_done;
                      } break;
            case 8: { // FM2: 2-Operator FM Synthesizer
                       float ratio = 1.0f + P[1] * 15.0f;
                       float index = (P[2] + P[5]*env) * (env * env) * 12.0f;
                       float feedback = P[3] * 1.5f + P[4] * 0.5f;
                       ph += inc; ph2 += inc * ratio;
                       float m = sinf(ph2 + lastS * feedback);
                       lastS = m; 
                       s = sinf(ph + m * index);
                    } break; 
            case 9: { // FOLD: Wavefolding Distorted Sine
                        if(i==0) {
                            r_ksBuffer[3] = r_randf(); r_ksBuffer[4] = r_randf(); 
                            r_ksBuffer[5] = 1.0f + (r_randf() * 2.0f - 1.0f) * 0.005f;
                            r_ksBuffer[6] = (r_randf() * 2.0f - 1.0f) * 0.02f; r_ksBuffer[7] = r_randf();
                        }
                        
                        // Phase 4: Integer Phasing for main OSC
                        uint32_t phMain_u = (uint32_t)(r_ksBuffer[1] * 683565275.5f); // Scale TWO_PI to 2^32

                        for(uint32_t i=0; i<len; ++i) {
                            if(r_activeId != myId) break; 
                            float prog = (float)i * invLen; 
                            float env = 1.0f - prog; if(env < 0.001f) env = 0.0f;

                            // TURBO-DSP: Update modulators and frequency every 16 samples
                            if((i & 15) == 0 || i == 0) {
                                 r_ksBuffer[7] += 2.2f * invFs * 16.0f; if(r_ksBuffer[7] >= 1.0f) r_ksBuffer[7] -= 1.0f;
                                 float wow = r_fast_sinf(r_ksBuffer[7] * TWO_PI) * 0.003f;
                                 float curFreq = (f * r_ksBuffer[5]) * (1.0f + wow);
                                 r_ksBuffer[10] = curFreq * TWO_PI * invFs; // Store curInc

                                 r_ksBuffer[3] += 1.8f * invFs * 16.0f; if(r_ksBuffer[3] >= 1.0f) r_ksBuffer[3] -= 1.0f;
                                 float lfo2Rate = 0.5f + P[5] * 14.5f; 
                                 r_ksBuffer[4] += lfo2Rate * invFs * 16.0f; if(r_ksBuffer[4] >= 1.0f) r_ksBuffer[4] -= 1.0f;

                                 float slowMod = r_fast_sinf(r_ksBuffer[3] * TWO_PI);
                                 float fastMod = r_fast_sinf(r_ksBuffer[4] * TWO_PI);
                                 r_ksBuffer[11] = (P[2] + r_ksBuffer[6] - 0.5f) * 1.5f + slowMod * 0.2f; // bias
                                 r_ksBuffer[12] = (1.0f + P[1] * 12.0f) * (1.0f + fastMod * 0.15f); // drive
                                 r_ksBuffer[13] = (P[4] * 0.7f) * (1.0f + fastMod * 0.1f);  // feedbackAmt
                            }
                            float curInc = r_ksBuffer[10];
                            float bias = r_ksBuffer[11], drive = r_ksBuffer[12], feedbackAmt = r_ksBuffer[13];
                            float dc = P[3] * 0.3f;

                            float in_osc = r_sin_u32(phMain_u + (uint32_t)(r_ksBuffer[2] * feedbackAmt * 683565275.5f)); 
                            float x = (in_osc + bias + dc) * drive;

                            float fold_val = x * 0.25f; 
                            float wrap = fold_val + 0.75f;
                            float s_fold = 4.0f * fabsf(wrap - (int)wrap - 0.5f) - 1.0f;

                            float lpfFold = 0.02f + (1.0f - P[5] * 0.92f) * 0.48f; 
                            r_ksBuffer[0] += lpfFold * (s_fold - r_ksBuffer[0]);
                            float res = r_ksBuffer[0] * 1.8f;
                            r_ksBuffer[2] = res; 

                            uint32_t step_u = (uint32_t)(curInc * 683565275.5f);
                            phMain_u += step_u; 

                            float outVal = res * env;
                            if(outVal > 1.0f) outVal = 1.0f; if(outVal < -1.0f) outVal = -1.0f;
                            if(!swapped && i == R_JIT_CHUNK) { bufferPtr = r_activateVoice(len); swapped = true; }
                            bufferPtr[i] = (int16_t)(outVal * 2040.0f);
                        }
                        r_ksBuffer[1] = (float)phMain_u * 1.462918e-9f;
                        goto jit_done;
                      } break;
            case 10: { // MULTI: Multi-Algorithm FM (16 Modes)
                        if(i==0) { /* state already cleared */ }
                        int modeIdx = (int)(P[4] * 15.99f), alg = modeIdx % 8; bool dirty = modeIdx >= 8;
                        int ratIdx = (int)(P[1] * 7.99f); float r[5]={0,1,1,1,1};
                        switch(ratIdx) {
                            case 0: r[2]=1.0f; r[3]=1.0f; r[4]=1.0f; break; 
                            case 1: r[2]=2.0f; r[3]=3.0f; r[4]=4.0f; break; 
                            case 2: r[2]=3.0f; r[3]=5.0f; r[4]=7.0f; break; 
                            case 3: r[2]=1.414f; r[3]=1.732f; r[4]=2.236f; break; 
                            case 4: r[2]=0.5f; r[3]=2.0f; r[4]=4.0f; break; 
                            case 5: r[2]=4.02f; r[3]=3.01f; r[4]=1.005f; break; 
                            case 6: r[2]=0.75f; r[3]=1.25f; r[4]=1.5f; break; 
                            case 7: r[2]=8.0f; r[3]=0.25f; r[4]=16.0f; break; 
                        }
                        float mIdx = (0.05f + P[2] * 0.95f) * 4.0f; 
                        r_ksBuffer[1] += invFs * f * r[1]; r_ksBuffer[2] += invFs * f * r[2] * (1.0f + P[5]*0.08f);
                        r_ksBuffer[3] += invFs * f * r[3] * (1.0f - P[5]*0.05f); r_ksBuffer[4] += invFs * f * r[4] * (1.0f + P[5]*0.12f);
                        for(int k=1; k<=4; k++) { if(r_ksBuffer[k] >= 1.0f) r_ksBuffer[k] -= 1.0f; }
                        float ph1 = r_ksBuffer[1]*TWO_PI, ph2 = r_ksBuffer[2]*TWO_PI, ph3 = r_ksBuffer[3]*TWO_PI, ph4 = r_ksBuffer[4]*TWO_PI;
                        auto mSin = [&](float ph, float mod) { float sVal = sinf(ph + mod * mIdx); return dirty ? tanhf(sVal) * 0.8f : sVal; };
                        float o1=0, o2=0, o3=0, o4=0, sI = P[3]*3.0f;
                        switch(alg) {
                            case 0: o4=sinf(ph4); o3=mSin(ph3,o4*sI); o2=mSin(ph2,o3); s=mSin(ph1,o2); break;
                            case 1: o4=sinf(ph4)*sI; o3=sinf(ph3); o2=mSin(ph2,o4+o3); s=mSin(ph1,o2); break;
                            case 2: o4=sinf(ph4)*sI; o3=mSin(ph3,o4); o2=mSin(ph2,o4); s=mSin(ph1,o3+o2); break;
                            case 3: o4=sinf(ph4)*sI; o3=mSin(ph3,o4); o2=sinf(ph2); s=(o3+mSin(ph1,o2))*0.6f; break;
                            case 4: o4=sinf(ph4)*sI; o3=mSin(ph3,o4); o2=mSin(ph2,o4); o1=mSin(ph1,o4); s=(o3+o2+o1)*0.4f; break;
                            case 5: o4=sinf(ph4)*sI; o3=sinf(ph3)*0.5f; o2=sinf(ph2)*0.25f; s=mSin(ph1,o4+o3+o2); break;
                            case 6: o4=sinf(ph4)*sI; o3=mSin(ph3,o4); o2=sinf(ph2); o1=sinf(ph1); s=(o3+o2+o1)*0.4f; break;
                            case 7: o4=sinf(ph4)*sI; o3=sinf(ph3); o2=sinf(ph2); o1=sinf(ph1); s=(o4+o3+o2+o1)*0.3f; break;
                        }
                        s *= 2.0f;
                      } break;
            case 11: { // ZAP: Glitched Percussive Sweep
                       float bend = P[4] * 2.0f * env;
                       float sweep = powf(1.0f - prog, 1.0f + P[1]*10.0f); 
                       float fZap = (P[0]+P[5]) * 5000.0f * sweep + 40.0f + bend*1000.0f;
                       ph += fZap * TWO_PI * invFs; ph2 += fZap * (1.0f + P[2]*4.0f) * TWO_PI * invFs;
                       s = r_fast_sinf(ph + r_fast_sinf(ph2)*P[2]*2.5f); 
                       if(P[3] > 0.5f && (fmodf(ph*0.5f, TWO_PI) < PI)) s *= 0.8f; 
                     } break;
            case 12: { // SNARE: Physical Filtered Snare Drum
                       if(i==0) { /* state already cleared */ }
                       float bodyEnv = env * env; float wireEnv = env; 
                       float punchEnv = powf(env, 12.0f + P[5]*10.0f);
                       float bodyPitch = baseFreq * (1.0f + P[2] * punchEnv * 2.0f + P[4]*env); 
                       float phInc = bodyPitch * TWO_PI * invFs;
                       r_ksBuffer[0] += phInc; r_ksBuffer[1] += phInc * 1.473f;
                       float drum = (sinf(r_ksBuffer[0]) + 0.4f * sinf(r_ksBuffer[1]));
                       float n = (r_randf() * 2.0f - 1.0f);
                       float cutoff = 1200.0f + P[3] * 6000.0f; float res = 0.4f + P[1] * 0.4f;
                       float gf = 2.0f * sinf(PI * cutoff * invFs); float gq = 1.0f / (0.6f + res * 2.0f);
                       float hp = n - (r_ksBuffer[2]*gq) - r_ksBuffer[3];
                       float bp = hp * gf + r_ksBuffer[2]; float lp = bp * gf + r_ksBuffer[3];
                       r_ksBuffer[2] = bp; r_ksBuffer[3] = lp;
                       float wires = bp * 2.5f;
                       s = (drum * (1.0f - P[1]) + wires * P[1]) * 1.5f;
                     } break; 
            case 13: { // BYTE: LFSR 8-bit Noise Machine
                       if(i%(int)(1+P[1]*50)==0){ uint32_t b=((sr>>0)^(sr>>2)^(sr>>3)^(sr>>5))&1; sr=(sr>>1)|(b<<15); } 
                       if(P[2]>0.5f) sr^=0xAA55; 
                       if(i%2==0) sr ^= (uint32_t)(P[4]*65535); // NEW: Bit-chaos
                       uint32_t mask = (uint32_t)(P[3] * 65535 + P[5]*env*32768); 
                       s = (((sr ^ mask)&0xFFFF)/32768.0f)-1.0f; 
                     } break;
                         case 14: { // HIHAT: Metallic Percussion Array (Stable SVF)
                          if(i==0) { /* state already cleared */ }
                          float r_mod = P[4] * 0.5f;
                          float ratios[6] = {1.304f+r_mod, 1.466f, 1.787f-r_mod, 1.932f, 2.536f, 1.0f}; 
                          float metallic = 0.0f;
                          for(int k=0; k<6; k++) { 
                              float fOsc = f * ratios[k]; r_ksBuffer[k] += fOsc * invFs; 
                              if(r_ksBuffer[k] > 1.0f) r_ksBuffer[k] -= 1.0f; 
                              metallic += (r_ksBuffer[k] > 0.5f) ? 0.16f : -0.16f; 
                          }
                          float src = metallic;
                          if(P[3] > 0.01f) src = src * (1.0f - P[3]) + (r_randf()*2.0f-1.0f) * P[3] * 0.5f;

                          // 1st order HPF (DC Block / Bottom Cut) - Stronger 150Hz-ish cut
                          float hpf_z = r_ksBuffer[6]; float hp_out = src - hpf_z;
                          r_ksBuffer[6] = hpf_z + 0.15f * (src - hpf_z);

                          // Stable SVF Bandpass
                          float cutoff = constrain(400.0f + P[1] * 7000.0f + P[5]*env*2500.0f, 200.0f, 11500.0f); 
                          // HARD LIMIT: P2 resonance minimum 0.1 to prevent clicking
                          float q = 1.2f + constrain(P[2], 0.1f, 1.0f) * 12.0f;
                          float gf = 2.0f * sinf(PI * cutoff * invFs); if(gf > 1.3f) gf = 1.3f;
                          float gq = 1.0f / q;
                          
                          float hp = hp_out - (r_ksBuffer[7]*gq) - r_ksBuffer[8];
                          float bp = hp * gf + r_ksBuffer[7]; float lp = bp * gf + r_ksBuffer[8];
                          if(isnan(bp) || isinf(bp)) { bp = 0; r_ksBuffer[7]=0; r_ksBuffer[8]=0; }
                          r_ksBuffer[7] = bp; r_ksBuffer[8] = lp;

                          s = bp * 2.2f;
                        } break;
                         case 15: { // SHAKER: Biquad Shaker Simulation
                       if(i==0) { r_ksBuffer[8] = 0.0f; }
                       int type = (int)(P[0] * 3.99f); 
                       float freqMult = 0.5f + P[1] * 1.5f + P[4]*env; 
                       float count = 4.0f + P[2] * 200.0f + P[5]*100.0f; 
                       float shakeDecay = 0.999f + P[3] * 0.00095f; 
                       float baseF = 3200.0f; float res = 0.96f; float gain = 1.0f;
                       if(type == 1) { baseF = 3000.0f; res = 0.70f; gain = 2.0f; } 
                       else if(type == 2) { baseF = 5500.0f; res = 0.60f; gain = 1.5f; } 
                       else if(type == 3) { baseF = 5600.0f; res = 0.98f; gain = 0.8f; }
                       baseF *= freqMult; if(baseF > 9500.0f) baseF = 9500.0f; 
                       float a2 = res * res; float a1 = -2.0f * res * cosf(TWO_PI * baseF * invFs);
                       float b0 = 0.5f - 0.5f * a2; float b2 = -b0; float energy = powf(shakeDecay, (float)i);
                       float impulse = 0.0f; if(r_randf() < energy * count * 0.001f) { impulse = (r_randf() * 2.0f - 1.0f) * gain; }
                       float resOut = b0*impulse + 0.0f*r_ksBuffer[0] + b2*r_ksBuffer[1] - a1*r_ksBuffer[2] - a2*r_ksBuffer[3];
                       if(isnan(resOut) || isinf(resOut)) resOut = 0;
                       r_ksBuffer[1] = r_ksBuffer[0]; r_ksBuffer[0] = impulse; r_ksBuffer[3] = r_ksBuffer[2]; r_ksBuffer[2] = resOut; s = resOut * 4.0f;
                     } break;
        }
                       

        // Final Output Gain and Envelope Stage

        float outVal = s * envE;
        if(outVal > 1.0f) outVal = 1.0f;
        if(outVal < -1.0f) outVal = -1.0f;
        bufferPtr[i] = (int16_t)(outVal * 2040.0f);
    }
    jit_done: // LABEL for skip
    if(!swapped) { (void)r_activateVoice(len); }
}

// ==========================================
// DEBUG HELPER FUNCTIONS
// ==========================================
const char* r_getEngineName(uint8_t id) {
    switch(id) {
        case 0: return "PLONK";   case 1: return "PLUCK";   case 2: return "WOODY";
        case 3: return "TUBE";    case 4: return "PWM";     case 5: return "BOUNCE";
        case 6: return "RESO2";   case 7: return "VOX";     case 8: return "2OPFM";
        case 9: return "FOLD";    case 10:return "MULTI";   case 11:return "ZAP"; 
        case 12:return "SNARE";   case 13:return "BYTE";    case 14:return "HIHAT";
        case 15:return "SHAKR";   default: return "UNK";
    }
}
const char* r_getParamLabel(uint8_t eng, uint8_t idx) {
    static const char* labels[16][6] = {
        {"Frq", "Ton", "Str", "Exc", "Dcy", "Inh"}, // 0 (PLONK)
        {"Frq", "Stf", "Pck", "Dmp", "Vib", "Bdy"}, // 1 (PLUCK)
        {"Frq", "Tmb", "Exc", "Pos", "Det", "Sus"}, // 2 (WOODY)
        {"Frq", "Tmb", "Dmp", "Geo", "Pre", "Dir"}, // 3 (TUBE)
        {"Frq", "Mod", "Sub", "PW",  "Swp", "Det"}, // 4 (PWM)
        {"Frq", "Spd", "Grv", "Swp", "Jit", "Dmp"}, // 5 (BOUNCE)
        {"Frq", "Rez", "NFr", "Nmx", "Spr", "Dry"}, // 6 (RESO2)
        {"Frq", "Vow", "Sht", "LFO", "Xmd", "Wet"}, // 7 (VOX)
        {"Frq", "Rat", "Ind", "Fdb", "Atk", "Shp"}, // 8 (2OPFM)
        {"Frq", "Drv", "Sym", "Off", "Fdb", "Smy"}, // 9 (FOLD)
        {"Frq", "Rat", "Ind", "Mod", "Alg", "Det"}, // 10 (MULTI)
        {"Frq", "Swp", "FMI", "Gat", "Bnd", "Ofs"}, // 11 (ZAP)
        {"Frq", "Snp", "Imp", "Clr", "Rat", "Env"}, // 12 (SNARE)
        {"Frq", "Rat", "Mod", "Msk", "Eq",  "Lop"}, // 13 (BYTE)
        {"Frq", "Ton", "Rez", "Nse", "Mmx", "Clk"}, // 14 (HIHAT)
        {"Typ", "Frq", "Cnt", "Dcy", "Grt", "Spd"}  // 15 (SHAKR)
    };
    if(eng < 16 && idx < 6) return labels[eng][idx];
    return "---";
}

void printDebugInfo(ChaosPatch p, float p1, float p2, float p3) {
    if(!ENABLE_SERIAL_DEBUG) return;

    // REPLICATE MAPPING LOGIC TO SHOW "FINAL" VALUES
    float P[6]; for(int i=0; i<6; i++) P[i] = p.fixedP[i];
    bool pMod[6] = {0};
    
    // Pot 2 (MIDDLE) -> maps[2,3], Pot 3 (BOTTOM) -> maps[0,1]
    for(int k=0; k<2; k++) {
        if(p.maps[k].target < 6) { P[p.maps[k].target] = r_mapVal(p3, p.maps[k]); pMod[p.maps[k].target] = true; }
    }
    for(int k=2; k<4; k++) {
        if(p.maps[k].target < 6) { P[p.maps[k].target] = r_mapVal(p2, p.maps[k]); pMod[p.maps[k].target] = true; }
    }

    uint32_t len = 600 + (uint32_t)(p1 * (R_BUFFER_SZ - 601)); 

    char buf[256];
    // LINE 1: ENGINE & PARAMETERS
    snprintf(buf, sizeof(buf), "[TRIG] Slot:%d | Eng:%s | Full Params: [%.2f, %.2f, %.2f, %.2f, %.2f, %.2f]",
        r_currentSlot, r_getEngineName(p.engine), P[0], P[1], P[2], P[3], P[4], P[5]
    );
    Serial.println(buf);

    // LINE 1b: Labels & Modulation info
    snprintf(buf, sizeof(buf), "       P:[%s:%.2f, %s%.2f%s:%s, %s%.2f%s:%s, %s%.2f%s:%s, %s%.2f%s:%s, %s%.2f%s:%s]",
        r_getParamLabel(p.engine, 0), P[0],
        pMod[1]?"*":"", P[1], pMod[1]?"*":"", r_getParamLabel(p.engine, 1),
        pMod[2]?"*":"", P[2], pMod[2]?"*":"", r_getParamLabel(p.engine, 2),
        pMod[3]?"*":"", P[3], pMod[3]?"*":"", r_getParamLabel(p.engine, 3),
        pMod[4]?"*":"", P[4], pMod[4]?"*":"", r_getParamLabel(p.engine, 4),
        pMod[5]?"*":"", P[5], pMod[5]?"*":"", r_getParamLabel(p.engine, 5)
    );
    Serial.println(buf);

    // LINE 2: ROUTING DETAILS
    char p3Msg[64] = "P3(Bot)->[";
    char p2Msg[64] = "P2(Mid)->[";
    bool firstT = true, firstF = true;
    for(int k=0; k<4; k++) {
        if(p.maps[k].target == 255) continue;
        char step[32];
        const char* tName = r_getParamLabel(p.engine, p.maps[k].target);
        bool inv = p.maps[k].minVal > p.maps[k].maxVal;
        snprintf(step, 32, "%s%s", tName, inv?"!":"");
        if(k < 2) { 
            if(!firstT) strncat(p3Msg, ", ", sizeof(p3Msg) - strlen(p3Msg) - 1);
            strncat(p3Msg, step, sizeof(p3Msg) - strlen(p3Msg) - 1); firstT = false;
        } else {
            if(!firstF) strncat(p2Msg, ", ", sizeof(p2Msg) - strlen(p2Msg) - 1);
            strncat(p2Msg, step, sizeof(p2Msg) - strlen(p2Msg) - 1); firstF = false;
        }
    }
    strncat(p3Msg, "]", sizeof(p3Msg) - strlen(p3Msg) - 1);
    strncat(p2Msg, "]", sizeof(p2Msg) - strlen(p2Msg) - 1);
    
    snprintf(buf, sizeof(buf), "       Layout: P1(Dcy:%.2f) | %s | %s", 
        p1, p2Msg, p3Msg);
    Serial.println(buf);
    Serial.println("-");
}


void r_setup1() {}
void r_loop1() {
    uint32_t renderIdx; // 0 or 1
    if(rp2040.fifo.pop_nb(&renderIdx)) {
        r_isCalculating = true; 
        int16_t* target = r_scratchBuffer;
        float dD = r_sys_btn_held ? r_bSnap[0] : r_jDecay;
        float dF = r_sys_btn_held ? r_bSnap[1] : r_jFX;
        float dT = r_sys_btn_held ? r_bSnap[2] : r_jTimbre;
        generateJIT(target, activePatches[renderIdx], dD, dT, dF, r_activeId);
        r_isCalculating = false;
    }
}

uint8_t getWeightedRandomEngine() {
    float centers[4] = {0.0f, 0.33f, 0.66f, 1.0f};
    float weights[4];
    float totalW = 0;

    // 0.5 = Neutral Bias (Equal probability)
    float inf = fabsf(sys_random_bias - 0.5f) * 4.0f;
    if(inf > 1.0f) inf = 1.0f;

    for(int i=0; i<4; i++) {
        // Linear peak at group center
        float dist = fabsf(sys_random_bias - centers[i]);
        float peakW = 1.0f - dist * 3.0f; 
        if(peakW < 0.1f) peakW = 0.1f; 
        
        // Blend between the biased peakW and a uniform floor (1.0)
        weights[i] = (peakW * inf) + (1.0f * (1.0f - inf));
        totalW += weights[i];
    }
    
    float r = r_randf() * totalW;
    float cumulative = 0;
    for(int i=0; i<4; i++) {
        cumulative += weights[i];
        if(r <= cumulative) {
            return (uint8_t)(i * 4 + (r_xrand() % 4));
        }
    }
    return r_xrand() % 16;
}

void randomizePatch(uint8_t id) {
    ChaosPatch* p = &slots[id];
    uint8_t old = p->engine; 
    do { p->engine = getWeightedRandomEngine(); } while(p->engine == old);
    
    // Improved Pitch Generation: Bell curve distribution
    float r1 = r_randf(); float r2 = r_randf(); float r3 = r_randf();
    p->fixedP[0] = (r1 + r2 + r3) / 3.0f; 

    p->fixedP[1] = r_randf(); p->fixedP[2] = r_randf(); p->fixedP[3] = r_randf();
    p->fixedP[4] = r_randf(); p->fixedP[5] = r_randf();
    
    // 2. Cluster Assignment: Shuffle all 6 params and split them
    uint8_t pool[6] = {0, 1, 2, 3, 4, 5};
    for(int i=0; i<6; i++) {
        int r = i + r_xrand() % (6 - i);
        uint8_t temp = pool[i]; pool[i] = pool[r]; pool[r] = temp;
    }
    // Store assigned cluster (0-2 for Pot 2 Cluster, 3-5 for Pot 3 Cluster)
    for(int i=0; i<6; i++) p->cluster[i] = pool[i];

    auto setMap = [&](Mapping* m, uint8_t target) {
        m->target = target;
        if(target == 0) { // Pitch focalization
            float width = 0.15f + r_randf() * 0.55f; 
            float start = r_randf() * (1.0f - width);
            float end = start + width;
            if(r_xrand()%2) { m->minVal = start; m->maxVal = end; }
            else { m->minVal = end; m->maxVal = start; }
        } else { // Full sweep for others
            if(r_xrand()%2) { m->minVal = 0.0f; m->maxVal = 1.0f; }
            else { m->minVal = 1.0f; m->maxVal = 0.0f; }
        }
    };

    // 3. Mapping Density Roll for each Poti Cluster
    // Pot 2 Cluster (p->cluster[0..2])
    if(r_xrand() % 2 == 0) { // Macro Mode (2 Visible, 1 Shift)
        setMap(&p->maps[0], p->cluster[0]);
        setMap(&p->maps[1], p->cluster[1]);
    } else { // Precision Mode (1 Visible, 2 Shift)
        setMap(&p->maps[0], p->cluster[0]);
        p->maps[1].target = 255; // Disable second map for this pot
    }

    // Pot 3 Cluster (p->cluster[3..5])
    if(r_xrand() % 2 == 0) { // Macro Mode (2 Visible, 1 Shift)
        setMap(&p->maps[2], p->cluster[3]);
        setMap(&p->maps[3], p->cluster[4]);
    } else { // Precision Mode (1 Visible, 2 Shift)
        setMap(&p->maps[2], p->cluster[3]);
        p->maps[3].target = 255; // Disable second map for this pot
    }
    
    p->lfoRate = 0.0f; 
    p->lfoDepth = 0.0f; 
}

// ==========================================
// SETUP
// ==========================================
void r_advanceSequencer() {
    if(currentMode == 1) {
        if(sys_groove_select <= 15) {
            uint8_t rawVal = grooves[sys_groove_select][r_stepCounter % 16];
            r_currentSlot = (rawVal - 1) % sys_slot_limit;
        } else {
            // Drunk Walk (Zone 9/10)
            r_currentSlot = (r_currentSlot + (r_xrand() % 3 - 1) + sys_slot_limit) % sys_slot_limit;
        }
        r_stepCounter++;
    }
}

void r_on_pwm_wrap() {
    pwm_clear_irq(r_sliceIRQ);
    int32_t mix = 0; bool anyActive = false;
    uint32_t maxEnv = 0;
    
    uint32_t save = spin_lock_blocking(r_voiceLock);
    for(int i=0; i<4; i++) {
        if(r_voices[i].active) {
            anyActive = true; mix += r_voiceBuffers[i][r_voices[i].playHead];
            
            uint32_t rem = r_voices[i].stopHead - r_voices[i].playHead;
            uint32_t env = (rem * 1023) / r_voices[i].stopHead;
            if(env > maxEnv) maxEnv = env;
            
            r_voices[i].playHead++; if(r_voices[i].playHead >= r_voices[i].stopHead) r_voices[i].active = false;
        }
    }
    spin_unlock(r_voiceLock, save);
    
    if(mix > 2047) mix = 2047; if(mix < -2047) mix = -2047;
    pwm_set_chan_level(r_sliceAudio, PWM_CHAN_B, R_PWM_MID + mix);
    
    // LED follows the Decay Envelope instead of audio VU (adds ^2 for visual punch)
    r_currentVU = (maxEnv * maxEnv) >> 10; 
    // Legacy LED: Only track envelope in interrupt if NOT in Shift mode
    if (system_hw_is_legacy && !r_sys_btn_held) {
        pwm_set_chan_level(r_sliceLED, PWM_CHAN_B, r_currentVU); 
    }
}

void r_triggerSound() {
    // Atomic state update (Ping-Pong buffers)
    ppIdx = !ppIdx; 
    r_activeId++; // Invalidate ongoing renders to ensure low latency
    activePatches[ppIdx] = slots[r_currentSlot];
    // MICRO-MUTATION
    activePatches[ppIdx].fixedP[0] += (r_randf() * 0.0001f) - 0.00005f; 
    
    // Push the INDEX (0 or 1)
    rp2040.fifo.push_nb(ppIdx); 

    r_activeLED_Engine = activePatches[ppIdx].engine;

    // DEBUG PRINT
    printDebugInfo(activePatches[ppIdx], r_jDecay, r_jFX, r_jTimbre);
}

void r_setup() {
    r_loadHardwareConfig();
    pinMode(R_PIN_BTN, INPUT_PULLUP);
    pinMode(R_PIN_RAND_TRIG, INPUT);
    pinMode(R_PIN_SOUND_TRIG, INPUT);
    
    gpio_set_function(1, GPIO_FUNC_PWM);
    gpio_set_function(2, GPIO_FUNC_PWM);
    
    r_sliceAudio = pwm_gpio_to_slice_num(1);
    r_sliceIRQ = pwm_gpio_to_slice_num(2);
    
    pwm_set_wrap(r_sliceAudio, R_PWM_TOP);
    pwm_set_enabled(r_sliceAudio, true);
    
    pwm_set_wrap(r_sliceIRQ, R_PWM_TOP);
    pwm_set_enabled(r_sliceIRQ, true);

    r_sliceLED = pwm_gpio_to_slice_num(5);
    pwm_set_wrap(r_sliceLED, 1023);
    
    if(!system_hw_is_legacy) {
        r_strip.begin();
        r_strip.setBrightness(255);
        r_strip.show();
    } else {
        gpio_set_function(5, GPIO_FUNC_PWM);
        pwm_set_enabled(r_sliceLED, true);
    }
    
    // serviceMenu(); // Redundant and disabled, as hardware LED config is already handled in runMasterBootMenu()
    
    watchdog_enable(8000, 1);
    
    for(int i=0; i<4; i++) {
        r_voiceBuffers[i] = r_rawBuffers[i];
    }
    r_scratchBuffer = r_rawBuffers[4];
    
    r_seed = micros() ^ (analogRead(26) << 4) ^ (analogRead(A0) << 10);
    srand(r_seed);
    for(int n=0; n<2049; n++) r_SINE_LUT[n] = sinf((float)n * TWO_PI / 2048.0f);
    
    r_voiceLock = spin_lock_init(spin_lock_claim_unused(true));
    
    for(int i=0; i<8; i++) randomizePatch(i);

    pwm_set_irq_enabled(r_sliceIRQ, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, r_on_pwm_wrap);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    
    r_runStartupAnimation();
}

// ==========================================
// SERIAL COMMAND PARSER
// ==========================================
void parseSerialCommand() {
    if(!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if(cmd.length() == 0) return;

    int sp1 = cmd.indexOf(' ');
    String op = (sp1 == -1) ? cmd : cmd.substring(0, sp1);
    op.toUpperCase();

    ChaosPatch* p = &slots[r_currentSlot]; // Edit Current Slot

    if(op == "TRIG") {
        r_triggerSound();
        Serial.println("[CMD] Triggered!");
    }
    else if(op == "ENG") {
        int val = cmd.substring(sp1+1).toInt();
        p->engine = constrain(val, 0, 15);
        Serial.print("[CMD] Engine set to: "); Serial.print(val); Serial.print(" ("); Serial.print(r_getEngineName(p->engine)); Serial.println(")");
    }
    else if(op == "MOD") { // Slot-based manual override
        int val = cmd.substring(sp1+1).toInt();
        r_currentSlot = constrain(val, 0, 7);
        Serial.print("[CMD] Editing Slot: "); Serial.println(r_currentSlot);
    }
    else if(op == "P") { // P <idx> <val>
        int sp2 = cmd.indexOf(' ', sp1+1);
        if(sp2 != -1) {
            int idx = cmd.substring(sp1+1, sp2).toInt();
            float val = cmd.substring(sp2+1).toFloat();
            if(idx >= 0 && idx < 6) {
                p->fixedP[idx] = constrain(val, 0.0f, 1.0f);
                Serial.print("[CMD] P"); Serial.print(idx); Serial.print(" set to "); Serial.print(val);
                Serial.print(" ("); Serial.print(r_getParamLabel(p->engine, idx)); Serial.println(")");
            }
        }
    }
    else if(op == "MAP") { // MAP <src> <tgt>
        int sp2 = cmd.indexOf(' ', sp1+1);
        if(sp2 != -1) {
            String src = cmd.substring(sp1+1, sp2); src.toUpperCase();
            String tgt = cmd.substring(sp2+1); tgt.toUpperCase();
            int mIdx = -1;
            if(src == "T" || src == "T1") mIdx = 0;
            else if(src == "T2") mIdx = 1;
            else if(src == "F" || src == "F1") mIdx = 2;
            else if(src == "F2") mIdx = 3;
            if(mIdx != -1) {
                Mapping* m = &p->maps[mIdx];
                if(tgt == "NONE") m->target = 255;
                else if(tgt == "P0") m->target = 0; else if(tgt == "P1") m->target = 1;
                else if(tgt == "P2") m->target = 2; else if(tgt == "P3") m->target = 3;
                else if(tgt == "M") m->target = 4; else if(tgt == "F1") m->target = 5;
                else if(tgt == "F2") m->target = 6;
                Serial.print("[CMD] Mapped "); Serial.print(src); Serial.print(" to "); Serial.println(tgt);
            }
        }
    }
    else if(op == "RNG") { // RNG <src> <min> <max>
        int sp2 = cmd.indexOf(' ', sp1+1);
        int sp3 = cmd.indexOf(' ', sp2+1);
        if(sp2 != -1 && sp3 != -1) {
            String src = cmd.substring(sp1+1, sp2); src.toUpperCase();
            float minVal = cmd.substring(sp2+1, sp3).toFloat();
            float maxVal = cmd.substring(sp3+1).toFloat();
            int mIdx = -1;
            if(src == "T" || src == "T1") mIdx = 0;
            else if(src == "T2") mIdx = 1;
            else if(src == "F" || src == "F1") mIdx = 2;
            else if(src == "F2") mIdx = 3;
            if(mIdx != -1) {
                Mapping* m = &p->maps[mIdx];
                m->minVal = constrain(minVal, 0.0f, 1.0f);
                m->maxVal = constrain(maxVal, 0.0f, 1.0f);
                Serial.print("[CMD] Range "); Serial.print(src); Serial.print(" set to "); Serial.print(m->minVal); Serial.print("-"); Serial.println(m->maxVal);
            }
        }
    }
    else {
        Serial.println("[ERR] Unknown: TRIG, ENG <id>, FX <id>, P <idx> <val>, FP <idx> <val>, MAP <src> <tgt>, RNG <src> <min> <max>");
    }
}

void r_loop() {
    // 1. Check Serial Commands
    parseSerialCommand();

    watchdog_update();
    uint32_t now = millis();
    
    if(now - r_lastADC > 10) { 
        r_lastADC = now; 
        // RAW POT READINGS (Based on User Feedback)
        r_jDecay = analogRead(A0)/1023.0f; // TOP
        if(r_jDecay < 0.01f) r_jDecay = 0.01f; 
        r_jFX = analogRead(A1)/1023.0f;     // MIDDLE
        r_jTimbre = 1.0f - (analogRead(A2)/1023.0f); // BOTTOM (Inverted/CV)
    }
    bool t1 = digitalRead(R_PIN_SOUND_TRIG); bool t2 = digitalRead(R_PIN_RAND_TRIG); bool btn = (digitalRead(R_PIN_BTN) == LOW);
    if(btn && !r_lastBtn) { // Button Down (Press)
        r_bTime = now;
        r_isShiftAction = false;
        r_bSnap[0] = r_jDecay; r_bSnap[1] = r_jFX; r_bSnap[2] = r_jTimbre;
        r_lastStablePotiVal[0] = r_jDecay; r_lastStablePotiVal[1] = r_jFX; r_lastStablePotiVal[2] = r_jTimbre;
        r_lastPotiMoveTime = now;
        r_sys_btn_held = true; // Set flag AFTER capture
    }
    else if(!btn) {
        r_sys_btn_held = false; // Reset flag when button is released
    }
    
    if(btn) { // Button Hold (Config/Shift Check)
        float diff[3] = { fabsf(r_jDecay - r_bSnap[0]), fabsf(r_jFX - r_bSnap[1]), fabsf(r_jTimbre - r_bSnap[2]) };
        bool anyShift = false;
        
        // Robust UI tracking: Handle movement thresholds to ignore jitter/crosstalk
        float sD1 = fabsf(r_jDecay - r_lastStablePotiVal[0]);
        float sD2 = fabsf(r_jFX - r_lastStablePotiVal[1]);
        float sD3 = fabsf(r_jTimbre - r_lastStablePotiVal[2]);
        float maxD = sD1; if (sD2 > maxD) maxD = sD2; if (sD3 > maxD) maxD = sD3;
        
        if (maxD > 0.03f) { 
            if (maxD == sD1) r_lastShiftPoti = 0;
            else if (maxD == sD2) r_lastShiftPoti = 1;
            else r_lastShiftPoti = 2;
            r_lastStablePotiVal[0] = r_jDecay; r_lastStablePotiVal[1] = r_jFX; r_lastStablePotiVal[2] = r_jTimbre; 
            r_lastPotiMoveTime = now;
        }
        
        if (currentMode == 1) { // LOOP MODE (Sequencer Config)
            // Poti 1: Slot Limit (2-8)
            if(diff[0] > 0.01f) {
                int old = sys_slot_limit;
                sys_slot_limit = floorf(2.0f + r_jDecay * 6.99f);
                if(sys_slot_limit > 8) sys_slot_limit = 8;
                if(old != sys_slot_limit) {
                    if (ENABLE_SERIAL_DEBUG) {
                        Serial.print("[SYS] Config Change: Slot Limit = "); Serial.println(sys_slot_limit);
                    }
                    r_shiftBlinkTimer = now;
                    r_isShiftBlinkActive = true;
                }
                anyShift = true;
            }
            // Poti 2: Groove Select (MIDDLE)
            if(diff[1] > 0.02f) {
                int old = sys_groove_select;
                sys_groove_select = (int)(r_jFX * 17.99f);
                if(old != sys_groove_select) {
                    if (ENABLE_SERIAL_DEBUG) {
                        const char* gNames[] = {"LINEAR", "N2 (7:8)", "N1 (5:6)", "BT-FIX", "GHOST", "POLY", "N3 (1:5)", "TERC", "BREAK", "SWING", "ODD", "ACCENT", "B2", "S2", "O2", "A2", "DRUNK", "DRUNK"};
                        Serial.print("[SYS] Config Change: Groove = "); Serial.println(gNames[sys_groove_select]); 
                    }
                    r_shiftBlinkTimer = now;
                    r_isShiftBlinkActive = true;
                }
                anyShift = true;
            }
            // Poti 3: Random Bias (BOTTOM)
            if(diff[2] > 0.02f) {
                float old = sys_random_bias;
                sys_random_bias = r_jTimbre;
                if(ENABLE_SERIAL_DEBUG && fabsf(old - sys_random_bias) > 0.01f) {
                    Serial.print("[SYS] Config Change: Random Bias = "); Serial.println(sys_random_bias);
                }
                anyShift = true;
            }
        } else { // ONE-SHOT MODE (Sound Design)
            // Poti 1: Engine Select (0-15)
            if(diff[0] > 0.01f) {
                int oldE = slots[r_currentSlot].engine;
                slots[r_currentSlot].engine = (int)(r_jDecay * 15.99f);
                if(oldE != slots[r_currentSlot].engine) {
                    if (ENABLE_SERIAL_DEBUG) {
                        Serial.print("[SYS] Config Change: Engine = "); Serial.println(r_getEngineName(slots[r_currentSlot].engine));
                    }
                    r_shiftBlinkTimer = now;
                    r_isShiftBlinkActive = true;
                }
                anyShift = true;
            }
            // Poti 2: Hidden Cluster Layer (MIDDLE)
            if(diff[1] > 0.01f) {
                for(int i=0; i<3; i++) {
                    uint8_t target = slots[r_currentSlot].cluster[i];
                    if(slots[r_currentSlot].maps[0].target != target && slots[r_currentSlot].maps[1].target != target) {
                         if(target < 6) slots[r_currentSlot].fixedP[target] = r_jFX;
                    }
                }
                anyShift = true;
            }
            // Poti 3: Hidden Cluster Layer (BOTTOM)
            if(diff[2] > 0.01f) {
                for(int i=3; i<6; i++) {
                    uint8_t target = slots[r_currentSlot].cluster[i];
                    if(slots[r_currentSlot].maps[2].target != target && slots[r_currentSlot].maps[3].target != target) {
                         if(target < 6) slots[r_currentSlot].fixedP[target] = r_jTimbre;
                    }
                }
                anyShift = true;
            }
        }

        if(anyShift) {
            if(diff[0] > 0.05f || diff[1] > 0.05f || diff[2] > 0.05f) r_isShiftAction = true;
            // Visual feedback handled in the 60fps render loop
        }
    }

    if(!btn && r_lastBtn) { // Button Up (Release)
        uint32_t dur = now - r_bTime;
        if(!r_isShiftAction) {
            if(dur > 1000) { // Long Press: Mode Toggle
                currentMode = !currentMode;
                if (system_hw_is_legacy) {
                    if (currentMode == 1) { // LOOP
                        for(int i=0; i<6; i++) { r_setCustomLED(0, (i%2)?255:0); delay(45); }
                    } else { // ONE-SHOT
                        for(int i=0; i<255; i+=15) { r_setCustomLED(0, i); delay(10); }
                        for(int i=255; i>=0; i-=10) { r_setCustomLED(0, i); delay(10); }
                    }
                } else {
                    for(int i=0; i<8; i++) { r_setSystemLED((i%2)?255:0); delay(40); }
                }
                if(ENABLE_SERIAL_DEBUG) { Serial.print("[SYS] Mode: "); Serial.println(currentMode ? "LOOP" : "ONE-SHOT"); }
            }
            else if(dur > 20) { // Short Press: Trigger/Random
                int t = r_currentSlot; 
                if (currentMode == 1) { // LOOP MODE: Step-and-Fill
                    r_advanceSequencer(); // Advance to next slot in sequence (QUIET)
                    randomizePatch(r_currentSlot); // Randomize the NEW slot
                    if(now - r_lastExternalTrigTime > 2000) {
                        r_triggerSound(); // Audition the new sound ONLY if no active clock
                        if(ENABLE_SERIAL_DEBUG) Serial.println("[CMD] Button: Step + Random + Audition (Loop Building)");
                    }
                } else { // ONE-SHOT MODE: Normal Chaos
                    randomizePatch(t); 
                    // Audition if more than 2 seconds since last external trigger
                    if(now - r_lastExternalTrigTime > 2000) {
                        r_triggerSound(); // Trigger current sound
                        if(ENABLE_SERIAL_DEBUG) Serial.println("[CMD] Button: Random + Manual Trigger (Audition Mode)");
                    } else {
                        if(ENABLE_SERIAL_DEBUG) Serial.println("[CMD] Button: Random Only (Sync Mode)");
                    }
                }
                r_setSystemLED(255); delay(20); 
            }
        }
        r_bTime = 0;
        r_setSystemLED(0); // Ensure LED is off after release or flicker
    }
    r_lastBtn = btn;

    if(t1 && !r_lT1 && (now - r_debounceTrig1 > 25)) { 
        r_debounceTrig1 = now; 
        r_lastExternalTrigTime = now; 
        r_advanceSequencer(); 
        r_triggerSound();
    } r_lT1 = t1;
    if(t2 && !r_lT2) { 
        r_lastExternalTrigTime = now;
        int t = r_currentSlot; randomizePatch(t); r_setSystemLED(255); delay(20); 
    } r_lT2 = t2;

    // Render loop decoupled from IRQ, max ~60fps
    if(now - r_lastNeopixelUpdate > 16) { 
        r_lastNeopixelUpdate = now;
        bool showShift = btn;
        
        if (system_hw_is_legacy) {
            if (showShift) {
                // Brighter Breathing Pulse for Shift Mode (~40-200 brightness)
                int brightness = 120 + (int)(80.0f * sinf(now * 0.008f)); 
                
                // Interaction Flicker: 10Hz Blink when parameter is moving
                if (r_isShiftAction && (now - r_lastPotiMoveTime < 100)) {
                    brightness = (millis() % 100 < 50) ? 255 : 0;
                }
                
                // Engine Click / Param Spike: Overrides flicker for 25ms
                if (r_isShiftBlinkActive) {
                    if (now - r_shiftBlinkTimer < 25) { 
                        brightness = 255;
                    } else {
                        r_isShiftBlinkActive = false;
                    }
                }
                r_setCustomLED(0, brightness);
            } else {
                r_setSystemLED(r_currentVU / 4); // Normal Dynamic Envelope
            }
        } else {
            // NEOPIXEL PREMIUM RENDERING
            if (showShift) {
                uint32_t c = (currentMode == 1) ? 0xFFFFFF : 0xFF00FF; // White for Loop, Magenta for One-Shot
                int brightness = 0;
                uint32_t holdTime = now - r_bTime;
                
                if (r_isShiftAction && (now - r_lastPotiMoveTime < 1000)) {
                    // Gamma-corrected brightness for linear perception
                    if (r_lastShiftPoti == 0) {
                        if (currentMode == 1) { // Loop Mode: Slot Limit
                            c = 0xFF0000;      // Poti 1 = Red
                            brightness = 15 + (int)(r_jDecay * r_jDecay * 240.0f);
                        } else { // One-Shot Mode: Engine Select
                            c = r_enginePalette[slots[r_currentSlot].engine]; 
                            brightness = 255;
                        }
                    } else if (r_lastShiftPoti == 1) { // POTI 2 (MIDDLE / FX)
                        if (currentMode == 1) { // LOOP MODE (Groove)
                             c = 0x00A0FF;      // Cyan/Blue
                             brightness = 15 + (int)(r_jFX * r_jFX * 240.0f);
                        } else { // ONE-SHOT MODE (Unmapped Param)
                            c = r_enginePalette[slots[r_currentSlot].engine]; 
                            brightness = 15 + (int)(r_jFX * r_jFX * 240.0f);
                        }
                    } else if (r_lastShiftPoti == 2) { // POTI 3 (BOTTOM / TIMBRE)
                        if (currentMode == 1) { // LOOP MODE (Random Bias)
                            float dist = fabsf(r_jTimbre - 0.5f);
                            if (dist < 0.05f) {
                                // Center position (True Random) - Sweet Spot
                                c = 0x00FF00; // Green
                                brightness = 40 + (int)(30.0f * sinf(now * 0.05f)); 
                            } else {
                                float intensity = (dist - 0.05f) / 0.45f; 
                                brightness = 20 + (int)(intensity * intensity * 235.0f); 
                                
                                if (r_jTimbre < 0.5f) {
                                    // Left-leaning bias (Warmer engines 0-3)
                                    uint8_t r = 255;
                                    uint8_t g = (uint8_t)((1.0f - intensity) * 200.0f);
                                    c = (r << 16) | (g << 8);
                                } else {
                                    // Right-leaning bias (Cooler engines 12-15)
                                    uint8_t r = (uint8_t)(intensity * 180.0f); 
                                    uint8_t g = (uint8_t)((1.0f - intensity) * 200.0f);
                                    uint8_t b = 255;
                                    c = (r << 16) | (g << 8) | b;
                                }
                            }
                        } else { // ONE-SHOT MODE (Unmapped Param 2)
                             c = r_getPastelColor(r_enginePalette[slots[r_currentSlot].engine]);
                             brightness = 15 + (int)(r_jTimbre * r_jTimbre * 240.0f);
                        }
                    }
                    
                    if (r_isShiftBlinkActive) {
                        if (now - r_shiftBlinkTimer < 25) { // 25ms flash
                            c = 0xFFFFFF; // Flash White
                            brightness = 40; // Let's make it softer
                        } else {
                            r_isShiftBlinkActive = false;
                        }
                    }
                } else {
                    if (holdTime < 1000) {
                        brightness = (int)((holdTime / 1000.0f) * 180.0f);
                    } else {
                        brightness = 90 + (int)(90.0f * sinf((holdTime - 1000) * 0.014f + 1.57f)); 
                    }
                }
                r_setCustomLED(c, brightness);
            } else {
                r_setSystemLED(r_currentVU / 4); 
            }
        }
    }
}

