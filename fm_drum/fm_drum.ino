/*
HAGIWO MOD2 FM Drum Ver1.2 - WITH PICKUP FEATURE

  Two‑operator FM percussion voice for Eurorack.
  ‑ 3 potentiometers × 2 modes  →  6 parameters
  ‑ Accent input (GPIO0) cuts level by −6 dB when HIGH
  ‑ Wavetable synthesis; 4 096 samples per note
  ‑ Click‑free edges: 2 % cosine fade‑in / 10 % fade‑out
  ‑ All parameters stored in on‑board flash (EEPROM emulation)
  ‑ Pickup feature prevents value jumping when switching modes

--Pin assign---
POT1     A0       Mode0: Pitch               | Mode1: Decay Time
POT2     A1       Mode0: Operator Ratio      | Mode1: Ratio Envelope
POT3     A2       Mode0: Modulation Index    | Mode1: Modulation Index
IN1      GPIO7    Trig in
IN2      GPIO0    Accent  – level × 0.5 when HIGH
CV       A2       Shared with POT3
OUT      GPIO1    Audio PWM output (10‑bit)
BUTTON   GPIO6    Mode toggle & parameter save
LED      GPIO5    Mode indicator (ON = Mode 1)
EEPROM   save 6 parameter

CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial purposes, all without asking permission.

[History]
v1.2  - Add: Pickup feature for smooth parameter transitions
v1.1  - Fix: EEPROM-related malfunction
v1.0  - Init: Initial release
*/

#include <Arduino.h>
#include "hardware/pwm.h"  // RP2040 hardware PWM register access
#include "hardware/irq.h"  // IRQ helpers
#include <math.h>
#include <EEPROM.h>  // on‑flash key/value storage

/**********************  Global compile‑time constants  **********************/
constexpr int TABLE_SIZE = 4096;                      // wavetable length
constexpr int TABLE_MASK = TABLE_SIZE - 1;            // for cheap modulo
constexpr float TABLE_INV = 1.0f / (TABLE_SIZE - 1);  // 1/(N‑1) pre‑calc

const float SYS_CLOCK = 150'000'000.0f;                                  // RP2040 core clock (Hz)
const float NOTE_LEN = 0.3f;                                             // fixed note duration (s)
const float BASE_INC = (TABLE_SIZE * 4096.0f) / (NOTE_LEN * SYS_CLOCK);  // phase step per PWM tick
const float DT = NOTE_LEN / TABLE_SIZE;                                  // time per sample
const float FULL_SCALE = 1023.0f;                                        // 10‑bit PWM range
const float MID_LEVEL = FULL_SCALE / 2.0f;                               // mid‑rail => silence

// Pickup feature constants
const float PICKUP_THRESHOLD = 0.05f;  //  threshold for pot noise
const int POT_SMOOTH_SAMPLES = 5;      // Number of samples for averaging

/****************************  Real‑time parameters  *************************/
volatile float f0 = 200.0f;       // fundamental (Hz)
volatile float opRatio = 2.0f;    // operator frequency ratio
volatile float modIndex = 1.0f;   // modulation index   (1 … 10)
volatile float decayRate = 5.0f;  // amplitude decay rate
volatile float ratioEnv = 0.0f;   // ratio envelope depth
volatile float indexEnv = 0.3f;   // index envelope depth (fixed)

// Accent flag – updated on every trigger edge
volatile bool accentState = false;  // true = level × 0.5

/**************************  Playback state variables  **********************/
volatile bool noteOn = false;     // true while table is streamed
volatile float phase = 0.0f;      // fractional table index
volatile float softClipK = 1.0f;  // tanh drive constant (= modIndex)

/****************************  Wavetable buffers  ****************************/
uint16_t rawTable[TABLE_SIZE];    // pure FM signal (0‑FULL_SCALE)
uint16_t finalTable[TABLE_SIZE];  // clipped & tapered copy streamed by PWM

