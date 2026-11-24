/*
  Cherry for Melon
  ================
  Hagiwo's additive VCO, the firmware of WGD Modular's Cherry module, on
  Melon. It adds up a fundamental and seven harmonics, all sine waves. POT1
  sets how loud each partial is and POT2 which harmonics they are, both through
  Hagiwo's tables with 256 settings each.

  WGD's Cherry module plays six of the eight partials, its code left out the
  last two because of pops. Here all eight play, scaled so that they never
  clip.

  Pin assign (MOD2 standard):
  POT1    A0     Harmonic gain
  POT2    A1     Harmonic spectrum
  POT3    A2     Pitch (knob + 1V/oct CV)
  IN1     GPIO7  not used
  IN2     GPIO0  not used
  OUT     GPIO1  Audio out (PWMAudio, 16-bit at 48 kHz)
  BUTTON  GPIO6  Octave up: 0, +1, +2, +3
  LED     GPIO5  WS2812B, the colour shows the octave

  Hagiwo's additive VCO: https://note.com/solder_state/n/n30b3a8737b1e
*/

#include <Arduino.h>
#include <PWMAudio.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

const float SAMPLE_RATE = 48000.0f;
const int PARTIALS = 8;  // and as many sine oscillators to play them

// POT3 and the CV jack are summed onto A2 through an inverting stage, so a
// higher voltage reads lower. The ADC range spans about five octaves at
// 1V/oct, the same scaling as the VCO, Braids and Reese Bass firmwares.
// TUNE_CAL trims the tracking against a tuner if needed.
const float TUNE_CAL = 0.992f;
const float CV_OCTAVES = 8.3f * (33.0f / 55.0f) * TUNE_CAL;
const float COUNTS_PER_SEMITONE = 4095.0f / (CV_OCTAVES * 12.0f);
const float BOTTOM_HZ = 32.7032f;  // C1, POT3 fully left without CV

// Partials fade out between these frequencies, before they would fold back
// from half the sample rate.
const float FADE_START_HZ = 16000.0f;
const float FADE_END_HZ = 20000.0f;

// A harmonic fades in or out with a time constant of 7 ms instead of
// clicking. One that the pitch pushes past the top cannot follow it, so it
// fades out on its old frequency with a time constant of 1.5 ms instead. If a
// harmonic of the new note comes within about a semitone of it there, it
// moves up to PARKED_HZ, where no harmonic plays, so the two do not cancel
// out.
const float LEVEL_SMOOTHING = 1.0f / (0.007f * SAMPLE_RATE);
const float RELEASE_SMOOTHING = 1.0f / (0.0015f * SAMPLE_RATE);
const float PARKED_HZ = (FADE_END_HZ + SAMPLE_RATE / 2.0f) / 2.0f;
const uint32_t PARKED_INCREMENT = (uint32_t)(PARKED_HZ * (4294967296.0f / SAMPLE_RATE));

// POT1 and POT2 are smoothed against ADC noise, once per ms.
const float POT_SMOOTHING = 0.1f;
const float POT_JUMP = 4.0f / 256.0f;

#define BUTTON_PIN 6
#define LED_PIN 5
const uint32_t OCTAVE_COLOURS[4] = { 0xFF1020, 0xFF7000, 0x30FF30, 0x2060FF };

PWMAudio audio(1);
Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);

