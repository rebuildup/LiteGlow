#pragma once

#ifndef LITEGLOW_FFT_H
#define LITEGLOW_FFT_H

#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"

// FFT-based blur for large kernel sizes
void FFTBasedBlur(
    PF_EffectWorld* input,
    PF_EffectWorld* output,
    float radius,
    float strength);

// Multi-resolution pyramid blur for medium kernel sizes
void PyramidBlur(
    PF_EffectWorld* input,
    PF_EffectWorld* output,
    float radius,
    int quality);

// Selects the best algorithm based on radius and available hardware
void OptimizedBlur(
    PF_InData* in_data,
    PF_EffectWorld* input,
    PF_EffectWorld* output,
    float radius,
    int quality,
    bool useGPU);

#endif // LITEGLOW_FFT_H