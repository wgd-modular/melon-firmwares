/*
HAGIWO MOD2 Kick Ver1.2 - WITH PICKUP FEATURE
Sin wave base , 6 parameters kick drum.
Pressing the button will change the assigned parameter.
Pickup feature prevents value jumping when switching modes.

--Pin assign---
POT1  A0  Pitch | Start freq
POT2  A1  Soft clip rate | End freq
POT3  A2  Amp envelope | Pitch envelope
IN1   D7  Clock in
IN2   D0  Accent (Volume decreases when HIGH)
CV    A2  Shared with POT3
OUT   D1  Audio output
BUTTON    Change assign parameters
LED   D5  WS2812B — amber = Mode 0, cyan = Mode 1, white flash on trigger (80 ms)
EEPROM    Record parameters when a button is pressed

CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial purposes, all without asking permission.

[History]
v1.2  - Add: Pickup feature for smooth parameter transitions
v1.1  - Fix: EEPROM-related malfunction
v1.0  - Init: Initial release
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <EEPROM.h>

// ── WS2812B ───────────────────────────────────────────────────────────────────
#include <Adafruit_NeoPixel.h>
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

bool     ledTrigOn    = false;
uint32_t ledTrigOffAt = 0;
// ──────────────────────────────────────────────────────────────────────────────

/* --------------------------------------------------
   System configuration
-------------------------------------------------- */
const float sys_clock = 150000000.0;
const float T = 0.3;
const float baseIncrement = (2048.0 * 4096.0) / (T * sys_clock);
const float dt = T / 2048.0;
const float FULL_SCALE = 1023.0;
const float MID_LEVEL = FULL_SCALE / 2.0;

const float PICKUP_THRESHOLD = 0.02f;
const int POT_SMOOTH_SAMPLES = 4;

bool reduce_state = 0;

uint16_t kickTable[2048];
uint16_t finalTable[2048];

uint slice_num1;
uint slice_num2;

volatile bool  kickPlaying     = false;
volatile float kickPhase       = 0.0;
volatile float pitchMultiplier = 1.0;
volatile float softClipRate    = 1.0;

#define NUM_CURVES 32
const int LUT_SIZE = 256;
float pitchEnvLUTs[NUM_CURVES][LUT_SIZE];
volatile uint8_t selectedCurve = 0;

float f0 = 250.0;
float f1 = 50.0;
float decayRate = 1.0 + 9.0 * (300.0 / 1023.0);

#define SEGMENTS 8
float ratioLUT[SEGMENTS + 1];

struct ParameterData {
  float value;
  float targetValue;
  bool  pickupActive;
  float lastPotValue;
};

struct {
  ParameterData pitchMult;
  ParameterData softClip;
  ParameterData decay;
  ParameterData startFreq;
  ParameterData endFreq;
  ParameterData curve;
} paramData;

float pot1Buffer[POT_SMOOTH_SAMPLES] = {0};
float pot2Buffer[POT_SMOOTH_SAMPLES] = {0};
float pot3Buffer[POT_SMOOTH_SAMPLES] = {0};
int   potBufferIndex = 0;

/* --------------------------------------------------
   PWM wrap ISR — untouched
-------------------------------------------------- */
void on_pwm_wrap() {
  pwm_clear_irq(slice_num2);

  if (!kickPlaying) {
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
    return;
  }

  float    currInc       = baseIncrement * pitchMultiplier;
  float    index         = kickPhase;
  uint16_t idx           = (uint16_t)index;
  float    frac          = index - idx;
  uint16_t s1            = finalTable[idx];
  uint16_t s2            = finalTable[(idx + 1) % 2048];
  float    interp_sample = s1 * (1.0f - frac) + s2 * frac;
  pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)interp_sample);

  kickPhase += currInc;
  if (kickPhase >= 2048.0f) {
    kickPlaying = false;
    kickPhase   = 0.0f;
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
  }
}

/* --------------------------------------------------
   Wavetable generation — untouched
-------------------------------------------------- */
void make_wavetable() {
  float reduce_level = 1 - (reduce_state * 0.5f);

  float ratio = f1 / f0;
  for (int i = 0; i <= SEGMENTS; i++) {
    float t     = float(i) / SEGMENTS;
    ratioLUT[i] = powf(ratio, t);
  }

  float phase = 0.0f;
  for (int i = 0; i < 2048; i++) {
    float x      = float(i) / 2047.0f;
    int   lutIdx = int(x * (LUT_SIZE - 1));
    float x_adj  = pitchEnvLUTs[selectedCurve][lutIdx];

    float segF    = x_adj * SEGMENTS;
    int   seg     = int(segF);
    if (seg >= SEGMENTS) seg = SEGMENTS - 1;
    float segFrac   = segF - seg;
    float ratio_pow = ratioLUT[seg] * (1.0f - segFrac) + ratioLUT[seg + 1] * segFrac;

    float f = f0 * ratio_pow;
    if (i > 0) phase += 2.0f * PI * f * dt;

    float sample = sinf(phase) * reduce_level;
    kickTable[i] = uint16_t((sample + 1.0f) * (FULL_SCALE / 2.0f));
  }
}

