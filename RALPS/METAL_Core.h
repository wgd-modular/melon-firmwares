#pragma once
#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include <math.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>

/* 
 * METAL_Core Components (Global Scope)
 * Surgical 1:1 Restore from METAL_EDITION.ino (v6.4)
 */

// Engines
#include "METAL_Engine.h"
#include "MULTI_Engine.h"
#include "PLONK_Engine.h"
#include "VOX_Engine.h"
#include "BOUNCE_Engine.h"

// Global Variables for METAL Engine (Prefixed m_)
extern bool system_hw_is_legacy; // Shared with RALPS

#define M_NEOPIXEL_PIN 5
extern Adafruit_NeoPixel systemStrip;
#define m_strip systemStrip

const uint32_t m_pageColors[5][3] = {
    {0xFF0000, 0xFF4500, 0xA52A2A}, // 0: METAL (Red)
    {0x00FFFF, 0x0000FF, 0x8000FF}, // 1: MULTI (Blue)
    {0x00FF00, 0x32CD32, 0x008080}, // 2: PLONK (Green)
    {0xFF007F, 0x8000FF, 0xFF7F50}, // 3: VOX   (Rose)
    {0xFFFF00, 0xEEDD00, 0xCCFF00}  // 4: BOUNC (Yellow)
};

volatile int m_currentVU = 0;
uint32_t m_lastNeopixelUpdate = 0;
uint32_t m_sliceAudio, m_sliceIRQ, m_sliceLED;

const int M_PIN_ACCENT_GATE  = 0; 
const int M_PIN_BTN           = 6;
const int M_PIN_SOUND_TRIG    = 7;

#ifndef F_CPU
#define F_CPU 150000000 
#endif
const uint32_t M_SYSTEM_CLK = F_CPU; 
const uint32_t M_PWM_TOP    = 4095; 
const float M_AUDIO_FS      = (float)M_SYSTEM_CLK / (float)(M_PWM_TOP + 1); 
const uint32_t M_BUFFER_SZ  = 16000; 
const int16_t M_PWM_MID     = 2048; 
const uint32_t M_JIT_CHUNK = 512;

/* --- CORE SYSTEM MEMORY --- */
static int16_t m_rawBuffers[2][M_BUFFER_SZ]; 
static int16_t* volatile m_voiceBuffer; 
static int16_t* volatile m_scratchBuffer;

struct M_Voice {
    bool active;
    uint32_t playHead;
    uint32_t stopHead;
};
static volatile M_Voice m_voice; 
static spin_lock_t* m_voiceLock; 

// --- STATE ---
static volatile float m_pValues[12]; 
static volatile float m_globalDecay = 0.5f;
static volatile float m_accentStrength = 0.5f; 
static volatile uint8_t m_activePage = 0; 
static volatile uint8_t m_currentEngineIdx = 0;
static volatile bool m_isAccentTrigger = false; 

volatile uint32_t m_activeId = 0; // NOT static to ensure cross-core visibility

// Globals
static volatile float m_jDecay = 0.5f, m_jFX = 0.5f, m_jTimbre = 0.5f;
static volatile float m_bSnap[3]; 
static volatile bool m_lastBtn = false, m_lastT1 = false;
static volatile uint32_t m_bTime = 0;
static volatile uint32_t m_lastTrigTime = 0;
static volatile bool m_isShiftAction = false;
static volatile bool m_engineSwitchArmed = false;
static bool m_manualEngineSelect = false;
static uint8_t m_tempEngineIdx = 0;
static bool m_potUnlocked[1]= {false}; 
static bool m_potUnlockedSet[2] = {false, false}; 

static uint32_t m_seed0 = 0xCAFEBABE;
static uint32_t m_seed1 = 0xDEADBEEF;