// Level of each partial, 0 to 255, for the 256 positions of the gain control.
// Row 0 is the fundamental, rows 1 to 7 the harmonics.
const uint8_t GAIN_TABLE[8][256] = {
  {
    255, 255, 254, 253, 251, 249, 247, 244, 242, 238, 235, 231, 227, 223, 218, 213,
    208, 203, 197, 192, 186, 180, 173, 167, 161, 154, 148, 141, 134, 128, 121, 114,
    108, 101,  94,  88,  82,  76,  70,  64,  58,  52,  47,  42,  37,  32,  28,  24,
     20,  17,  14,  11,   8,   6,   4,   2,   1,   0,   0,   0,   0,   0,   1,   2,
      4,   6,   8,  10,  13,  17,  20,  24,  28,  32,  37,  42,  47,  52,  58,  63,
     69,  75,  81,  88,  94, 101, 107, 114, 120, 127, 134, 141, 147, 154, 160, 167,
    173, 179, 185, 191, 197, 202, 208, 213, 218, 222, 227, 231, 235, 238, 241, 244,
    247, 249, 251, 253, 254, 255, 255, 255, 255, 255, 254, 253, 251, 249, 247, 245,
    242, 239, 235, 231, 227, 223, 218, 213, 208, 203, 198, 192, 186, 180, 174, 168,
    161, 155, 148, 141, 135, 128, 121, 115, 108, 101,  95,  88,  82,  76,  70,  64,
     58,  53,  47,  42,  37,  33,  28,  24,  20,  17,  14,  11,   8,   6,   4,   2,
      1,   0,   0,   0,   0,   0,   1,   2,   4,   6,   8,  10,  13,  16,  20,  24,
     28,  32,  37,  41,  46,  52,  57,  63,  69,  75,  81,  87,  94, 100, 107, 113,
    120, 127, 133, 140, 147, 153, 160, 166, 173, 179, 185, 191, 197, 202, 207, 213,
    217, 222, 226, 231, 234, 238, 241, 244, 247, 249, 251, 253, 254, 255, 255, 255,
    255, 255, 254, 253, 251, 249, 247, 245, 242, 239, 235, 232, 228, 223, 219, 214,
  },
  {
    221, 214, 207, 199, 191, 182, 173, 163, 153, 142, 132, 121, 111, 100,  90,  80,
     70,  60,  51,  43,  35,  28,  22,  16,  11,   7,   4,   1,   0,   0,   0,   1,
      4,   7,  11,  16,  21,  28,  35,  43,  51,  60,  69,  79,  89, 100, 110, 121,
    131, 142, 152, 162, 172, 182, 191, 199, 207, 214, 221, 226, 231, 235, 238, 241,
    242, 243, 242, 241, 239, 236, 231, 227, 221, 215, 207, 200, 191, 182, 173, 163,
    153, 143, 132, 122, 111, 100,  90,  80,  70,  61,  52,  43,  35,  28,  22,  16,
     11,   7,   4,   1,   0,   0,   0,   1,   3,   7,  11,  15,  21,  28,  35,  42,
     51,  60,  69,  79,  89,  99, 110, 120, 131, 142, 152, 162, 172, 181, 190, 199,
    207, 214, 220, 226, 231, 235, 238, 241, 242, 243, 242, 241, 239, 236, 232, 227,
    221, 215, 208, 200, 192, 183, 173, 163, 153, 143, 133, 122, 111, 101,  90,  80,
     71,  61,  52,  44,  36,  29,  22,  16,  11,   7,   4,   2,   0,   0,   0,   1,
      3,   6,  10,  15,  21,  27,  34,  42,  51,  59,  69,  79,  89,  99, 109, 120,
    131, 141, 152, 162, 171, 181, 190, 198, 206, 214, 220, 226, 231, 235, 238, 241,
    242, 243, 242, 241, 239, 236, 232, 227, 221, 215, 208, 200, 192, 183, 174, 164,
    154, 143, 133, 122, 112, 101,  91,  81,  71,  61,  52,  44,  36,  29,  22,  16,
     11,   7,   4,   2,   0,   0,   0,   1,   3,   6,  10,  15,  21,  27,  34,  42,
  },
  {
    165, 158, 150, 143, 135, 127, 119, 111, 103,  95,  87,  79,  72,  64,  57,  50,
     44,  38,  32,  27,  22,  17,  13,  10,   7,   4,   2,   1,   0,   0,   0,   1,
      2,   4,   6,   9,  13,  17,  21,  26,  32,  37,  44,  50,  57,  64,  71,  79,
     87,  94, 102, 110, 118, 126, 134, 142, 150, 158, 165, 172, 179, 185, 192, 197,
    203, 208, 212, 216, 220, 223, 225, 227, 229, 230, 230, 230, 229, 227, 226, 223,
    220, 217, 213, 208, 203, 198, 192, 186, 179, 173, 166, 158, 151, 143, 135, 127,
    119, 111, 103,  95,  87,  80,  72,  65,  58,  51,  44,  38,  32,  27,  22,  17,
     13,  10,   7,   4,   2,   1,   0,   0,   0,   1,   2,   4,   6,   9,  13,  17,
     21,  26,  31,  37,  43,  50,  57,  64,  71,  79,  86,  94, 102, 110, 118, 126,
    134, 142, 150, 157, 165, 172, 179, 185, 191, 197, 203, 207, 212, 216, 220, 223,
    225, 227, 229, 230, 230, 230, 229, 228, 226, 223, 220, 217, 213, 208, 203, 198,
    192, 186, 180, 173, 166, 159, 151, 143, 136, 128, 120, 112, 103,  96,  88,  80,
     72,  65,  58,  51,  44,  38,  32,  27,  22,  17,  13,  10,   7,   4,   2,   1,
      0,   0,   0,   0,   2,   4,   6,   9,  13,  17,  21,  26,  31,  37,  43,  49,
     56,  63,  71,  78,  86,  94, 102, 110, 118, 126, 134, 142, 149, 157, 164, 171,
    178, 185, 191, 197, 202, 207, 212, 216, 219, 223, 225, 227, 229, 230, 230, 230,
  },
  {
     97,  86,  75,  64,  54,  44,  36,  28,  20,  14,   9,   5,   2,   0,   0,   0,
      2,   5,   9,  14,  20,  27,  35,  44,  54,  64,  74,  85,  97, 108, 119, 131,
    142, 152, 162, 172, 181, 189, 196, 202, 208, 212, 215, 216, 217, 217, 215, 212,
    208, 203, 197, 189, 181, 173, 163, 153, 142, 131, 120, 109,  97,  86,  75,  64,
     54,  45,  36,  28,  21,  14,   9,   5,   2,   0,   0,   0,   2,   5,   9,  14,
     20,  27,  35,  44,  53,  64,  74,  85,  96, 108, 119, 130, 141, 152, 162, 172,
    181, 189, 196, 202, 207, 212, 215, 216, 217, 217, 215, 212, 208, 203, 197, 190,
    182, 173, 163, 153, 143, 132, 120, 109,  98,  86,  75,  65,  55,  45,  36,  28,
     21,  14,   9,   5,   2,   0,   0,   0,   2,   5,   9,  14,  20,  27,  35,  44,
     53,  63,  74,  85,  96, 107, 119, 130, 141, 152, 162, 171, 180, 188, 196, 202,
    207, 211, 215, 216, 217, 217, 215, 212, 208, 203, 197, 190, 182, 173, 164, 154,
    143, 132, 121, 109,  98,  87,  76,  65,  55,  45,  36,  28,  21,  15,   9,   5,
      2,   0,   0,   0,   2,   4,   8,  13,  20,  27,  35,  43,  53,  63,  73,  84,
     96, 107, 118, 130, 141, 151, 162, 171, 180, 188, 196, 202, 207, 211, 214, 216,
    217, 217, 215, 212, 208, 203, 197, 190, 182, 173, 164, 154, 143, 132, 121, 110,
     98,  87,  76,  65,  55,  46,  37,  28,  21,  15,  10,   5,   2,   0,   0,   0,
  },
  {
     46,  42,  38,  33,  30,  26,  22,  19,  16,  13,  11,   8,   6,   5,   3,   2,
      1,   0,   0,   0,   0,   0,   1,   2,   3,   4,   6,   8,  11,  13,  16,  19,
     22,  26,  29,  33,  37,  42,  46,  51,  55,  60,  65,  70,  75,  80,  86,  91,
     96, 102, 107, 112, 118, 123, 128, 133, 138, 143, 148, 153, 157, 162, 166, 170,
    174, 178, 181, 185, 188, 190, 193, 195, 197, 199, 201, 202, 203, 204, 204, 204,
    204, 204, 203, 202, 201, 199, 198, 196, 193, 191, 188, 185, 182, 178, 175, 171,
    167, 162, 158, 153, 149, 144, 139, 134, 129, 124, 118, 113, 108, 102,  97,  92,
     86,  81,  76,  71,  66,  61,  56,  51,  46,  42,  38,  34,  30,  26,  23,  19,
     16,  13,  11,   9,   6,   5,   3,   2,   1,   0,   0,   0,   0,   0,   1,   2,
      3,   4,   6,   8,  10,  13,  16,  19,  22,  25,  29,  33,  37,  41,  46,  50,
     55,  60,  65,  70,  75,  80,  85,  91,  96, 101, 107, 112, 117, 123, 128, 133,
    138, 143, 148, 153, 157, 162, 166, 170, 174, 178, 181, 184, 187, 190, 193, 195,
    197, 199, 201, 202, 203, 204, 204, 204, 204, 204, 203, 202, 201, 199, 198, 196,
    193, 191, 188, 185, 182, 178, 175, 171, 167, 163, 158, 154, 149, 144, 139, 134,
    129, 124, 119, 113, 108, 103,  97,  92,  87,  81,  76,  71,  66,  61,  56,  51,
     47,  42,  38,  34,  30,  26,  23,  20,  16,  14,  11,   9,   7,   5,   3,   2,
  },
  {
      9,   5,   3,   1,   0,   0,   0,   1,   3,   5,   8,  12,  17,  22,  27,  34,
     40,  47,  55,  62,  70,  79,  87,  95, 104, 112, 120, 128, 136, 143, 150, 157,
    163, 169, 174, 178, 182, 186, 188, 190, 191, 191, 191, 190, 188, 186, 183, 179,
    174, 169, 164, 157, 151, 144, 136, 129, 121, 113, 104,  96,  88,  79,  71,  63,
     55,  48,  41,  34,  28,  22,  17,  13,   9,   5,   3,   1,   0,   0,   0,   1,
      3,   5,   8,  12,  17,  22,  27,  33,  40,  47,  54,  62,  70,  78,  87,  95,
    103, 112, 120, 128, 136, 143, 150, 157, 163, 169, 174, 178, 182, 186, 188, 190,
    191, 191, 191, 190, 188, 186, 183, 179, 175, 169, 164, 158, 151, 144, 137, 129,
    121, 113, 105,  96,  88,  80,  71,  63,  56,  48,  41,  34,  28,  22,  17,  13,
      9,   6,   3,   1,   0,   0,   0,   1,   3,   5,   8,  12,  16,  21,  27,  33,
     40,  47,  54,  62,  70,  78,  86,  95, 103, 111, 120, 128, 135, 143, 150, 157,
    163, 168, 174, 178, 182, 185, 188, 190, 191, 191, 191, 190, 188, 186, 183, 179,
    175, 170, 164, 158, 151, 144, 137, 129, 121, 113, 105,  96,  88,  80,  72,  64,
     56,  48,  41,  35,  28,  23,  17,  13,   9,   6,   3,   1,   0,   0,   0,   1,
      2,   5,   8,  12,  16,  21,  27,  33,  40,  47,  54,  62,  70,  78,  86,  94,
    103, 111, 119, 127, 135, 142, 150, 156, 163, 168, 173, 178, 182, 185, 188, 190,
  },
  {
      0,   0,   1,   3,   5,   7,  10,  13,  17,  20,  25,  29,  34,  39,  44,  50,
     55,  61,  67,  73,  80,  86,  92,  98, 104, 111, 117, 122, 128, 134, 139, 144,
    149, 153, 158, 161, 165, 168, 171, 173, 175, 177, 178, 178, 179, 179, 178, 177,
    175, 173, 171, 168, 165, 162, 158, 154, 149, 145, 139, 134, 129, 123, 117, 111,
    105,  99,  93,  86,  80,  74,  68,  62,  56,  50,  45,  39,  34,  29,  25,  21,
     17,  13,  10,   7,   5,   3,   2,   0,   0,   0,   0,   0,   1,   3,   5,   7,
     10,  13,  16,  20,  24,  29,  34,  39,  44,  49,  55,  61,  67,  73,  79,  85,
     92,  98, 104, 110, 116, 122, 128, 133, 139, 144, 149, 153, 157, 161, 165, 168,
    171, 173, 175, 177, 178, 178, 179, 179, 178, 177, 175, 173, 171, 168, 165, 162,
    158, 154, 150, 145, 140, 134, 129, 123, 117, 111, 105,  99,  93,  87,  80,  74,
     68,  62,  56,  50,  45,  40,  34,  30,  25,  21,  17,  13,  10,   8,   5,   3,
      2,   0,   0,   0,   0,   0,   1,   3,   5,   7,  10,  13,  16,  20,  24,  29,
     33,  38,  44,  49,  55,  61,  67,  73,  79,  85,  91,  98, 104, 110, 116, 122,
    128, 133, 139, 144, 148, 153, 157, 161, 165, 168, 171, 173, 175, 177, 178, 178,
    179, 179, 178, 177, 175, 174, 171, 169, 166, 162, 158, 154, 150, 145, 140, 135,
    129, 124, 118, 112, 106,  99,  93,  87,  81,  74,  68,  62,  56,  51,  45,  40,
  },
  {
     15,  21,  27,  34,  41,  49,  57,  65,  74,  83,  91, 100, 108, 116, 124, 131,
    138, 144, 150, 155, 159, 162, 164, 165, 166, 165, 164, 162, 159, 155, 150, 145,
    139, 132, 125, 117, 109, 100,  92,  83,  74,  66,  57,  49,  41,  34,  27,  21,
     16,  11,   7,   4,   1,   0,   0,   0,   1,   3,   7,  10,  15,  21,  27,  33,
     41,  48,  57,  65,  74,  82,  91, 100, 108, 116, 124, 131, 138, 144, 150, 154,
    158, 162, 164, 165, 166, 165, 164, 162, 159, 155, 150, 145, 139, 132, 125, 117,
    109, 101,  92,  83,  75,  66,  58,  49,  42,  34,  27,  21,  16,  11,   7,   4,
      1,   0,   0,   0,   1,   3,   6,  10,  15,  20,  27,  33,  40,  48,  56,  65,
     73,  82,  91,  99, 108, 116, 124, 131, 138, 144, 150, 154, 158, 162, 164, 165,
    166, 166, 164, 162, 159, 155, 150, 145, 139, 132, 125, 117, 109, 101,  92,  84,
     75,  66,  58,  50,  42,  35,  28,  21,  16,  11,   7,   4,   2,   0,   0,   0,
      1,   3,   6,  10,  15,  20,  26,  33,  40,  48,  56,  64,  73,  82,  90,  99,
    107, 116, 123, 131, 138, 144, 149, 154, 158, 161, 164, 165, 166, 166, 164, 162,
    159, 155, 151, 145, 139, 132, 125, 118, 109, 101,  93,  84,  75,  67,  58,  50,
     42,  35,  28,  22,  16,  11,   7,   4,   2,   0,   0,   0,   1,   3,   6,  10,
     15,  20,  26,  33,  40,  48,  56,  64,  73,  81,  90,  99, 107, 115, 123, 131,
  },
};

