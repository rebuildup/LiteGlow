#pragma once

#ifndef LITEGLOW_GPU_IMPL_H
#define LITEGLOW_GPU_IMPL_H

#include "LiteGlowGPU.h"

// Simple implementation data
typedef struct {
    A_Boolean is_gpu_available;
} GPUImplData, * GPUImplDataP;

// Implementation-specific functions
PF_Err InitGPUImpl(struct LiteGlowGPUContext* gpu_contextP);
PF_Err ReleaseGPUImpl(struct LiteGlowGPUContext* gpu_contextP);
PF_Err ProcessFrameGPUImpl(PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    struct LiteGlowGPUContext* gpu_contextP);

#endif // LITEGLOW_GPU_IMPL_H