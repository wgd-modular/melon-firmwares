#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

class PLONKEngine {
public:
    float bp[6], lp[6];
    float pIdx;
    float lpfPluck;
    float ksBuf[1024]; 
    float modeGf[6], modeGq[6], modeAmp[6];
    
    PLONKEngine() { reset(); }
    
    void reset() {
        for(int i=0; i<6; i++) { bp[i] = 0; lp[i] = 0; modeGf[i]=0; modeGq[i]=0; modeAmp[i]=0; }
        pIdx = 0; lpfPluck = 0;
        for(int i=0; i<1024; i++) ksBuf[i] = 0;
    }
    
    float renderSample(float env, float P[12], float invFs, float randomValue) {
        float envE = env * env;
        float baseFreq = 25.0f + (P[0] * P[0] * P[0]) * 1200.0f;
        
        float pitchEnv = m_fast_powf(env, 12.0f) * P[7] * 4.0f;
        float f = baseFreq * (1.0f + pitchEnv);

        // Parameters (Rearranged for Poti 3 optimization)
        // P1: Position (CVable Expressive)
        // P5: Structure (CVable Expressive)
        // P10: Damping (CVable Expressive)
        
        float qBase = 100.0f + m_fast_powf(P[4], 3.0f) * 15000.0f; // P4: Resonance
        float brightness = P[2]; // P2: Brightness/Spectral Slope (moved from P1)
        float structure = P[5];
        float spread = 1.0f + P[9] * 0.5f; // P9: Detune Partials
        float position = P[1]; // P1: Positioning (Expressive CV)

        for(int m=0; m<6; m++) {
            float partialFreq = f * (m + 1) * (1.0f + m * structure * spread);
            if(partialFreq > 18000.0f) partialFreq = 18000.0f;
            
            modeGf[m] = 2.0f * sinf(M_PI * partialFreq * invFs);
            if(modeGf[m] > 1.4f) modeGf[m] = 1.4f;
            
            float q = qBase / (1.0f + m * (1.0f - P[10]) * 5.0f); // P10: Damping (Expressive CV)
            modeGq[m] = 1.0f / q;
            
            modeAmp[m] = cosf(M_PI * position * (float)(m + 1)) * m_fast_powf(brightness, (float)m * 0.5f);
        }

        // --- EXCITER ---
        float pulse = (env > 0.999f) ? 1.0f : 0.0f;
        float exciter = (pulse * (1.0f - P[3]) + randomValue * P[3] * 0.5f) * (0.2f + P[6] * 0.8f);

        int dLen = 1024;
        int writeIdx = (int)pIdx;
        int readIdx = (writeIdx - 50 + dLen) % dLen;
        lpfPluck += (0.1f + P[8]*0.4f) * (ksBuf[readIdx] - lpfPluck); // P8: Pluck Hardness
        float outPluck = exciter + lpfPluck * 0.85f;
        ksBuf[writeIdx] = fb_limit(outPluck);
        pIdx = (float)((writeIdx + 1) % dLen);

        // --- RESONATOR BANK ---
        float total = 0.0f;
        for(int m=0; m<6; m++) {
            float nEx = outPluck - (bp[m] * modeGq[m]) - lp[m];
            bp[m] += nEx * modeGf[m];
            lp[m] += bp[m] * modeGf[m];
            if(bp[m] > 5.0f) bp[m] = 5.0f; if(bp[m] < -5.0f) bp[m] = -5.0f;
            total += bp[m] * modeAmp[m];
        }

        float out = total * 2.5f;
        if(P[11] > 0.1f) out = tanhf(out * (1.0f + P[11] * 5.0f)); // P11: Saturation

        return out * envE;
    }

private:
    float fb_limit(float x) { return (x > 1.0f) ? 1.0f : ((x < -1.0f) ? -1.0f : x); }
};