// Core-Safe Fast PRNG (XOR-Shift)
inline uint32_t m_xrand(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
inline float m_randf_core0() { return (float)(m_xrand(m_seed0) & 0xFFFFFF) / 16777216.0f; }
inline float m_randf_core1() { return (float)(m_xrand(m_seed1) & 0xFFFFFF) / 16777216.0f; }

void m_setCustomLED(uint32_t color, int brightness) {
    if(brightness > 255) brightness = 255;
    if(brightness < 0) brightness = 0;
    if(!system_hw_is_legacy) {
        uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
        uint8_t g = ((color >>  8) & 0xFF) * brightness / 255;
        uint8_t b = ((color      ) & 0xFF) * brightness / 255;
        m_strip.setPixelColor(0, m_strip.Color(r, g, b));
        m_strip.show();
    } else {
        pwm_set_chan_level(m_sliceLED, PWM_CHAN_B, brightness * 16); 
    }
}

void m_enginePulse(uint8_t engineId) {
    uint32_t c = m_pageColors[engineId][0]; 
    for(int i=0; i<3; i++) {
        m_setCustomLED(c, 255); delay(40);
        m_setCustomLED(0, 0); delay(40);
    }
}

void m_serviceMenu() {
    if(digitalRead(M_PIN_BTN) == HIGH) return; 
    uint32_t startTime = millis();
    bool last_mode = system_hw_is_legacy;
    while(digitalRead(M_PIN_BTN) == LOW || (millis() - startTime < 2000)) {
        watchdog_update();
        float p1 = analogRead(A0) / 1023.0f;
        bool selectingLegacy = (p1 > 0.5f);
        if(selectingLegacy != last_mode) {
             if(selectingLegacy) { gpio_set_function(5, GPIO_FUNC_PWM); pwm_set_enabled(m_sliceLED, true); }
             else { m_strip.begin(); }
             last_mode = selectingLegacy;
        }
        if(!selectingLegacy) {
            system_hw_is_legacy = false;
            float pulse = 0.5f + 0.5f * sinf(millis() * 0.005f);
            m_setCustomLED(0x0000FF, (int)(pulse * 255));
        } else {
            system_hw_is_legacy = true;
            bool blink = (millis() % 200) < 100;
            m_setCustomLED(0, blink ? 255 : 0);
        }
        if(digitalRead(M_PIN_BTN) == HIGH && millis() - startTime > 500) break;
        delay(10);
    }
    EEPROM.write(0, system_hw_is_legacy ? 1 : 0); EEPROM.commit();
}

void m_triggerSound(bool accent) {
    m_isAccentTrigger = accent;
    m_activeId++;
    rp2040.fifo.push_nb(m_activeId); 
}

void m_activateVoice(uint32_t len) {
    uint32_t save = spin_lock_blocking(m_voiceLock);
    int16_t* oldBuf = m_voiceBuffer;
    m_voiceBuffer = m_scratchBuffer; m_scratchBuffer = oldBuf;
    m_voice.playHead = 0; m_voice.stopHead = len;
    m_voice.active = true;
    spin_unlock(m_voiceLock, save);
}

void m_generateJIT(uint32_t myId) {
    float P[12]; for(int i=0; i<12; i++) P[i] = m_pValues[i];
    float effectiveDecay = m_globalDecay;
    if (m_isAccentTrigger) { 
        P[0] += 0.70f * m_accentStrength; if(P[0] > 1.0f) P[0] = 1.0f; 
        P[7] += 0.40f * m_accentStrength; if(P[7] > 1.0f) P[7] = 1.0f;
        P[6] += 0.30f * m_accentStrength; if(P[6] > 1.0f) P[6] = 1.0f;
        P[5] += 0.20f * m_accentStrength; if(P[5] > 1.0f) P[5] = 1.0f;
        effectiveDecay += 0.35f * m_accentStrength; if(effectiveDecay > 1.0f) effectiveDecay = 1.0f;
    }
    memset(m_scratchBuffer, 0, M_BUFFER_SZ * sizeof(int16_t));
    uint32_t len = 800 + (uint32_t)(effectiveDecay * (M_BUFFER_SZ - 801));
    if(len < 400) len = 400; if(len > M_BUFFER_SZ) len = M_BUFFER_SZ;
    float invFs = 1.0f / M_AUDIO_FS, invLen = 1.0f / (float)len;
    bool swapped = false;
    static METALEngine metal; static MULTIEngine multi; static PLONKEngine plonk; static VOXEngine vox; static BOUNCEEngine bounce;
    switch(m_currentEngineIdx) {
        case 0: metal.reset(); break; case 1: multi.reset(); break; case 2: plonk.reset(); break; case 3: vox.reset(); break; case 4: bounce.reset(); break;
    }
    for(uint32_t i=0; i<len; ++i) {
        if(m_activeId != myId) return; 
        if(!swapped && i == M_JIT_CHUNK) { m_activateVoice(len); swapped = true; }
        float env = 1.0f - (float)i * invLen; if(env < 0.001f) env = 0.0f;
        float rVal = (m_randf_core1() * 2.0f - 1.0f);
        float sOut = 0;
        switch(m_currentEngineIdx) {
            case 0: sOut = metal.renderSample(env, P, invFs, rVal); break;
            case 1: sOut = multi.renderSample(env, P, invFs, rVal); break;
            case 2: sOut = plonk.renderSample(env, P, invFs, rVal); break;
            case 3: sOut = vox.renderSample(env, P, invFs, rVal);   break;
            case 4: sOut = bounce.renderSample(env, P, invFs, rVal); break;
        }
        (swapped ? m_voiceBuffer : m_scratchBuffer)[i] = (int16_t)(sOut * 2040.0f);
    }
    if(!swapped) { m_activateVoice(len); }
}

void m_on_pwm_wrap() {
    pwm_clear_irq(m_sliceIRQ);
    int32_t outVal = 0; int vu = 0;
    uint32_t save = spin_lock_blocking(m_voiceLock);
    if(m_voice.active) {
        outVal = m_voiceBuffer[m_voice.playHead];
        uint32_t rem = m_voice.stopHead - m_voice.playHead;
        vu = (rem * 255) / m_voice.stopHead;
        m_voice.playHead++; if(m_voice.playHead >= m_voice.stopHead) m_voice.active = false;
    }
    m_currentVU = vu;
    spin_unlock(m_voiceLock, save);
    int32_t finalOut = outVal + M_PWM_MID;
    if(finalOut > 4095) finalOut = 4095; if(finalOut < 0) finalOut = 0;
    pwm_set_chan_level(m_sliceAudio, PWM_CHAN_B, (uint16_t)finalOut);
    if(system_hw_is_legacy && !m_lastBtn) { pwm_set_chan_level(m_sliceLED, PWM_CHAN_B, m_currentVU * 16); }
}

void m_loop1() {
    while(true) { uint32_t mId = rp2040.fifo.pop(); m_generateJIT(mId); }
}

void m_setup() {
    pinMode(M_PIN_BTN, INPUT_PULLUP); pinMode(M_PIN_ACCENT_GATE, INPUT); pinMode(M_PIN_SOUND_TRIG, INPUT);
    EEPROM.begin(512); 
    system_hw_is_legacy = (EEPROM.read(0) == 1);
    m_currentEngineIdx = EEPROM.read(2); if(m_currentEngineIdx > 4) m_currentEngineIdx = 0;
    gpio_set_function(1, GPIO_FUNC_PWM); gpio_set_function(2, GPIO_FUNC_PWM);
    m_sliceAudio = pwm_gpio_to_slice_num(1); m_sliceIRQ = pwm_gpio_to_slice_num(2);
    pwm_set_wrap(m_sliceAudio, M_PWM_TOP); pwm_set_enabled(m_sliceAudio, true);
    pwm_set_wrap(m_sliceIRQ, M_PWM_TOP); pwm_set_enabled(m_sliceIRQ, true);
    m_sliceLED = pwm_gpio_to_slice_num(5); pwm_set_wrap(m_sliceLED, 4095);
    
    // m_serviceMenu(); // Moved to DUAL_CORE_OS Master
    
    if(!system_hw_is_legacy) { m_strip.begin(); m_strip.setBrightness(255); m_strip.show(); }
    else { gpio_set_function(5, GPIO_FUNC_PWM); pwm_set_enabled(m_sliceLED, true); }
    watchdog_enable(8000, 1);
    m_voiceBuffer = m_rawBuffers[0]; m_scratchBuffer = m_rawBuffers[1];
    m_seed0 = micros() ^ (analogRead(26) << 4);
    m_seed1 = m_seed0 ^ 0x12345678;
    for(int i=0; i<12; i++) m_pValues[i] = m_randf_core0();
    m_voiceLock = spin_lock_init(spin_lock_claim_unused(true));
    pwm_set_irq_enabled(m_sliceIRQ, true); irq_set_exclusive_handler(PWM_IRQ_WRAP, m_on_pwm_wrap); irq_set_enabled(PWM_IRQ_WRAP, true);
    m_enginePulse(m_currentEngineIdx); 
}

void m_loop() {
    watchdog_update(); uint32_t now = millis();
    if(now % 10 == 0) { m_jDecay = analogRead(A0)/1023.0f; m_jFX = analogRead(A1)/1023.0f; m_jTimbre = 1.0f - (analogRead(A2)/1023.0f); }
    bool btn = (digitalRead(M_PIN_BTN) == LOW);
    if(btn && !m_lastBtn) { 
        m_bTime = now; m_isShiftAction = false; m_engineSwitchArmed = false; m_manualEngineSelect = false;
        m_bSnap[0]=m_jDecay; m_bSnap[1]=m_jFX; m_bSnap[2]=m_jTimbre; 
        m_potUnlockedSet[0]=false; m_potUnlockedSet[1]=false; m_potUnlocked[0]=false;
    }
    if(btn) {
        if(fabsf(m_jFX - m_bSnap[1]) > 0.035f)    { m_potUnlockedSet[0] = true; m_isShiftAction = true; }
        if(fabsf(m_jTimbre - m_bSnap[2]) > 0.035f) { m_potUnlockedSet[1] = true; m_isShiftAction = true; }
        if(!m_engineSwitchArmed && (now - m_bTime > 5000)) { m_engineSwitchArmed = true; }
        if(m_engineSwitchArmed) {
            float lastTemp = m_tempEngineIdx;
            if(fabsf(m_jDecay - m_bSnap[0]) > 0.10f) { m_manualEngineSelect = true; }
            if(m_manualEngineSelect) { 
                m_tempEngineIdx = (int)(m_jDecay * 4.99f); 
                if(m_tempEngineIdx != lastTemp && system_hw_is_legacy) { m_setCustomLED(0, 0); delay(10); m_setCustomLED(0, 255); delay(10); }
            }
        }
    }
    if(!btn && m_lastBtn) { 
        if(m_engineSwitchArmed) {
            if(m_manualEngineSelect) { m_currentEngineIdx = m_tempEngineIdx; } else { m_currentEngineIdx = (m_currentEngineIdx + 1) % 5; }
            EEPROM.write(2, m_currentEngineIdx); EEPROM.commit();
            for(int i=0; i<12; i++) m_pValues[i] = m_randf_core0();
            m_enginePulse(m_currentEngineIdx);
            m_engineSwitchArmed = false; m_manualEngineSelect = false; m_isShiftAction = true; 
        } else if(!m_isShiftAction) {
            uint32_t dur = now - m_bTime;
            if(dur < 800) { m_activePage = (m_activePage + 1) % 3; for(int i=0; i<3; i++) { m_setCustomLED(0xFFFFFF, 255); delay(20); m_setCustomLED(0,0); delay(20); } }
        }
    } 
    m_lastBtn = btn;
    if (btn) { if(fabsf(m_jDecay - m_bSnap[0]) > 0.035f) m_potUnlocked[0] = true; if(m_potUnlocked[0]) m_accentStrength = m_jDecay; } else { m_globalDecay = m_jDecay; }
    if (m_activePage == 0) { 
        if(btn) { if(m_potUnlockedSet[0]) m_pValues[7] = m_jFX; if(m_potUnlockedSet[1]) m_pValues[2] = m_jTimbre; } 
        else    { m_pValues[0] = m_jFX * 0.30f; m_pValues[1] = m_jTimbre; }
    } else if (m_activePage == 1) { 
        if(btn) { if(m_potUnlockedSet[0]) m_pValues[4] = m_jFX; if(m_potUnlockedSet[1]) m_pValues[6] = m_jTimbre; } 
        else    { m_pValues[3] = m_jFX; m_pValues[5] = m_jTimbre; } 
    } else { 
        if(btn) { if(m_potUnlockedSet[0]) m_pValues[9] = m_jFX; if(m_potUnlockedSet[1]) m_pValues[11] = m_jTimbre; } 
        else    { m_pValues[8] = m_jFX; m_pValues[10] = m_jTimbre; } 
    }
    bool t1 = (digitalRead(M_PIN_SOUND_TRIG) == HIGH); 
    bool t2 = (digitalRead(M_PIN_ACCENT_GATE) == HIGH); 
    if(t1 && !m_lastT1 && (now - m_lastTrigTime > 50)) {
        m_triggerSound(t2); 
        m_lastTrigTime = now;
    }
    m_lastT1 = t1; 
    if(now - m_lastNeopixelUpdate > 16) {
        m_lastNeopixelUpdate = now;
        uint32_t activeColor = m_pageColors[m_currentEngineIdx][m_activePage];
        int brightness = 40; 
        if (m_engineSwitchArmed) {
            if(m_manualEngineSelect) { activeColor = m_pageColors[m_tempEngineIdx][0]; brightness = 255; }
            else { brightness = (now % 100 < 50) ? 255 : 40; activeColor = m_pageColors[(m_currentEngineIdx + 1) % 5][0]; }
        } else if (btn && !m_isShiftAction) {
            float swell = (float)(now - m_bTime) / 5000.0f; if(swell > 1.0f) swell = 1.0f;
            brightness = 40 + (int)(swell * 215.0f);
        } else if (m_currentVU > 0) { brightness = 60 + (m_currentVU * 195 / 255); }
        m_setCustomLED(activeColor, brightness);
    }
}