// Which harmonic each of partials 2 to 8 plays, as a multiple of the
// fundamental, for the 256 positions of the spectrum control.
const uint8_t HARMONIC_TABLE[7][256] = {
  {
      4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   3,   3,   3,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   3,   3,   3,   3,   3,
      3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,
      3,   3,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
      4,   4,   4,   4,   4,   3,   3,   3,   3,   3,   3,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,
  },
  {
      5,   5,   5,   5,   4,   4,   4,   4,   4,   4,   4,   3,   3,   3,   3,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,
      4,   4,   4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   5,   5,   6,   6,
      6,   6,   6,   6,   6,   5,   5,   5,   5,   5,   5,   5,   4,   4,   4,   4,
      4,   4,   4,   3,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,   4,   4,   4,   4,
      5,   5,   5,   5,   5,   5,   6,   6,   6,   6,   6,   6,   6,   5,   5,   5,
      5,   5,   5,   5,   4,   4,   4,   4,   4,   4,   4,   3,   3,   3,   3,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,
      4,   4,   4,   4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   5,   6,   6,
      6,   6,   6,   6,   6,   5,   5,   5,   5,   5,   5,   5,   4,   4,   4,   4,
      4,   4,   4,   3,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
  },
  {
      5,   5,   4,   4,   4,   4,   4,   4,   4,   4,   3,   3,   3,   3,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,
      3,   4,   4,   4,   4,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   6,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
      7,   7,   7,   6,   6,   6,   6,   6,   5,   5,   5,   5,   5,   4,   4,   4,
      4,   4,   4,   4,   3,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   4,   4,   4,   4,   4,
      4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   6,   6,   7,   7,   7,   7,
      7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,   6,   6,
      6,   6,   6,   5,   5,   5,   5,   4,   4,   4,   4,   4,   4,   4,   3,   3,
      3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   3,   3,   3,   4,   4,   4,   4,   4,   4,   4,   4,   5,   5,   5,
      5,   6,   6,   6,   6,   6,   7,   7,   7,   7,   7,   7,   7,   7,   7,   7,
  },
  {
      4,   4,   4,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   4,   4,   4,   4,   5,   5,
      6,   6,   6,   7,   7,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
      8,   8,   8,   8,   7,   7,   6,   6,   6,   5,   5,   4,   4,   4,   4,   3,
      3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   3,   3,   4,   4,   4,   4,   5,   5,   6,   6,   6,   7,
      7,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,
      7,   7,   6,   6,   6,   5,   5,   4,   4,   4,   4,   3,   3,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   4,   4,   4,   4,   5,   5,   5,   6,   6,   7,   7,   8,   8,   8,
      8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   8,   7,   7,   6,   6,
      6,   5,   5,   4,   4,   4,   4,   3,   3,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   4,   4,
      4,   4,   4,   5,   5,   6,   6,   7,   7,   8,   8,   8,   8,   8,   8,   8,
      8,   8,   8,   8,   8,   8,   8,   8,   7,   7,   7,   6,   6,   5,   5,   4,
      4,   4,   4,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
  },
  {
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      3,   4,   4,   4,   5,   5,   6,   7,   7,   8,   8,   8,   8,  10,  10,  10,
     10,  10,  10,  10,  10,  10,  10,   8,   8,   8,   8,   7,   6,   6,   5,   4,
      4,   4,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   3,   3,   4,   4,   4,   5,   6,   6,   7,   8,   8,   8,   8,
     10,  10,  10,  10,  10,  10,  10,  10,  10,  10,   8,   8,   8,   8,   7,   7,
      6,   5,   5,   4,   4,   4,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   3,   4,   4,   4,   5,   5,   6,   7,   7,
      8,   8,   8,   8,  10,  10,  10,  10,  10,  10,  10,  10,  10,  10,   8,   8,
      8,   8,   7,   6,   6,   5,   4,   4,   4,   3,   3,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   4,   4,   4,   5,
      6,   6,   7,   8,   8,   8,   8,  10,  10,  10,  10,  10,  10,  10,  10,  10,
     10,   8,   8,   8,   8,   7,   7,   6,   5,   5,   4,   4,   4,   3,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   4,
      4,   4,   5,   5,   6,   7,   7,   8,   8,   8,   8,  10,  10,  10,  10,  10,
     10,  10,  10,  10,   8,   8,   8,   8,   7,   7,   6,   6,   5,   4,   4,   4,
  },
  {
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,
      3,   3,   4,   4,   4,   5,   5,   6,   6,   7,   7,   8,   8,   8,   8,  10,
     10,  12,  12,  12,  12,  14,  14,  14,  14,  14,  14,  14,  14,  14,  12,  12,
     12,  12,  10,  10,   8,   8,   8,   8,   7,   7,   6,   6,   5,   5,   4,   4,
      4,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   4,   4,   4,   5,   5,   6,
      6,   7,   7,   8,   8,   8,   8,  10,  10,  12,  12,  12,  12,  14,  14,  14,
     14,  14,  14,  14,  14,  14,  12,  12,  12,  12,  10,  10,   8,   8,   8,   8,
      7,   7,   6,   6,   5,   5,   4,   4,   4,   3,   3,   3,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   4,   4,   4,   4,   5,   6,   6,   7,   7,   8,   8,   8,   8,  10,
     10,  12,  12,  12,  12,  14,  14,  14,  14,  14,  14,  14,  14,  14,  12,  12,
     12,  12,  10,  10,   8,   8,   8,   8,   7,   7,   6,   6,   5,   5,   4,   4,
      4,   4,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   4,   4,   4,   4,   5,   6,
      6,   7,   7,   8,   8,   8,   8,  10,  10,  12,  12,  12,  12,  14,  14,  14,
  },
  {
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   4,   4,
      4,   4,   5,   5,   6,   6,   7,   7,   8,   8,   8,   8,  10,  10,  10,  12,
     12,  14,  14,  14,  15,  15,  15,  15,  15,  15,  16,  16,  16,  16,  16,  15,
     15,  15,  15,  15,  15,  14,  14,  14,  12,  12,  10,  10,  10,   8,   8,   8,
      8,   7,   7,   6,   6,   5,   5,   4,   4,   4,   4,   3,   3,   3,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   3,   3,   3,   4,   4,   4,   4,   5,   5,   6,   6,
      7,   7,   8,   8,   8,   8,  10,  10,  10,  12,  12,  14,  14,  14,  15,  15,
     15,  15,  15,  15,  16,  16,  16,  16,  16,  16,  15,  15,  15,  15,  15,  14,
     14,  14,  12,  12,  12,  10,  10,   8,   8,   8,   8,   7,   7,   6,   6,   5,
      5,   4,   4,   4,   4,   3,   3,   3,   2,   2,   2,   2,   2,   2,   2,   2,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   3,
      3,   3,   4,   4,   4,   4,   5,   5,   6,   6,   7,   7,   8,   8,   8,   8,
     10,  10,  10,  12,  12,  14,  14,  14,  15,  15,  15,  15,  15,  15,  16,  16,
     16,  16,  16,  16,  15,  15,  15,  15,  15,  14,  14,  14,  12,  12,  12,  10,
     10,   8,   8,   8,   8,   7,   7,   6,   6,   5,   5,   4,   4,   4,   4,   3,
  },
};

