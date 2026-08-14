#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*
 * SuperBIA Synthesis Engine
 * Portable 12-parameter core for RP2350 and PC Sandbox.
 */
class METALEngine {
public:
    float phArr[6];
    
    METALEngine() {
        reset();
    }
    
    void reset() {
        for(int i=0; i<6; i++) phArr[i] = 0.0f;
    }
    
    /**
     * Renders a single sample of the Super-BIA engine.
     * 
     * @param env         Current envelope value (1.0 down to 0.0)
     * @param P           Array of 12 synthesis parameters (0.0 to 1.0)
     * @param invFs       Inverse sample rate (1.0 / SampleRate)
     * @param randomValue Current random noise value (-1.0 to 1.0)
     * @return            Rendered sample in range [-1.0, 1.0]
     */
    float renderSample(float env, float P[12], float invFs, float randomValue) {
        float envE = env * env;
        
        // --- 1. Pitch & Pitch Envelope (P0 & P7) ---
        float baseFreq = 25.0f + (P[0] * P[0] * P[0]) * 1500.0f; // P0: Pitch
        
        // P7: Punch / Pitch Envelope
        // Higher P7 = higher start pitch AND slower decay (Glissando effect)
        float punchAmount = P[7] * P[7] * 800.0f;
        float punchDecay = 4.0f + (1.0f - P[7]) * 60.0f; // Lower exponent = longer slide
        baseFreq += punchAmount * m_fast_powf(env, punchDecay);

        // --- 2. Parameters ---
        float morph = P[1];        // P1: Morph (Sine to Square)
        float harmonics = P[2];    // P2: Spectral Damping
        float spread = P[3];       // P3: Harmonic Spacing
        float metal = P[4] * P[4] * 6.0f;      // P4: PM Index
        float fold = 1.0f + P[5] * P[5] * 20.0f; // P5: Folder Drive
        
        // P6: Strike Level (Hard Transient)
        // A very short, aggressive noise + pitch burst
        float strike = 0.0f;
        if (env > 0.992f) { // Only first few samples
            strike = randomValue * P[6] * 0.7f;
        }
        
        // P8: Dirt / Drive (Saturation stage)
        float drive = 1.0f + P[8] * 4.0f;

        // --- 3. Harmonic Ratios (Spread P3) ---
        float f[6];
        f[0] = baseFreq;
        float ratios[5] = {1.501f, 1.998f, 3.001f, 4.137f, 7.82f};
        for(int m=1; m<6; m++) {
            float r = (float)(m+1) * (1.0f - spread) + ratios[m-1] * spread;
            f[m] = baseFreq * r;
            if(f[m] > 18000.0f) f[m] = 18000.0f; // Soft cap
        }

        // --- 4. 6-Operator PM Cascade ---
        float fmIn = 0.0f;
        float finalSum = 0.0f;
        for(int m=5; m>=0; m--) {
            // Harmonics control (P2)
            float sc = 1.0f - (float)m * 0.15f * (1.0f - harmonics);
            if(sc < 0.1f) sc = 0.1f;
            float hDecay = 3.0f + (1.0f - sc) * 12.0f;
            float hEnv = powf(env, hDecay);
            
            phArr[m] += f[m] * invFs;
            if(phArr[m] >= 1.0f) phArr[m] -= 1.0f;
            
            float phaseshift = phArr[m] + fmIn * metal * hEnv;
            float s = sinf(phaseshift * 6.283185307f);
            
            // Sine to Square Morph (P1)
            float sq = (s > 0.0f) ? 1.0f : -1.0f;
            float morphed = s * (1.0f - morph) + sq * morph;
            
            // Feedback (P10)
            fmIn = morphed + (fmIn * P[10] * 0.45f);
            finalSum += morphed * hEnv * 0.35f; // Increased fundamental weight
        }

        // --- 5. Noise & Drive ---
        float noise = randomValue * m_fast_powf(env, 25.0f) * P[9] * 0.8f;
        float preMix = (finalSum + noise + strike) * drive;

        // --- 6. Folding, Sub & Final Clipping ---
        // P5: Overdriven Folder
        float folded = sinf(preMix * fold * (float)M_PI);
        
        // P11: Dirty Sub (Sine with a bit of saturation)
        float subVal = sinf(phArr[0] * (float)M_PI);
        float subSat = tanhf(subVal * 2.0f) * 0.6f; 
        float sub = subSat * envE * P[11]; 

        // Final Glue (Saturator stage)
        float sOut = (folded + sub) * env; // Changed to linear env for a more natural decay body
        sOut = tanhf(sOut * 1.5f) * 1.2f; // Evil Saturation for broad spectrum

        if(sOut > 1.0f) sOut = 1.0f;
        if(sOut < -1.0f) sOut = -1.0f;
        
        return sOut;
    }
};
