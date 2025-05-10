#include "LiteGlowGPU_Impl.h"
#include "AEGP_SuiteHandler.h"
#include <string.h>
PF_Err InitGPUImpl(struct LiteGlowGPUContext* gpu_contextP)
{
    PF_Err err = PF_Err_NONE;

    // Validation
    if (!gpu_contextP) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // In this stub implementation, we do nothing and pretend GPU isn't available
    // No memory allocation, no external resources

    return PF_Err_BAD_CALLBACK_PARAM; // Always fail to ensure we use CPU
}

PF_Err ReleaseGPUImpl(struct LiteGlowGPUContext* gpu_contextP)
{
    // Validation
    if (!gpu_contextP) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Nothing to clean up in our stub implementation

    return PF_Err_NONE;
}

PF_Err ProcessFrameGPUImpl(PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    struct LiteGlowGPUContext* gpu_contextP)
{
    // Stub implementation - always fail to trigger CPU fallback
    return PF_Err_BAD_CALLBACK_PARAM;
}