float sineTable[1025];
float outputScale = 0.0f;  // set in setup(), so the loudest setting stays below full scale

// Written by loop(), read by the audio callback.
volatile uint32_t increment[PARTIALS];
volatile float targetLevel[PARTIALS];
volatile float smoothing[PARTIALS];  // LEVEL_SMOOTHING or RELEASE_SMOOTHING

// Written by the audio callback. loop() reads the levels to find a quiet
// oscillator for a new harmonic.
uint32_t phase[PARTIALS];
volatile float level[PARTIALS];

// The harmonic each oscillator plays or last played, 0 for none yet.
int oscillatorHarmonic[PARTIALS] = { 0 };

float pitchCounts = 0.0f;  // smoothed A2, flipped so a higher CV is a larger value
float gainPot = 0.0f;      // smoothed POT1, 0 to 1
float spectrumPot = 0.0f;  // smoothed POT2, 0 to 1
int gainIndex = 0;
int spectrumIndex = 0;
int octave = 0;

static inline float sine(uint32_t p) {
  uint32_t i = p >> 22;
  float a = sineTable[i];
  return a + (sineTable[i + 1] - a) * ((p & 0x3FFFFF) * (1.0f / 4194304.0f));
}

int16_t computeSample() {
  float sum = 0.0f;
  for (int i = 0; i < PARTIALS; i++) {
    phase[i] += increment[i];
    float l = level[i];
    l += (targetLevel[i] - l) * smoothing[i];
    level[i] = l;
    sum += l * sine(phase[i]);
  }
  return (int16_t)constrain(sum * 32767.0f, -32767.0f, 32767.0f);
}

