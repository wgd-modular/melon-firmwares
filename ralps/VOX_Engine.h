#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

class VOXEngine {
public:
    float bp[3], lp[3];
    uint32_t phPulse_u;
    uint32_t phLFO_u;
    uint32_t phVib_u;
    float lfoPhase;
    float fmtShift;
    float pitchDrift;

    VOXEngine() { reset(); }

    void reset() {
        for(int i=0; i<3; i++) { bp[i] = 0; lp[i] = 0; }
        phPulse_u = 0; phLFO_u = 0; phVib_u = 0;
        lfoPhase = (float)rand()/(float)RAND_MAX;
        fmtShift = 1.0f + ((float)rand()/(float)RAND_MAX * 2.0f - 1.0f) * 0.06f;
        pitchDrift = 1.0f + ((float)rand()/(float)RAND_MAX * 2.0f - 1.0f) * 0.01f;
    }

    // Helper for fast sine from integer phase
    inline float sin_u32(uint32_t ph) {
        float x = (float)ph * 2.3283064365e-10f; // ph / 2^32
        return sinf(x * 2.0f * M_PI);
    }

    float renderSample(float env, float P[12], float invFs, float randomValue) {
        float envE = env * env;
        
        // 1. Pitch & Vibrato (P10: Vib Depth)
        float vibHz = 5.0f + (P[3] * 2.0f); // Slight link to LFO rate
        phVib_u += (uint32_t)(vibHz * invFs * 4294967296.0f);
        float vib = sin_u32(phVib_u) * P[10] * 0.05f;
        
        float baseFreq = 25.0f + (P[0] * P[0] * P[0]) * 800.0f;
        float pitchEnv = envE * env * P[4] * 2.0f; // P4: Inflection
        float f = baseFreq * (1.0f + pitchEnv + vib) * pitchDrift;

        // 2. LFO (Vowel Morphing LFO) (P3: Rate, P5: Depth)
        float lfoHz = 0.5f + (P[3] * P[3] * P[3]) * 19.5f;
        phLFO_u += (uint32_t)(lfoHz * invFs * 4294967296.0f);
        float lfoVal = sin_u32(phLFO_u + (uint32_t)(lfoPhase * 4294967296.0f));

        // 3. Vowel Matrix (A, E, I, O, U)
        float vIdx = P[1] * 4.0f + lfoVal * P[5] * 2.0f;
        if(vIdx < 0) vIdx = 0; if(vIdx > 3.99f) vIdx = 3.99f;
        int v0 = (int)vIdx; int v1 = v0 + 1;
        float vFrac = vIdx - (float)v0;

        // Formant data: {F1, F2, F3}
        float vf[5][3] = {
            {730, 1090, 2440}, // A
            {530, 1840, 2480}, // E
            {270, 2290, 3010}, // I
            {400, 840, 2800},  // O
            {300, 870, 2240}   // U
        };

        float shift = (0.4f + P[2] * 2.0f) * fmtShift; // P2: Formant Shift

        // 4. Source Oscillator (Buzz / Sibilance / Pop)
        phPulse_u += (uint32_t)(f * invFs * 4294967296.0f);
        
        // P9: Harmonic Buzz (Saw vs Pulse morph)
        float phase = (float)phPulse_u * 2.3283064365e-10f;
        float saw = (phase * 2.0f - 1.0f);
        float pulse = (phase < 0.1f) ? 1.0f : -0.1f;
        float buzz = saw * (1.0f - P[9]) + pulse * P[9];

        // P6: Consonant / Strike Pop
        float pop = (env > 0.99f) ? (randomValue * P[6] * 0.8f) : 0.0f;
        // P7: Sibilance (Breathiness)
        float breath = randomValue * P[7] * 0.15f * env;

        float exciter = buzz * 0.5f + pop + breath;

        // 5. Parallel Formant Filters
        float total = 0.0f;
        float qFactor = 1.0f / (12.0f + P[8] * 40.0f); // P8: Width / Q

        for(int m=0; m<3; m++) {
            float Fm = (vf[v0][m] * (1.0f - vFrac) + vf[v1][m] * vFrac) * shift;
            if(Fm > 18000.0f) Fm = 18000.0f;
            float gf = 2.0f * sinf(M_PI * Fm * invFs);
            if(gf > 1.4f) gf = 1.4f;

            float nEx = exciter - (bp[m] * qFactor) - lp[m];
            bp[m] += nEx * gf;
            lp[m] += bp[m] * gf;
            
            // Limit for stability
            if(bp[m] > 4.0f) bp[m] = 4.0f; if(bp[m] < -4.0f) bp[m] = -4.0f;
            total += bp[m];
        }

        // 6. Post (P11: Saturation)
        float out = total * 2.0f;
        if(P[11] > 0.1f) {
            float drive = 1.0f + P[11] * 8.0f;
            out = tanhf(out * drive);
        }

        return out * envE;
    }
};
