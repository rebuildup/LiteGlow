#include "LiteGlowGPU.h"
#include "LiteGlowGPU_Impl.h"
#include "AEGP_SuiteHandler.h"
#include <string.h>

// Initialize GPU Processing
PF_Err LiteGlow_InitGPU(struct LiteGlowGPUContext* gpu_contextP)
{
    PF_Err err = PF_Err_NONE;

    // Check for valid pointer
    if (!gpu_contextP) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Only initialize once
    if (!gpu_contextP->initialized) {
        // Always start with unavailable status
        gpu_contextP->gpu_status = GPU_PROCESSING_UNAVAILABLE;
        gpu_contextP->gpu_context = NULL;
        gpu_contextP->gpu_program = NULL;

        // Check if GPU processing is supported
        if (LiteGlow_IsGPUSupported()) {
            // Initialize platform-specific GPU implementation
            err = InitGPUImpl(gpu_contextP);
            if (!err) {
                gpu_contextP->initialized = TRUE;
                gpu_contextP->gpu_status = GPU_PROCESSING_AVAILABLE;
            }
        }
    }

    return err;
}

// Release GPU Resources
PF_Err LiteGlow_ReleaseGPU(struct LiteGlowGPUContext* gpu_contextP)
{
    PF_Err err = PF_Err_NONE;

    // Check for valid pointer
    if (!gpu_contextP) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    if (gpu_contextP->initialized) {
        // Release platform-specific GPU resources
        err = ReleaseGPUImpl(gpu_contextP);
        gpu_contextP->initialized = FALSE;
        gpu_contextP->gpu_status = GPU_PROCESSING_UNAVAILABLE;
        gpu_contextP->gpu_context = NULL;
        gpu_contextP->gpu_program = NULL;
    }

    return err;
}

// Process Frame with GPU
PF_Err LiteGlow_ProcessFrameGPU(PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    struct LiteGlowGPUContext* gpu_contextP)
{
    PF_Err err = PF_Err_NONE;

    // Check if GPU is available and initialized
    if (gpu_contextP->gpu_status == GPU_PROCESSING_AVAILABLE &&
        gpu_contextP->initialized) {

        // Set status to active during processing
        gpu_contextP->gpu_status = GPU_PROCESSING_ACTIVE;

        // Call platform-specific implementation
        err = ProcessFrameGPUImpl(in_data, out_data, params, output, gpu_contextP);

        // Reset status after processing
        gpu_contextP->gpu_status = GPU_PROCESSING_AVAILABLE;

    }
    else {
        // Not available, return error to fall back to CPU
        err = PF_Err_BAD_CALLBACK_PARAM;
    }

    return err;
}

// Check if GPU processing is supported
A_Boolean LiteGlow_IsGPUSupported(void)
{
    // Stub implementation - always return false until we can properly link OpenCV
    return FALSE;
}