/* --------------------------------------------------
   Pot smoothing — untouched
-------------------------------------------------- */
float readPotSmoothed(int pin, float* buffer) {
  buffer[potBufferIndex] = analogRead(pin) / 1023.0f;
  float sum = 0;
  for (int i = 0; i < POT_SMOOTH_SAMPLES; i++) sum += buffer[i];
  return sum / POT_SMOOTH_SAMPLES;
}

/* --------------------------------------------------
   Pickup check — untouched
-------------------------------------------------- */
bool checkPickup(ParameterData* param, float currentPotValue) {
  if (!param->pickupActive) return true;

  float normalizedTarget = param->targetValue;

  bool crossedFromBelow = (param->lastPotValue < normalizedTarget - PICKUP_THRESHOLD) &&
                          (currentPotValue >= normalizedTarget - PICKUP_THRESHOLD);
  bool crossedFromAbove = (param->lastPotValue > normalizedTarget + PICKUP_THRESHOLD) &&
                          (currentPotValue <= normalizedTarget + PICKUP_THRESHOLD);

  if (crossedFromBelow || crossedFromAbove ||
      fabs(currentPotValue - normalizedTarget) < PICKUP_THRESHOLD) {
    param->pickupActive = false;
    return true;
  }

  param->lastPotValue = currentPotValue;
  return false;
}

/* --------------------------------------------------
   Init parameter data — untouched
-------------------------------------------------- */
void initParameterData() {
  paramData.pitchMult.value = 1.0f;   paramData.pitchMult.pickupActive = false; paramData.pitchMult.lastPotValue = 0.5f;
  paramData.softClip.value  = 1.0f;   paramData.softClip.pickupActive  = false; paramData.softClip.lastPotValue  = 0.0f;
  paramData.decay.value     = 5.0f;   paramData.decay.pickupActive     = false; paramData.decay.lastPotValue     = 0.444f;
  paramData.startFreq.value = 250.0f; paramData.startFreq.pickupActive = false; paramData.startFreq.lastPotValue = 0.243f;
  paramData.endFreq.value   = 50.0f;  paramData.endFreq.pickupActive   = false; paramData.endFreq.lastPotValue   = 0.094f;
  paramData.curve.value     = 0;      paramData.curve.pickupActive     = false; paramData.curve.lastPotValue     = 0.0f;
}