/**********************  PWM slice numbers (set in setup)  *******************/
uint sliceAudio;  // GPIO1 – PWM channel B → audio
uint sliceTimer;  // GPIO2 – dummy PWM used only for IRQ timing

/**********************  Pickup Feature Data Structure  **********************/
struct ParameterData {
  float value;           // Current parameter value
  float targetValue;     // Target value when switching modes
  bool pickupActive;     // True if waiting for pot to catch up
  float lastPotValue;    // Last raw pot reading (0-1)
};

// Structure to hold all 6 parameters
struct {
  ParameterData pitch;        // Mode 0, POT1
  ParameterData operatorRatio;// Mode 0, POT2
  ParameterData modIndexM0;   // Mode 0, POT3
  ParameterData decayTime;    // Mode 1, POT1
  ParameterData ratioEnvelope;// Mode 1, POT2
  ParameterData modIndexM1;   // Mode 1, POT3
} paramData;

// Pot smoothing buffers
float pot1Buffer[POT_SMOOTH_SAMPLES] = {0};
float pot2Buffer[POT_SMOOTH_SAMPLES] = {0};
float pot3Buffer[POT_SMOOTH_SAMPLES] = {0};
int potBufferIndex = 0;

/**************************  PWM wrap interrupt (audio ISR) ******************/
void on_pwm_wrap() {
  pwm_clear_irq(sliceTimer);  // acknowledge IRQ

  if (!noteOn) {  // idle → output mid‑rail
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)MID_LEVEL);
    return;
  }

  // Linear interpolation between table samples
  float idx = phase;
  uint32_t i = (uint32_t)idx;
  float frac = idx - i;
  uint16_t s1 = finalTable[i];
  uint16_t s2 = finalTable[(i + 1) & TABLE_MASK];
  float y = s1 * (1.0f - frac) + s2 * frac;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)y);

  // advance phase; stop after one table pass
  phase += BASE_INC;
  if (phase >= (float)TABLE_SIZE) {
    noteOn = false;
    phase = 0.0f;
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)MID_LEVEL);
  }
}

/************************  FM wavetable generation ***************************/
void make_wavetable() {
  float phaseC = 0.0f;
  float phaseM = 0.0f;
  const float twoPi = 2.0f * PI;
  const float stepC = twoPi * f0 * DT;  // carrier increment

  for (int i = 0; i < TABLE_SIZE; ++i) {
    float x = i * TABLE_INV;           // 0 … 1 across table
    float envR = 1.0f - ratioEnv * x;  // ratio envelope
    float envI = 1.0f - indexEnv * x;  // index envelope

    float stepM = twoPi * f0 * (opRatio * envR) * DT;  // modulator inc
    phaseM += stepM;
    phaseC += stepC;

    float sample = sinf(phaseC + (modIndex * envI) * sinf(phaseM));
    rawTable[i] = (uint16_t)((sample + 1.0f) * (FULL_SCALE / 2.0f));
  }
}

/************************  Pot reading with smoothing  ***********************/
float readPotSmoothed(int pin, float* buffer) {
  // Read and store in circular buffer
  buffer[potBufferIndex] = analogRead(pin) / 1023.0f;

  // Calculate average
  float sum = 0;
  for (int i = 0; i < POT_SMOOTH_SAMPLES; i++) {
    sum += buffer[i];
  }
  return sum / POT_SMOOTH_SAMPLES;
}

