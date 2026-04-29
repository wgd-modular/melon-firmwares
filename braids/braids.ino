/*
  (c) 2025 blueprint@poetaster.de
  GPLv3 the libraries are MIT as the originals for STM from MI were also MIT.
*/

bool debug = true;

#include <Arduino.h>
#include "stdio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "potentiometer.h"

long midiTimer;

float pitch_offset = 36;
float max_voltage_of_adc = 3.3;
float voltage_division_ratio = 0.3333333333333;
float notes_per_octave = 12;
float volts_per_octave = 1;

float mapping_upper_limit = (max_voltage_of_adc / voltage_division_ratio) * notes_per_octave * volts_per_octave;

#include <hardware/pwm.h>
#include <PWMAudio.h>

#define SAMPLERATE  48000
#define PWMOUT      D7
#define BUTTON_PIN  D4
#define TRIG_PIN    D5

// ── WS2812B ───────────────────────────────────────────────────────────────────
#include <Adafruit_NeoPixel.h>
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Engine group colors (indexed by engine number 0–46)
// Groups:  0–9  Classic analog  → amber
//         10–14  Filtered/Z      → yellow
//         15–17  Vocal/Formant   → green
//         18–21  FM/Harmonic     → blue
//         22–30  Physical model  → pink
//         31–34  Wavetable       → purple
//         35–46  Noise/Digital   → red  (trigger flash = white)
uint32_t engineGroupColor(int engine) {
  if (engine <=  9) return 0xFF8C00;   // amber
  if (engine <= 14) return 0xFFE000;   // yellow
  if (engine <= 17) return 0x00FF80;   // green
  if (engine <= 21) return 0x0088FF;   // blue
  if (engine <= 30) return 0xFF00AA;   // pink
  if (engine <= 34) return 0xAA00FF;   // purple
  return 0xFF1800;                     // red  (noise/digital/unknown 35–46)
}

bool     ledTrigOn    = false;
uint32_t ledTrigOffAt = 0;
// ──────────────────────────────────────────────────────────────────────────────

#include "utility.h"
#include <STMLIB.h>
#include <BRAIDS.h>
#include "braids.h"

#include <Bounce2.h>
Bounce2::Button button = Bounce2::Button();

PWMAudio DAC(PWMOUT);

const char* engineNames[47] = {
  "CSAW", "MORPH", "SAW_SQ", "FOLD", "SQ_SUB", "SAW_SUB", "SQ_SYNC",
  "SAW_3", "SQ_3", "SAW_COMB", "TOY", "ZLPF", "ZPKF", "ZBPF", "ZHPF",
  "VOSIM", "VOWEL", "VOW_FOF", "HARM", "FM", "FBFM", "WTFM",
  "PLUCK", "BOW", "BLOW", "FLUTE", "BELL", "DRUM", "KICK", "CYMBAL", "SNARE",
  "WTBL", "WMAP", "WLIN", "WTx4",
  "NOISE", "TWNQ", "CLKN", "CLOUD", "PRTC", "QPSK",
  "ENG41", "ENG42", "ENG43", "ENG44", "ENG45", "ENG46"
};

int  engineCount      = 0;
int  engineInc        = 0;
bool longPressHandled = false;

#define TIMER_INTERRUPT_DEBUG     0
#define _TIMERINTERRUPT_LOGLEVEL_ 4

#include "RPi_Pico_TimerInterrupt.h"

#define TIMER0_INTERVAL_MS   20.833333333333
#define DEBOUNCING_INTERVAL_MS 2
#define LOCAL_DEBUG            0

volatile int counter = 0;

RPI_PICO_Timer ITimer0(0);

bool TimerHandler0(struct repeating_timer *t) {
  (void) t;
  bool sync = true;
  if (DAC.availableForWrite()) {
    for (size_t i = 0; i < BLOCK_SIZE; i++) {
      DAC.write(voices[0].pd.buffer[i], sync);
    }
    counter = 1;
  }
  return true;
}

void cb() {
  bool sync = true;
  if (DAC.availableForWrite() >= BLOCK_SIZE) {
    for (int i = 0; i < BLOCK_SIZE; i++) {
      DAC.write(voices[0].pd.buffer[i]);
    }
  }
}

void HandleNoteOn(byte channel, byte note, byte velocity) {
  pitch_in   = note << 7;
  trigger_in = velocity / 127.0;
}
void HandleNoteOff(byte channel, byte note, byte velocity) {
  trigger_in = 0.0f;
}