void onAudioTransmit() {
  while (audio.availableForWrite()) {
    audio.write(computeSample());
  }
}

// Note-sized jumps are taken straight away, small wobble is smoothed out.
void readPitch(bool snap) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(A2);
  float counts = 4095.0f - sum / 8.0f;
  float diff = counts - pitchCounts;
  float k = fabsf(diff) / (0.5f * COUNTS_PER_SEMITONE);
  if (snap || k > 1.0f) k = 1.0f;
  else if (k < 0.15f) k = 0.15f;
  pitchCounts += diff * k;
}

// 0 to 1, averaged against ADC noise.
float readPot(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(pin);
  return sum / (8 * 4095.0f);
}

// Follows a move of more than a few settings at once and smooths smaller
// changes, which are ADC noise or a slow turn.
float smoothPot(float smoothed, float reading) {
  if (fabsf(reading - smoothed) > POT_JUMP) return reading;
  return smoothed + (reading - smoothed) * POT_SMOOTHING;
}

// Steps a pot through `count` positions. The position only changes once the
// pot is half a step past the edge, so it does not flicker on a boundary.
// With current at -1 it gives the exact position.
int stepOf(float position, int current, int count) {
  float x = position * count;
  if (current >= 0 && x > current - 0.5f && x < current + 1.5f) return current;
  return constrain((int)x, 0, count - 1);
}