/************************  Pickup feature implementation  *********************/
bool checkPickup(ParameterData* param, float currentPotValue) {
  if (!param->pickupActive) {
    return true;  // No pickup needed
  }

  // Calculate normalized target position (0-1)
  float normalizedTarget = param->targetValue;

  // Check if pot has crossed the target value
  bool crossedFromBelow = (param->lastPotValue < normalizedTarget - PICKUP_THRESHOLD) &&
                          (currentPotValue >= normalizedTarget - PICKUP_THRESHOLD);
  bool crossedFromAbove = (param->lastPotValue > normalizedTarget + PICKUP_THRESHOLD) &&
                          (currentPotValue <= normalizedTarget + PICKUP_THRESHOLD);

  if (crossedFromBelow || crossedFromAbove ||
      fabs(currentPotValue - normalizedTarget) < PICKUP_THRESHOLD) {
    param->pickupActive = false;  // Pickup complete
    return true;
  }

  param->lastPotValue = currentPotValue;
  return false;  // Still waiting for pickup
}

/************************  Initialize parameter data  ************************/
void initParameterData() {
  // Initialize all parameters with default values
  paramData.pitch.value = 200.0f;
  paramData.pitch.pickupActive = false;

  paramData.operatorRatio.value = 2.0f;
  paramData.operatorRatio.pickupActive = false;

  paramData.modIndexM0.value = 1.0f;
  paramData.modIndexM0.pickupActive = false;

  paramData.decayTime.value = 5.0f;
  paramData.decayTime.pickupActive = false;

  paramData.ratioEnvelope.value = 0.0f;
  paramData.ratioEnvelope.pickupActive = false;

  paramData.modIndexM1.value = 1.0f;
  paramData.modIndexM1.pickupActive = false;
}

