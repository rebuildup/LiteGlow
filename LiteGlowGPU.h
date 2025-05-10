#pragma once

#ifndef LITEGLOW_GPU_H
#define LITEGLOW_GPU_H

#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "LiteGlow.h"

// GPU Processing Status Enum
enum {
    GPU_PROCESSING_UNAVAILABLE = 0,
    GPU_PROCESSING_AVAILABLE = 1,
    GPU_PROCESSING_ACTIVE = 2
};

// GPU Processing Context
typedef struct LiteGlowGPUContext {
    A_long      gpu_status;      // GPU processing status
    void* gpu_context;     // GPU context (CUDA/OpenCL/Metal)
    void* gpu_program;     // GPU program/shader
    A_Boolean   initialized;     // Flag to check if GPU is initialized
} LiteGlowGPUContext;

// GPU Handler Functions
PF_Err LiteGlow_InitGPU(struct LiteGlowGPUContext* gpu_contextP);
PF_Err LiteGlow_ReleaseGPU(struct LiteGlowGPUContext* gpu_contextP);
PF_Err LiteGlow_ProcessFrameGPU(PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    struct LiteGlowGPUContext* gpu_contextP);
A_Boolean LiteGlow_IsGPUSupported(void);

#endif // LITEGLOW_GPU_H