bool buttonPressed() {
  static bool last = HIGH, stable = HIGH;
  static uint32_t changedAt = 0;
  bool now = digitalRead(BUTTON_PIN);
  if (now != last) {
    last = now;
    changedAt = millis();
  }
  if (now != stable && millis() - changedAt >= 10) {
    stable = now;
    return stable == LOW;
  }
  return false;
}

void updatePartials() {
  float fundamental = BOTTOM_HZ * exp2f(pitchCounts / 4095.0f * CV_OCTAVES + octave);
  // Every partial steps an exact multiple of the fundamental's increment, so
  // they stay locked to it.
  uint32_t base = (uint32_t)(fundamental * (4294967296.0f / SAMPLE_RATE));

  // The tables put some partials on the same harmonic. Played apart they
  // would add up or cancel out depending on how their phases happen to line
  // up, so each harmonic plays on one oscillator, at the level the partials
  // give it on average: the root of the sum of their squared gains.
  // Harmonics too high to play are left out.
  int harmonic[PARTIALS];
  float power[PARTIALS];
  int harmonics = 0;
  for (int i = 0; i < PARTIALS; i++) {
    int m = i == 0 ? 1 : HARMONIC_TABLE[i - 1][spectrumIndex];
    float g = GAIN_TABLE[i][gainIndex];
    if (g == 0.0f || fundamental * m >= FADE_END_HZ) continue;
    int k = 0;
    while (k < harmonics && harmonic[k] != m) k++;
    if (k == harmonics) {
      harmonic[harmonics] = m;
      power[harmonics++] = 0.0f;
    }
    power[k] += g * g;
  }

  // A harmonic stays on its oscillator, also when it comes back while still
  // fading out, so it never jumps in level or phase. A new one takes the
  // quietest oscillator left. That one is silent unless the knob moves
  // quickly, then the new harmonic starts from the level it still has.
  int oscillatorOf[PARTIALS];
  bool taken[PARTIALS] = { false };
  for (int k = 0; k < harmonics; k++) {
    oscillatorOf[k] = -1;
    for (int o = 0; o < PARTIALS; o++) {
      if (oscillatorHarmonic[o] == harmonic[k]) {
        oscillatorOf[k] = o;
        taken[o] = true;
      }
    }
  }
  for (int k = 0; k < harmonics; k++) {
    if (oscillatorOf[k] >= 0) continue;
    int o = -1;
    for (int u = 0; u < PARTIALS; u++) {
      if (!taken[u] && (o < 0 || level[u] < level[o])) o = u;
    }
    oscillatorOf[k] = o;
    taken[o] = true;
    oscillatorHarmonic[o] = harmonic[k];
  }

  uint32_t inc[PARTIALS];
  float target[PARTIALS];
  float rate[PARTIALS];
  for (int k = 0; k < harmonics; k++) {
    int o = oscillatorOf[k];
    float fade = constrain((FADE_END_HZ - fundamental * harmonic[k]) / (FADE_END_HZ - FADE_START_HZ), 0.0f, 1.0f);
    inc[o] = base * harmonic[k];
    target[o] = sqrtf(power[k]) * outputScale * fade;
    rate[o] = LEVEL_SMOOTHING;
  }
  // The other oscillators fade out on the harmonic they had and follow the
  // pitch on it. No other oscillator plays that harmonic, so they never meet
  // one. Those the pitch pushes past the top stay where they are, unless a
  // harmonic comes close.
  for (int o = 0; o < PARTIALS; o++) {
    if (taken[o]) continue;
    int h = oscillatorHarmonic[o];
    target[o] = 0.0f;
    if (fundamental * h < FADE_END_HZ) {
      inc[o] = base * h;
      rate[o] = LEVEL_SMOOTHING;
      continue;
    }
    inc[o] = increment[o];
    rate[o] = RELEASE_SMOOTHING;
    for (int k = 0; k < harmonics; k++) {
      uint32_t other = inc[oscillatorOf[k]];
      uint32_t gap = other > inc[o] ? other - inc[o] : inc[o] - other;
      if (gap < inc[o] / 16) inc[o] = PARKED_INCREMENT;
    }
  }
  // All in one go, so the audio callback never plays half of a change.
  noInterrupts();
  for (int o = 0; o < PARTIALS; o++) {
    increment[o] = inc[o];
    targetLevel[o] = target[o];
    smoothing[o] = rate[o];
  }
  interrupts();
}