/*******************************  Arduino setup  *****************************/
void setup() {
  // Initialize parameter data
  initParameterData();

  // --- restore parameters from flash --------------------------------------
  EEPROM.begin(64);
  float temp;
  EEPROM.get(0, temp);
  if (!isnan(temp) && temp >= 30.0f && temp <= 1200.0f) paramData.pitch.value = temp;
  f0 = paramData.pitch.value;

  EEPROM.get(4, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 8.0f) paramData.operatorRatio.value = temp;
  opRatio = paramData.operatorRatio.value;

  EEPROM.get(8, temp);
  if (!isnan(temp) && temp >= 1.0f && temp <= 10.0f) paramData.modIndexM0.value = temp;
  paramData.modIndexM1.value = paramData.modIndexM0.value;
  modIndex  = paramData.modIndexM0.value;
  if (modIndex < 1.0f) modIndex = 1.0f;
  softClipK = modIndex;

  EEPROM.get(12, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 10.0f) paramData.decayTime.value = temp;
  decayRate = paramData.decayTime.value;

  EEPROM.get(16, temp);
  if (!isnan(temp) && temp >= 0.0f && temp <= 1.0f) paramData.ratioEnvelope.value = temp;
  ratioEnv = paramData.ratioEnvelope.value;

  make_wavetable();
  memcpy(finalTable, rawTable, sizeof(rawTable));

  // --- PWM setup -----------------------------------------------------------
  pinMode(1, OUTPUT);
  gpio_set_function(1, GPIO_FUNC_PWM);  // audio PWM
  pinMode(2, OUTPUT);
  gpio_set_function(2, GPIO_FUNC_PWM);  // timer PWM
  sliceAudio = pwm_gpio_to_slice_num(1);
  sliceTimer = pwm_gpio_to_slice_num(2);

  pwm_set_clkdiv(sliceAudio, 1);
  pwm_set_wrap(sliceAudio, 1023);
  pwm_set_enabled(sliceAudio, true);
  pwm_set_clkdiv(sliceTimer, 1);
  pwm_set_wrap(sliceTimer, 4095);
  pwm_set_enabled(sliceTimer, true);
  pwm_clear_irq(sliceTimer);
  pwm_set_irq_enabled(sliceTimer, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  // --- GPIO setup ----------------------------------------------------------
  pinMode(7, INPUT);         // trigger input
  pinMode(0, INPUT);         // accent input (active HIGH)
  pinMode(6, INPUT_PULLUP);  // mode button
  pinMode(5, OUTPUT);        // mode LED

  // attach trigger ISR (inline lambda for brevity) -------------------------
  attachInterrupt(
    digitalPinToInterrupt(7), []() {
      noteOn = true;
      phase = 0.0f;
      accentState = digitalRead(0);  // sample accent pin

      // protect against audio ISR while rebuilding tables
      irq_set_enabled(PWM_IRQ_WRAP, false);
      make_wavetable();

      // envelope & final‑table post‑process
      const int fadeInEnd = (int)(TABLE_SIZE * 0.02f);     // 2 % head ramp
      const int fadeOutStart = (int)(TABLE_SIZE * 0.90f);  // 10 % tail ramp
      const int fadeOutDen = TABLE_SIZE - 1 - fadeOutStart;
      const float invN = 1.0f / (TABLE_SIZE - 1);
      const float expStep = expf(-decayRate * invN);
      const float halfScale = FULL_SCALE / 2.0f;
      const float invHalfScale = 2.0f / FULL_SCALE;
      const float clipNorm = 1.0f / tanhf(softClipK);
      const float level = accentState ? 0.5f : 1.0f;  // −6 dB accent

      float env = 1.0f;
      for (int i = 0; i < TABLE_SIZE; ++i) {
        float bipolar = (rawTable[i] - MID_LEVEL) * invHalfScale;
        float clipped = tanhf(softClipK * bipolar * env) * clipNorm;
        clipped *= level;  // apply accent
        float y = clipped * halfScale + MID_LEVEL;

        // fade‑in (first 2 %)
        if (i < fadeInEnd) {
          float mu = (float)i / fadeInEnd;
          float mu2 = (1.0f - cosf(mu * PI)) * 0.5f;
          y = (1.0f - mu2) * MID_LEVEL + mu2 * y;
        }
        // fade‑out (last 10 %)
        else if (i >= fadeOutStart) {
          float mu = (float)(i - fadeOutStart) / fadeOutDen;
          float mu2 = (1.0f - cosf(mu * PI)) * 0.5f;
          y = (1.0f - mu2) * y + mu2 * MID_LEVEL;
        }
        finalTable[i] = (uint16_t)y;
        env *= expStep;
      }
      irq_set_enabled(PWM_IRQ_WRAP, true);  // resume audio ISR
    },
    RISING);

  // Initialize pot buffers with current readings
  for (int i = 0; i < POT_SMOOTH_SAMPLES; i++) {
    pot1Buffer[i] = analogRead(A0) / 1023.0f;
    pot2Buffer[i] = analogRead(A1) / 1023.0f;
    pot3Buffer[i] = analogRead(A2) / 1023.0f;
  }
}

/*****************************  Main loop (UI)  ******************************/
void loop() {
  static bool editMode = false;  // false: Mode0, true: Mode1
  static bool prevBtn = HIGH;
  bool currBtn = digitalRead(6);

  // --- handle button press -------------------------------------------------
  if (prevBtn == HIGH && currBtn == LOW) {
    // Save current values before switching modes
    if (!editMode) {
      // Leaving Mode 0, save Mode 0 values
      paramData.pitch.value = f0;
      paramData.operatorRatio.value = opRatio;
      paramData.modIndexM0.value = modIndex;
    } else {
      // Leaving Mode 1, save Mode 1 values
      paramData.decayTime.value = decayRate;
      paramData.ratioEnvelope.value = ratioEnv;
      paramData.modIndexM1.value = modIndex;
    }

    editMode = !editMode;       // toggle mode
    digitalWrite(5, editMode);  // LED reflects current mode

    // Set up pickup targets for the new mode
    if (!editMode) {
      // Entering Mode 0
      // Normalize target values to 0-1 range for pot comparison
      paramData.pitch.targetValue = (paramData.pitch.value - 30.0f) / 1170.0f;
      paramData.pitch.pickupActive = true;

      paramData.operatorRatio.targetValue = (paramData.operatorRatio.value - 0.5f) / 7.5f;
      paramData.operatorRatio.pickupActive = true;

      paramData.modIndexM0.targetValue = 1.0f - ((paramData.modIndexM0.value - 1.0f) / 9.0f);
      paramData.modIndexM0.pickupActive = true;
    } else {
      // Entering Mode 1
      paramData.decayTime.targetValue = 1.0f - ((paramData.decayTime.value - 0.5f) / 9.5f);
      paramData.decayTime.pickupActive = true;

      paramData.ratioEnvelope.targetValue = 1.0f - paramData.ratioEnvelope.value;
      paramData.ratioEnvelope.pickupActive = true;

      paramData.modIndexM1.targetValue = 1.0f - ((paramData.modIndexM1.value - 1.0f) / 9.0f);
      paramData.modIndexM1.pickupActive = true;
    }

    // Save all parameters to EEPROM
    EEPROM.put(0, paramData.pitch.value);
    EEPROM.put(4, paramData.operatorRatio.value);
    EEPROM.put(8, paramData.modIndexM0.value);
    EEPROM.put(12, paramData.decayTime.value);
    EEPROM.put(16, paramData.ratioEnvelope.value);
    EEPROM.commit();
  }
  prevBtn = currBtn;

  // Update buffer index
  potBufferIndex = (potBufferIndex + 1) % POT_SMOOTH_SAMPLES;

  // --- read potentiometers with pickup feature ----------------------------
  if (!editMode) {  //  Mode 0 : Pitch / Ratio / Index
    float pot1Val = readPotSmoothed(A0, pot1Buffer);
    if (checkPickup(&paramData.pitch, pot1Val)) {
      f0 = 30.0f + 1170.0f * pot1Val;
      paramData.pitch.value = f0;
    } else {
      f0 = paramData.pitch.value;  // Use stored value
    }

    float pot2Val = readPotSmoothed(A1, pot2Buffer);
    if (checkPickup(&paramData.operatorRatio, pot2Val)) {
      opRatio = 0.5f + 7.5f * pot2Val;
      paramData.operatorRatio.value = opRatio;
    } else {
      opRatio = paramData.operatorRatio.value;  // Use stored value
    }

    float pot3Val = readPotSmoothed(A2, pot3Buffer);
    if (checkPickup(&paramData.modIndexM0, pot3Val)) {
      modIndex = 1.0f + 9.0f * (1.0f - pot3Val);
      paramData.modIndexM0.value = modIndex;
      softClipK = modIndex;
    } else {
      modIndex = paramData.modIndexM0.value;  // Use stored value
      softClipK = modIndex;
    }

  } else {  //  Mode 1 : Decay / RatioEnv / Index
    float pot1Val = readPotSmoothed(A0, pot1Buffer);
    if (checkPickup(&paramData.decayTime, pot1Val)) {
      decayRate = 0.5f + 9.5f * (1.0f - pot1Val);
      paramData.decayTime.value = decayRate;
    } else {
      decayRate = paramData.decayTime.value;  // Use stored value
    }

    float pot2Val = readPotSmoothed(A1, pot2Buffer);
    if (checkPickup(&paramData.ratioEnvelope, pot2Val)) {
      ratioEnv = 1.0f - pot2Val;
      paramData.ratioEnvelope.value = ratioEnv;
    } else {
      ratioEnv = paramData.ratioEnvelope.value;  // Use stored value
    }

    float pot3Val = readPotSmoothed(A2, pot3Buffer);
    if (checkPickup(&paramData.modIndexM1, pot3Val)) {
      modIndex = 1.0f + 9.0f * (1.0f - pot3Val);
      paramData.modIndexM1.value = modIndex;
      softClipK = modIndex;
    } else {
      modIndex = paramData.modIndexM1.value;  // Use stored value
      softClipK = modIndex;
    }
  }

  delay(10);  // simple UI debounce / CPU breather
}