void setup() {
  if (debug) {
    Serial.begin(57600);
    Serial.println(F("=== MOD2 BRAIDS FIRMWARE ==="));
  }

  engineCount = 22;  // PLUCK (default)
  engine_in   = engineCount;

  if (debug) {
    Serial.print(F("Default engine: "));
    Serial.print(engineCount);
    Serial.print(F(" - "));
    Serial.println(engineNames[engineCount]);
  }

  analogReadResolution(12);
  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);

  pinMode(TRIG_PIN,  INPUT_PULLDOWN);
  pinMode(AIN0, INPUT);
  pinMode(AIN1, INPUT);
  pinMode(AIN2, INPUT);
  pinMode(SCL,  INPUT_PULLDOWN);

  // ── WS2812B init (replaces pinMode(LED, OUTPUT)) ──────────────────────────
  led.begin();
  led.setBrightness(180);
  led.setPixelColor(0, engineGroupColor(engineCount));  // boot: PLUCK = pink
  led.show();
  // ──────────────────────────────────────────────────────────────────────────

  button.attach(BUTTON_PIN, INPUT_PULLUP);
  button.interval(5);
  button.setPressedState(LOW);

  if (ITimer0.attachInterruptInterval(TIMER0_INTERVAL_MS, TimerHandler0)) {
    if (debug) Serial.print(F("Starting ITimer0 OK, millis() = ")); Serial.println(millis());
  } else {
    if (debug) Serial.println(F("Can't set ITimer0. Select another freq. or timer"));
  }

  DAC.setBuffers(4, 32);
  DAC.setFrequency(SAMPLERATE);
  DAC.begin();

  initVoices();
  voices[0].pd.osc->set_shape(static_cast<braids::MacroOscillatorShape>(engine_in));

  if (debug) {
    Serial.print(F("Oscillator shape set to engine: "));
    Serial.println(engine_in);
  }

  readpot(0);
  readpot(1);
  readpot(2);

  int16_t timbre = map(potvalue[0], POT_MIN, POT_MAX, 0, 32767);
  timbre_in = timbre;
  int16_t morph = map(potvalue[1], POT_MIN, POT_MAX, 0, 32767);
  morph_in  = morph;

  midiTimer = millis();
}

void loop() {
  if (counter > 0) {
    updateBraidsAudio();
    counter = 0;
  }
}

// ── Second core: UI / control rate — LED handling here only ───────────────────

void setup1() {
  delay(200);
}

void loop1() {
  uint32_t now = millis();

  int16_t timbre = map(potvalue[0], POT_MIN, POT_MAX, 0, 32767);
  timbre_in = timbre;
  int16_t morph = map(potvalue[1], POT_MIN, POT_MAX, 0, 32767);
  morph_in  = morph;

  int16_t pitch       = map(potvalue[2], POT_MIN, POT_MAX, 3072, 8192);
  int16_t pitch_delta = abs(previous_pitch - pitch);
  if (pitch_delta > 10) {
    pitch_in       = pitch;
    previous_pitch = pitch;
  }

  button.update();

  // Long press: decrement engine
  if (button.isPressed() && button.currentDuration() > 500) {
    if (!longPressHandled) {
      engineCount--;
      if (engineCount < 0) engineCount = 46;
      engine_in = engineCount;
      longPressHandled = true;

      // ── Update group color on engine change ───────────────────────────────
      led.setPixelColor(0, engineGroupColor(engineCount));
      led.show();
      // ──────────────────────────────────────────────────────────────────────

      if (debug) {
        Serial.print(F("Engine (long): "));
        Serial.print(engineCount);
        Serial.print(F(" - "));
        Serial.println(engineNames[engineCount]);
      }
    }
  } else if (button.released()) {
    // Short press: increment engine
    if (!longPressHandled) {
      engineCount++;
      if (engineCount > 46) engineCount = 0;
      engine_in = engineCount;

      // ── Update group color on engine change ───────────────────────────────
      led.setPixelColor(0, engineGroupColor(engineCount));
      led.show();
      // ──────────────────────────────────────────────────────────────────────

      if (debug) {
        Serial.print(F("Engine: "));
        Serial.print(engineCount);
        Serial.print(F(" - "));
        Serial.println(engineNames[engineCount]);
      }
    }
    longPressHandled = false;
  }

   if (digitalRead(TRIG_PIN)) {
      trigger_in = 1.0f;
      // ── White trigger flash ───────────────────────────────────────────────
      if (!ledTrigOn) {
        led.setPixelColor(0, led.Color(255, 255, 255));
        led.show();
        ledTrigOn    = true;
        ledTrigOffAt = now + 80;
      }
      // ──────────────────────────────────────────────────────────────────────
    } else {
      trigger_in = 0.0f;
    }

  if ((now - pot_timer) > POT_SAMPLE_TIME) {
    readpot(0);
    readpot(1);
    readpot(2);

    pot_timer = now;
  }

  // ── Restore engine group color after trigger flash ────────────────────────
  if (ledTrigOn && now >= ledTrigOffAt) {
    led.setPixelColor(0, engineGroupColor(engineCount));
    led.show();
    ledTrigOn = false;
  }
  // ──────────────────────────────────────────────────────────────────────────
}
