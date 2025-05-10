/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2025 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/*******************************************************************/

#pragma once

#ifndef LITEGLOW_GPU_IMPL_H
#define LITEGLOW_GPU_IMPL_H

#include "AE_Effect.h"
#include "AE_EffectCB.h"

// Main GPU rendering function
PF_Err
LiteGlowGPU_Render(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_EffectWorld* input_worldP,
    PF_EffectWorld* output_worldP,
    float strength,
    float radius,
    float threshold,
    int quality,
    float blend_ratio);

#endif // LITEGLOW_GPU_IMPL_H