void showOctave() {
  led.setPixelColor(0, OCTAVE_COLOURS[octave]);
  led.show();
}

void setup() {
  analogReadResolution(12);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  for (int i = 0; i <= 1024; i++) sineTable[i] = sinf(2.0f * (float)PI * i / 1024.0f);

  // The oscillators could all peak together. Their levels add up to at most
  // the sum of the gains, so scale by the loudest sum.
  int loudest = 1;
  for (int g = 0; g < 256; g++) {
    int sum = 0;
    for (int i = 0; i < PARTIALS; i++) sum += GAIN_TABLE[i][g];
    loudest = max(loudest, sum);
  }
  outputScale = 0.95f / loudest;

  readPitch(true);
  gainPot = readPot(A0);
  spectrumPot = readPot(A1);
  gainIndex = stepOf(gainPot, -1, 256);
  spectrumIndex = stepOf(spectrumPot, -1, 256);
  updatePartials();

  led.begin();
  led.setBrightness(80);
  showOctave();

  audio.setBuffers(4, 32);
  audio.onTransmit(onAudioTransmit);
  audio.begin((uint32_t)SAMPLE_RATE);
}

void loop() {
  static uint32_t lastRead = 0;
  uint32_t now = micros();
  if (now - lastRead < 1000) return;
  lastRead = now;

  readPitch(false);
  gainPot = smoothPot(gainPot, readPot(A0));
  spectrumPot = smoothPot(spectrumPot, readPot(A1));
  gainIndex = stepOf(gainPot, gainIndex, 256);
  spectrumIndex = stepOf(spectrumPot, spectrumIndex, 256);
  if (buttonPressed()) {
    octave = (octave + 1) % 4;
    showOctave();
  }
  updatePartials();
}