/* --------------------------------------------------
   SETUP
-------------------------------------------------- */
void setup() {
  initParameterData();
  EEPROM.begin(128);

  float temp;
  EEPROM.get(0,  temp); if (!isnan(temp) && temp >= 0.5f  && temp <= 2.0f)     { paramData.pitchMult.value = temp; pitchMultiplier = temp; } else { pitchMultiplier = paramData.pitchMult.value; }
  EEPROM.get(4,  temp); if (!isnan(temp) && temp >= 0.5f  && temp <= 10.0f)    { paramData.softClip.value  = temp; softClipRate    = temp; } else { softClipRate    = paramData.softClip.value; }
  EEPROM.get(8,  temp); if (!isnan(temp) && temp >= 1.0f  && temp <= 10.0f)    { paramData.decay.value     = temp; decayRate       = temp; } else { decayRate       = paramData.decay.value; }
  EEPROM.get(12, temp); if (!isnan(temp) && temp >= 3.0f  && temp <= 1026.0f)  { paramData.startFreq.value = temp; f0              = temp; } else { f0              = paramData.startFreq.value; }
  EEPROM.get(16, temp); if (!isnan(temp) && temp >= 2.0f  && temp <= 513.0f)   { paramData.endFreq.value   = temp; f1              = temp; } else { f1              = paramData.endFreq.value; }
  EEPROM.get(20, temp); if (!isnan(temp) && temp >= 0     && temp < NUM_CURVES) { paramData.curve.value    = temp; selectedCurve   = (uint8_t)temp; } else { selectedCurve = (uint8_t)paramData.curve.value; }

  const float curveMin = 0.1f, curveMax = 2.0f;
  const float step = (curveMax - curveMin) / float(NUM_CURVES - 1);
  for (int c = 0; c < NUM_CURVES; c++) {
    float curveVal = curveMin + step * c;
    for (int i = 0; i < LUT_SIZE; i++) {
      float x = float(i) / float(LUT_SIZE - 1);
      pitchEnvLUTs[c][i] = powf(x, curveVal);
    }
  }

  make_wavetable();
  for (int i = 0; i < 2048; i++) finalTable[i] = kickTable[i];

  pinMode(1, OUTPUT); gpio_set_function(1, GPIO_FUNC_PWM);
  slice_num1 = pwm_gpio_to_slice_num(1);
  pinMode(2, OUTPUT); gpio_set_function(2, GPIO_FUNC_PWM);
  slice_num2 = pwm_gpio_to_slice_num(2);

  pwm_set_clkdiv(slice_num1, 1); pwm_set_wrap(slice_num1, 1023); pwm_set_enabled(slice_num1, true);
  pwm_set_clkdiv(slice_num2, 1); pwm_set_wrap(slice_num2, 4095); pwm_set_enabled(slice_num2, true);
  pwm_clear_irq(slice_num2);
  pwm_set_irq_enabled(slice_num2, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  pinMode(7, INPUT);
  attachInterrupt(digitalPinToInterrupt(7), onTrigger, RISING);
  pinMode(0, INPUT);
  pinMode(6, INPUT_PULLUP);

  // ── WS2812B init (replaces pinMode(5, OUTPUT)) ────────────────────────────
  led.begin();
  led.setBrightness(180);
  led.setPixelColor(0, led.Color(0, 220, 255));  // Mode 1 = cyan (selectMode starts true)
  led.show();
  // ──────────────────────────────────────────────────────────────────────────

  for (int i = 0; i < POT_SMOOTH_SAMPLES; i++) {
    pot1Buffer[i] = analogRead(A0) / 1023.0f;
    pot2Buffer[i] = analogRead(A1) / 1023.0f;
    pot3Buffer[i] = analogRead(A2) / 1023.0f;
  }
  paramData.pitchMult.lastPotValue = pot1Buffer[0];
  paramData.softClip.lastPotValue  = pot2Buffer[0];
  paramData.decay.lastPotValue     = pot3Buffer[0];
  paramData.startFreq.lastPotValue = pot1Buffer[0];
  paramData.endFreq.lastPotValue   = pot2Buffer[0];
  paramData.curve.lastPotValue     = pot3Buffer[0];
}

/* --------------------------------------------------
   LOOP — only change: replace digitalWrite(5,...)
   with led calls, plus trigger-flash restore at end
-------------------------------------------------- */
void loop() {
  static bool selectMode = 1;
  static bool prevBtn    = HIGH;
  static bool firstRun   = true;
  bool currBtn = digitalRead(6);

  if (prevBtn == HIGH && currBtn == LOW) {
    if (selectMode == 0) {
      paramData.pitchMult.value = pitchMultiplier;
      paramData.softClip.value  = softClipRate;
      paramData.decay.value     = decayRate;
    } else {
      paramData.startFreq.value = f0;
      paramData.endFreq.value   = f1;
      paramData.curve.value     = selectedCurve;
    }

    selectMode = !selectMode;

    // ── Replaces: digitalWrite(5, selectMode ? HIGH : LOW) ────────────────
    led.setPixelColor(0, selectMode ? led.Color(0, 220, 255) : led.Color(255, 140, 0));
    led.show();
    // ──────────────────────────────────────────────────────────────────────

    if (selectMode == 0) {
      paramData.pitchMult.targetValue = (paramData.pitchMult.value - 0.5f) / 1.5f;  paramData.pitchMult.pickupActive = true;
      paramData.softClip.targetValue  = (paramData.softClip.value  - 0.5f) / 9.5f;  paramData.softClip.pickupActive  = true;
      paramData.decay.targetValue     = (paramData.decay.value     - 1.0f) / 9.0f;  paramData.decay.pickupActive     = true;
    } else {
      paramData.startFreq.targetValue = (paramData.startFreq.value - 3.0f) / 1023.0f; paramData.startFreq.pickupActive = true;
      paramData.endFreq.targetValue   = (paramData.endFreq.value   - 2.0f) / 510.5f;  paramData.endFreq.pickupActive   = true;
      paramData.curve.targetValue     = paramData.curve.value / float(NUM_CURVES - 1); paramData.curve.pickupActive     = true;
    }

    EEPROM.put(0,  paramData.pitchMult.value);
    EEPROM.put(4,  paramData.softClip.value);
    EEPROM.put(8,  paramData.decay.value);
    EEPROM.put(12, paramData.startFreq.value);
    EEPROM.put(16, paramData.endFreq.value);
    EEPROM.put(20, paramData.curve.value);
    EEPROM.commit();
  }

  prevBtn        = currBtn;
  potBufferIndex = (potBufferIndex + 1) % POT_SMOOTH_SAMPLES;

  if (selectMode == 0) {
    float pot1Val = readPotSmoothed(A0, pot1Buffer);
    if (firstRun || checkPickup(&paramData.pitchMult, pot1Val)) { pitchMultiplier = 0.5f + 1.5f * pot1Val; paramData.pitchMult.value = pitchMultiplier; }
    else { pitchMultiplier = paramData.pitchMult.value; }

    float pot2Val = readPotSmoothed(A1, pot2Buffer);
    if (firstRun || checkPickup(&paramData.softClip, pot2Val))  { softClipRate = 0.5f + 9.5f * pot2Val;    paramData.softClip.value  = softClipRate; }
    else { softClipRate = paramData.softClip.value; }

    float pot3Val = readPotSmoothed(A2, pot3Buffer);
    if (firstRun || checkPickup(&paramData.decay, pot3Val))     { decayRate = 1.0f + 9.0f * pot3Val;       paramData.decay.value     = decayRate; }
    else { decayRate = paramData.decay.value; }

  } else {
    float pot1Val = readPotSmoothed(A0, pot1Buffer);
    if (firstRun || checkPickup(&paramData.startFreq, pot1Val)) { f0 = pot1Val * 1023.0f + 3.0f;                               paramData.startFreq.value = f0; }
    else { f0 = paramData.startFreq.value; }

    float pot2Val = readPotSmoothed(A1, pot2Buffer);
    if (firstRun || checkPickup(&paramData.endFreq, pot2Val))   { f1 = pot2Val * 510.5f + 2.0f;                                paramData.endFreq.value   = f1; }
    else { f1 = paramData.endFreq.value; }

    float pot3Val = readPotSmoothed(A2, pot3Buffer);
    if (firstRun || checkPickup(&paramData.curve, pot3Val))     { selectedCurve = min(NUM_CURVES-1, int(pot3Val * NUM_CURVES)); paramData.curve.value     = selectedCurve; }
    else { selectedCurve = (uint8_t)paramData.curve.value; }
  }

  firstRun = false;

  // ── Restore mode color after 80 ms trigger flash ──────────────────────────
  if (ledTrigOn && millis() >= ledTrigOffAt) {
    led.setPixelColor(0, selectMode ? led.Color(0, 220, 255) : led.Color(255, 140, 0));
    led.show();
    ledTrigOn = false;
  }
  // ──────────────────────────────────────────────────────────────────────────

  delay(10);
}

/* --------------------------------------------------
   Trigger ISR — untouched except LED flash at end
-------------------------------------------------- */
void onTrigger() {
  kickPlaying  = true;
  kickPhase    = 0.0f;
  reduce_state = digitalRead(0);

  irq_set_enabled(PWM_IRQ_WRAP, false);
  make_wavetable();

  const float invSamples   = 1.0f / 2047.0f;
  const float expStep      = expf(-decayRate * invSamples);
  float       env          = 1.0f;
  const float halfScale    = FULL_SCALE / 2.0f;
  const float invHalfScale = 2.0f / FULL_SCALE;
  const float clipNorm     = 1.0f / tanhf(softClipRate);
  const int   fadeStart    = int(2048 * 0.95f);
  const int   fadeDenom    = 2047 - fadeStart;
  const float invFadeDenom = 1.0f / fadeDenom;

  for (int i = 0; i < 2048; i++) {
    float bipolar    = (kickTable[i] - MID_LEVEL) * invHalfScale;
    float attenuated = bipolar * env;
    float clipped    = tanhf(softClipRate * attenuated) * clipNorm;
    float sampleOut  = clipped * halfScale + MID_LEVEL;

    if (i >= fadeStart) {
      float mu  = (i - fadeStart) * invFadeDenom;
      float mu2 = (1.0f - cosf(mu * PI)) * 0.5f;
      sampleOut = (1.0f - mu2) * sampleOut + mu2 * MID_LEVEL;
    }
    finalTable[i] = uint16_t(sampleOut);
    env *= expStep;
  }

  irq_set_enabled(PWM_IRQ_WRAP, true);

  // ── White flash 80 ms — restore handled in loop() ─────────────────────────
  led.setPixelColor(0, led.Color(255, 255, 255));
  led.show();
  ledTrigOn    = true;
  ledTrigOffAt = millis() + 80;
  // ──────────────────────────────────────────────────────────────────────────
}
