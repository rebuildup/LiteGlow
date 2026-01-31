/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2023 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

/*	LiteGlow.cpp

	High-performance glow effect plugin for Adobe After Effects.
	Supports CPU (8/16/32-bit) and GPU (DirectX 12) rendering paths.
	Uses multi-pass box blur for Gaussian approximation with quality-based
	downsampling for performance optimization.

	Revision History

	Version		Change													Engineer	Date
	=======		======													========	======
	1.0.0		Initial release												361do		2025-01-XX

*/

#include "LiteGlow.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "AE_EffectPixelFormat.h"
#include "AE_EffectGPUSuites.h"

#include <math.h>
#include <cstdlib>
#include <algorithm>

// DirectX support for Windows
#if defined(_WIN32)
    #define HAS_HLSL 1
    #include "DirectXUtils.h"
#else
    #define HAS_HLSL 0
#endif

#if HAS_HLSL
inline PF_Err DXErr(bool inSuccess) {
}

// Better error mapping function for HRESULT
inline PF_Err DXErrFromHRESULT(HRESULT hr) {
    if (SUCCEEDED(hr)) return PF_Err_NONE;
    switch (hr) {
        case E_OUTOFMEMORY:
        case HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY):
            return PF_Err_OUT_OF_MEMORY;
        case E_INVALIDARG:
            return PF_Err_BAD_PARAM;
        case HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED):
            return PF_Err_UNRECOGNIZED_PARAM_TYPE;
        default:
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
#define DX_ERR(FUNC) ERR(DXErr(FUNC))
#endif

// =============================================================================
// Naming Convention
// =============================================================================
// - PascalCase for functions and types
// - camelCase for local variables
// - UPPER_SNAKE_CASE for constants and macros
// - Prefix notation: m for member, g for global, p for pointer (hungarian-adjacent)
// - Suffix notation: 8/16/F for bit-depth specific variants (e.g., BlurH8, BlurV16, BlurHF)

// =============================================================================
// Named Constants for Magic Numbers
// =============================================================================
constexpr float BRIGHT_PASS_KNEE_DEFAULT = 0.1f;         // Soft knee range for threshold transition
constexpr float BRIGHT_PASS_INTENSITY_DEFAULT = 1.5f;    // Intensity multiplier for bright pass
constexpr float SCREEN_BLEND_STRENGTH_MULTIPLIER = 2.0f; // Strength multiplier for screen blend (compensates for downsampling)

constexpr int THREAD_GROUP_SIZE_X = 16;
constexpr int THREAD_GROUP_SIZE_Y = 16;
constexpr int MAX_ADJUSTED_BLUR_RADIUS = 32;
constexpr A_long BYTES_PER_PIXEL_BGRA128 = 16;
constexpr float COLOR_PARAM_MAX = 65535.0f;

// =============================================================================
// LiteGlow - High-Performance Glow Effect
// Multi-Frame Rendering + 32-bit Float Support
// =============================================================================

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    (void)params;  // Unused
    (void)output;  // Unused
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    out_data->my_version = LITEGLOW_VERSION_VALUE;

    out_data->out_flags =
        PF_OutFlag_PIX_INDEPENDENT |
        PF_OutFlag_DEEP_COLOR_AWARE;

    // SmartRender + Multi-Frame Rendering + 32-bit float + GPU
    out_data->out_flags2 =
        PF_OutFlag2_SUPPORTS_SMART_RENDER |
        PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
        PF_OutFlag2_FLOAT_COLOR_AWARE |
        PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 |
        PF_OutFlag2_WIDE_TIME_INPUT;
    
#if HAS_HLSL
    out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
#endif

    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Strength_Param_Name),
        STRENGTH_MIN, STRENGTH_MAX,
        STRENGTH_MIN, STRENGTH_MAX,
        STRENGTH_DFLT,
        PF_Precision_INTEGER,
        0, 0,
        STRENGTH_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Radius_Param_Name),
        RADIUS_MIN, RADIUS_MAX,
        RADIUS_MIN, RADIUS_MAX,
        RADIUS_DFLT,
        PF_Precision_INTEGER,
        0, 0,
        RADIUS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Threshold_Param_Name),
        THRESHOLD_MIN, THRESHOLD_MAX,
        THRESHOLD_MIN, THRESHOLD_MAX,
        THRESHOLD_DFLT,
        PF_Precision_INTEGER,
        0, 0,
        THRESHOLD_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_Quality_Param_Name),
        QUALITY_NUM_CHOICES,
        QUALITY_DFLT,
        STR(StrID_Quality_Param_Choices),
        QUALITY_DISK_ID);

    // Bloom Intensity: Controls the intensity multiplier for bright pass (0.0 - 4.0)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Bloom_Intensity_Param_Name),
        BLOOM_INTENSITY_MIN, BLOOM_INTENSITY_MAX,
        BLOOM_INTENSITY_MIN, BLOOM_INTENSITY_MAX,
        BLOOM_INTENSITY_DFLT,
        PF_Precision_FIXED,
        0, 0,
        BLOOM_INTENSITY_DISK_ID);

    // Threshold Softness (Knee): Controls softness of threshold transition (0.0 - 1.0)
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Knee_Param_Name),
        KNEE_MIN, KNEE_MAX,
        KNEE_MIN, KNEE_MAX,
        KNEE_DFLT,
        PF_Precision_FIXED,
        0, 0,
        KNEE_DISK_ID);

    // Blend Mode: Screen, Add, Normal
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_Blend_Mode_Param_Name),
        BLEND_MODE_NUM_CHOICES,
        BLEND_MODE_DFLT,
        STR(StrID_Blend_Mode_Param_Choices),
        BLEND_MODE_DISK_ID);

    // Tint Color: Color for the glow
    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(STR(StrID_Tint_Color_Param_Name),
        0xFFFF, 0xFFFF, 0xFFFF,  // Default to white (no tint)
        TINT_COLOR_DISK_ID);

    out_data->num_params = LITEGLOW_NUM_PARAMS;
    return err;
}

// =============================================================================
// Optimized CPU Pixel Processing
// =============================================================================

inline float Luma8(const PF_Pixel8* const p) noexcept {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / 255.0f;
}

inline float Luma16(const PF_Pixel16* const p) noexcept {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / (float)PF_MAX_CHAN16;
}

inline float LumaF(const PF_PixelFloat* const p) noexcept {
    return 0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue;
}

// Soft knee for natural threshold transition
// FIX 1: Added division by zero protection
inline float SoftKnee(const float x, const float threshold, const float knee) noexcept {
    float knee_start = threshold - knee;
    float knee_end = threshold + knee;

    if (x <= knee_start) return 0.0f;
    if (x >= knee_end) return x - threshold;

    float knee_range = knee_end - knee_start;
    if (knee_range <= 0.0001f) return 0.0f;  // Protect against division by zero

    float t = (x - knee_start) / knee_range;
    return t * t * (3.0f - 2.0f * t) * (x - threshold);
}

// Screen blend
inline float ScreenBlend(const float a, const float b) noexcept {
    return 1.0f - (1.0f - a) * (1.0f - b);
}

// Add blend
inline float AddBlend(const float a, const float b) noexcept {
    return MIN(1.0f, a + b);
}

// Normal blend (alpha compositing)
inline float NormalBlend(const float a, const float b, const float alpha) noexcept {
    return a * (1.0f - alpha) + b * alpha;
}

// =============================================================================
// Bright Pass Functions
// =============================================================================

typedef struct {
    float threshold;
    float knee;
    float intensity;
    PF_EffectWorld* src;
    int factor;
} BrightPassInfo;

// FIX 3: Added refcon NULL check at start of all iterate functions
static PF_Err BrightPass8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_Pixel8* srcP = inP;  // Default to input pixel

    // Only use downsampled source if factor is valid and source exists
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_Pixel8*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    // else: srcP remains inP (initialized)

    float l = Luma8(srcP);
    float contribution = SoftKnee(l, bp->threshold, bp->knee);
    
    if (contribution > 0.0f) {
        float scale = bp->intensity * (contribution / MAX(0.001f, l - bp->threshold + contribution));
        outP->red   = (A_u_char)MIN(255.0f, srcP->red   * scale);
        outP->green = (A_u_char)MIN(255.0f, srcP->green * scale);
        outP->blue  = (A_u_char)MIN(255.0f, srcP->blue  * scale);
    } else {
        outP->red = outP->green = outP->blue = 0;
    }
    outP->alpha = srcP->alpha;
    return PF_Err_NONE;
}

static PF_Err BrightPass16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_Pixel16* srcP = inP;  // Default to input pixel

    // Only use downsampled source if factor is valid and source exists
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_Pixel16*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    // else: srcP remains inP (initialized)

    float l = Luma16(srcP);
    float contribution = SoftKnee(l, bp->threshold, bp->knee);
    
    if (contribution > 0.0f) {
        float scale = bp->intensity * (contribution / MAX(0.001f, l - bp->threshold + contribution));
        outP->red   = (A_u_short)MIN((float)PF_MAX_CHAN16, srcP->red   * scale);
        outP->green = (A_u_short)MIN((float)PF_MAX_CHAN16, srcP->green * scale);
        outP->blue  = (A_u_short)MIN((float)PF_MAX_CHAN16, srcP->blue  * scale);
    } else {
        outP->red = outP->green = outP->blue = 0;
    }
    outP->alpha = srcP->alpha;
    return PF_Err_NONE;
}

static PF_Err BrightPassF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_PixelFloat* srcP = inP;  // Default to input pixel

    // Only use downsampled source if factor is valid and source exists
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_PixelFloat*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    // else: srcP remains inP (initialized)

    float l = LumaF(srcP);
    float contribution = SoftKnee(l, bp->threshold, bp->knee);
    
    if (contribution > 0.0f) {
        float scale = bp->intensity * (contribution / MAX(0.001f, l - bp->threshold + contribution));
        outP->red   = srcP->red   * scale;
        outP->green = srcP->green * scale;
        outP->blue  = srcP->blue  * scale;
    } else {
        outP->red = outP->green = outP->blue = 0.0f;
    }
    outP->alpha = srcP->alpha;
    return PF_Err_NONE;
}

// =============================================================================
// Blur Functions (Optimized Box Blur for Gaussian Approximation)
// =============================================================================

typedef struct {
    PF_EffectWorld* src;
    int radius_h;  // Horizontal blur radius (may differ from vertical for PAR/field rendering)
    int radius_v;  // Vertical blur radius
} BlurInfo;

// FIX 3 & 4: Added refcon NULL check and count > 0 division protection
static PF_Err BlurH8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_h;  // Use horizontal radius
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    PF_Pixel8* row = (PF_Pixel8*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel8* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = (A_u_char)((rsum + count/2) / count);
        outP->green = (A_u_char)((gsum + count/2) / count);
        outP->blue = (A_u_char)((bsum + count/2) / count);
        outP->alpha = (A_u_char)((asum + count/2) / count);
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

static PF_Err BlurH16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_h;  // Use horizontal radius
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    PF_Pixel16* row = (PF_Pixel16*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel16* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // Proper rounding to avoid bias from integer division
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = (A_u_short)((rsum + count/2) / count);
        outP->green = (A_u_short)((gsum + count/2) / count);
        outP->blue = (A_u_short)((bsum + count/2) / count);
        outP->alpha = (A_u_short)((asum + count/2) / count);
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

static PF_Err BlurHF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_h;  // Use horizontal radius
    float rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    PF_PixelFloat* row = (PF_PixelFloat*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_PixelFloat* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = rsum / count;
        outP->green = gsum / count;
        outP->blue = bsum / count;
        outP->alpha = asum / count;
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

static PF_Err BlurV8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_v;  // Use vertical radius
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel8* p = (PF_Pixel8*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // Proper rounding to avoid bias from integer division
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = (A_u_char)((rsum + count/2) / count);
        outP->green = (A_u_char)((gsum + count/2) / count);
        outP->blue = (A_u_char)((bsum + count/2) / count);
        outP->alpha = (A_u_char)((asum + count/2) / count);
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

static PF_Err BlurV16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_v;  // Use vertical radius
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel16* p = (PF_Pixel16*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // Proper rounding to avoid bias from integer division
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = (A_u_short)((rsum + count/2) / count);
        outP->green = (A_u_short)((gsum + count/2) / count);
        outP->blue = (A_u_short)((bsum + count/2) / count);
        outP->alpha = (A_u_short)((asum + count/2) / count);
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

static PF_Err BlurVF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius_v;  // Use vertical radius
    float rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;

    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_PixelFloat* p = (PF_PixelFloat*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    // FIX 4: Protect against division by zero
    if (count > 0) {
        outP->red = rsum / count;
        outP->green = gsum / count;
        outP->blue = bsum / count;
        outP->alpha = asum / count;
    } else {
        *outP = *inP;  // Fallback to input pixel
    }
    return PF_Err_NONE;
}

// =============================================================================
// Screen Blend Functions
// =============================================================================

typedef struct {
    PF_EffectWorld* glow;
    float strength;
    int factor;
    int blendMode;
    float tintR;
    float tintG;
    float tintB;
} BlendInfo;

static PF_Err BlendScreen8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    if (!refcon) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    if (bi->factor <= 0) bi->factor = 1;  // ADD THIS SAFETY CHECK

    int gx = MIN(bi->glow->width - 1, x / bi->factor);
    int gy = MIN(bi->glow->height - 1, y / bi->factor);
    PF_Pixel8* g = (PF_Pixel8*)((char*)bi->glow->data + gy * bi->glow->rowbytes) + gx;

    float s = bi->strength;
    float ir = inP->red / 255.0f;
    float ig = inP->green / 255.0f;
    float ib = inP->blue / 255.0f;

    // Apply tint color to glow
    float gr = g->red / 255.0f * s * bi->tintR;
    float gg = g->green / 255.0f * s * bi->tintG;
    float gb = g->blue / 255.0f * s * bi->tintB;

    outP->red   = (A_u_char)(ScreenBlend(ir, gr) * 255.0f);
    outP->green = (A_u_char)(ScreenBlend(ig, gg) * 255.0f);
    outP->blue  = (A_u_char)(ScreenBlend(ib, gb) * 255.0f);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlendScreen16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    
    int gx = MIN(bi->glow->width - 1, x / bi->factor);
    int gy = MIN(bi->glow->height - 1, y / bi->factor);
    PF_Pixel16* g = (PF_Pixel16*)((char*)bi->glow->data + gy * bi->glow->rowbytes) + gx;
    
    float s = bi->strength;
    float ir = inP->red / (float)PF_MAX_CHAN16;
    float ig = inP->green / (float)PF_MAX_CHAN16;
    float ib = inP->blue / (float)PF_MAX_CHAN16;
    // Apply tint color to glow
    float gr = g->red / (float)PF_MAX_CHAN16 * s * bi->tintR;
    float gg = g->green / (float)PF_MAX_CHAN16 * s * bi->tintG;
    float gb = g->blue / (float)PF_MAX_CHAN16 * s * bi->tintB;

    outP->red   = (A_u_short)(ScreenBlend(ir, gr) * (float)PF_MAX_CHAN16);
    outP->green = (A_u_short)(ScreenBlend(ig, gg) * (float)PF_MAX_CHAN16);
    outP->blue  = (A_u_short)(ScreenBlend(ib, gb) * (float)PF_MAX_CHAN16);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlendScreenF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    
    int gx = MIN(bi->glow->width - 1, x / bi->factor);
    int gy = MIN(bi->glow->height - 1, y / bi->factor);
    PF_PixelFloat* g = (PF_PixelFloat*)((char*)bi->glow->data + gy * bi->glow->rowbytes) + gx;
    
    float s = bi->strength;
    // User requirement: clamp to display range. Clamp base first to avoid
    // HDR (inP>1) producing negative results in screen when glow is strong.
    const float in_r = MIN(1.0f, MAX(0.0f, inP->red));
    const float in_g = MIN(1.0f, MAX(0.0f, inP->green));
    const float in_b = MIN(1.0f, MAX(0.0f, inP->blue));

    // Apply tint color to glow
    float r = ScreenBlend(in_r, g->red * s * bi->tintR);
    float g = ScreenBlend(in_g, g->green * s * bi->tintG);
    float b = ScreenBlend(in_b, g->blue * s * bi->tintB);
    // Hard clamp to display white.
    outP->red   = MIN(1.0f, MAX(0.0f, r));
    outP->green = MIN(1.0f, MAX(0.0f, g));
    outP->blue  = MIN(1.0f, MAX(0.0f, b));
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

// =============================================================================
// Helper Functions for Field Rendering and Pixel Aspect Ratio
// =============================================================================

// Calculate pixel aspect ratio as float from PF_RationalScale
inline float GetPixelAspectRatioFloat(const PF_RationalScale& par) {
    if (par.den == 0) return 1.0f;  // Safety: avoid division by zero
    return (float)par.num / (float)par.den;
}

// Adjust blur radius for field rendering and pixel aspect ratio
// For field rendering, vertical radius is halved since only half the lines are processed
// For non-square pixels, radius is adjusted to maintain circular appearance
inline void AdjustBlurRadiusForPARAndField(
    int base_radius,
    const PF_RationalScale& par,
    PF_Field field,
    int& out_radius_h,
    int& out_radius_v)
{
    float par_float = GetPixelAspectRatioFloat(par);

    // Base adjustment for pixel aspect ratio
    // Wider pixels (par < 1.0, e.g., NTSC 0.9) need larger H radius
    // Narrower pixels (par > 1.0, e.g., PAL 1.09) need smaller H radius
    // To maintain visual circularity, adjust proportionally
    if (par_float > 0.1f && par_float < 10.0f) {  // Sanity check
        // In AE's coordinate system:
        // - par = num/den represents horizontal_size / vertical_size
        // - For square pixels: par = 1/1 = 1.0
        // - For NTSC D1: par = 10/11 ≈ 0.909 (wider pixels, need more H samples)
        // - For PAL D1: par = 59/54 ≈ 1.09 (narrower pixels, need fewer H samples)

        // We adjust H radius inversely to PAR to maintain circular blur
        out_radius_h = (int)(base_radius / par_float + 0.5f);
        out_radius_v = base_radius;
    } else {
        out_radius_h = base_radius;
        out_radius_v = base_radius;
    }

    // For field rendering, reduce vertical radius since we're only processing
    // half the scanlines. This prevents over-blurring in vertical direction.
    if (field == PF_Field_UPPER || field == PF_Field_LOWER) {
        out_radius_v = MAX(1, out_radius_v / 2);
    }
}

// =============================================================================
// Main Render Function
// =============================================================================

typedef struct {
    float strength;
    float radius;
    float threshold;
    int quality;
    float bloomIntensity;  // User parameter (0-400) divided by 100 for actual multiplier
    float knee;            // User parameter (0-100) divided by 100 for actual knee value
    int blendMode;         // 1=Screen, 2=Add, 3=Normal
    float tintR;           // Tint color red (0-1)
    float tintG;           // Tint color green (0-1)
    float tintB;           // Tint color blue (0-1)
    PF_RationalScale pixel_aspect_ratio;  // Pixel aspect ratio for non-square pixel support
    PF_Field field;        // Field rendering info (FRAME/UPPER/LOWER)
} LiteGlowSettings;

// FIX 5: Added common ValidateSettings function
static PF_Err ValidateSettings(const LiteGlowSettings* settings) {
    if (!settings) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    if (settings->radius < 0.0f || settings->radius > RADIUS_MAX) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->strength < 0.0f || settings->strength > STRENGTH_MAX) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->threshold < THRESHOLD_MIN || settings->threshold > THRESHOLD_MAX) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->quality < 0 || settings->quality >= QUALITY_NUM_CHOICES) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->bloomIntensity < BLOOM_INTENSITY_MIN || settings->bloomIntensity > BLOOM_INTENSITY_MAX) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->knee < KNEE_MIN || settings->knee > KNEE_MAX) {
        return PF_Err_BAD_PARAM;
    }
    if (settings->blendMode < BLEND_MODE_SCREEN || settings->blendMode > BLEND_MODE_NORMAL) {
        return PF_Err_BAD_PARAM;
    }
    return PF_Err_NONE;
}

static PF_Err
ProcessWorlds(PF_InData* in_data, PF_OutData* out_data,
              const LiteGlowSettings* settings,
              PF_EffectWorld* inputW, PF_EffectWorld* outputW)
{
    PF_Err err = PF_Err_NONE;
    // Validate in_data and pica_basicP
    if (!in_data || !in_data->pica_basicP) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // FIX 5: Use common ValidateSettings function
    ERR(ValidateSettings(settings));
    if (err) return err;

    float strength_norm = settings->strength / 2000.0f;
    float threshold_norm = settings->threshold / 255.0f;
    int base_radius = (int)settings->radius;

    // FIX 2: Clamp quality to valid range before array access
    int quality = settings->quality;
    if (quality < 0) quality = 0;
    if (quality >= QUALITY_NUM_CHOICES) quality = QUALITY_NUM_CHOICES - 1;

    if (strength_norm <= 0.0001f || base_radius <= 0) {
        return PF_COPY(inputW, outputW, NULL, NULL);
    }

    AEFX_SuiteScoper<PF_WorldSuite2> worldSuite = AEFX_SuiteScoper<PF_WorldSuite2>(
        in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
    PF_EffectWorld brightW = {}, blur1 = {}, blur2 = {};
    bool brightW_created = false, blur1_created = false, blur2_created = false;

    // Get pixel format
    PF_PixelFormat pixfmt = PF_PixelFormat_INVALID;
    ERR(worldSuite->PF_GetPixelFormat(inputW, &pixfmt));
    if (err) goto cleanup;

    // Downsample for performance: Low=4x, Medium=2x, High=1x
    // Quality to downsample factor mapping (lower quality = higher downsample for performance)
    static const int kQualityToDownsample[] = { 4, 2, 1 };  // Index 0=LOW, 1=MEDIUM, 2=HIGH
    int ds = kQualityToDownsample[quality];
    int dsW = MAX(1, outputW->width / ds);
    int dsH = MAX(1, outputW->height / ds);
    // Clamp ds_radius after adding quality bonus to prevent overflow beyond 24
    int ds_radius = MAX(1, base_radius / ds + (quality == QUALITY_HIGH ? 2 : 0));
    ds_radius = MIN(ds_radius, 24);

    // Calculate separate horizontal and vertical blur radii based on PAR and field rendering
    int ds_radius_h, ds_radius_v;
    AdjustBlurRadiusForPARAndField(ds_radius, settings->pixel_aspect_ratio, settings->field, ds_radius_h, ds_radius_v);
    // Clamp adjusted radii to reasonable limits
    ds_radius_h = MIN(ds_radius_h, MAX_ADJUSTED_BLUR_RADIUS);
    ds_radius_v = MIN(ds_radius_v, 32);

    // Allocate temporary worlds for 4-pass blur
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &brightW));
    if (err) goto cleanup;
    brightW_created = true;

    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &blur1));
    if (err) goto cleanup;
    blur1_created = true;

    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &blur2));
    if (err) goto cleanup;
    blur2_created = true;

    // 1) Bright pass with soft knee for natural threshold
    {
        float knee_norm = settings->knee / 100.0f;
        float intensity_norm = settings->bloomIntensity / 100.0f;
        BrightPassInfo bp{ threshold_norm, knee_norm, intensity_norm, inputW, ds };
        A_long lines = brightW.height;
        
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPass8, &brightW));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPass16, &brightW));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPassF, &brightW));
    }
    if (err) goto cleanup;

    // 2) 4-pass blur for smooth Gaussian approximation
    // Pass 1: Horizontal
    {
        BlurInfo bi{ &brightW, ds_radius_h, ds_radius_h };  // Use horizontal radius for H blur
        A_long lines = brightW.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH8, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH16, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurHF, &blur1));
    }
    if (err) goto cleanup;

    // Pass 2: Vertical
    {
        BlurInfo bi{ &blur1, ds_radius_h, ds_radius_v };  // Use vertical radius for V blur
        A_long lines = blur1.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV8, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV16, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurVF, &blur2));
    }
    if (err) goto cleanup;

    // Pass 3: Horizontal
    {
        BlurInfo bi{ &blur2, ds_radius_h, ds_radius_h };  // Use horizontal radius for H blur
        A_long lines = blur2.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurH8, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurH16, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurHF, &blur1));
    }
    if (err) goto cleanup;

    // Pass 4: Vertical
    {
        BlurInfo bi{ &blur1, ds_radius_h, ds_radius_v };  // Use vertical radius for V blur
        A_long lines = blur1.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV8, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV16, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurVF, &blur2));
    }
    if (err) goto cleanup;

    // 3) Screen blend with tint color
    {
        BlendInfo bl{ &blur2, strength_norm * SCREEN_BLEND_STRENGTH_MULTIPLIER, ds, settings->blendMode, settings->tintR, settings->tintG, settings->tintB };
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreen8, outputW));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreen16, outputW));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreenF, outputW));
    }

cleanup:
    // Dispose all allocated worlds (safe to call even if allocation failed)
    if (brightW_created) worldSuite->PF_DisposeWorld(in_data->effect_ref, &brightW);
    if (blur1_created) worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur1);
    if (blur2_created) worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur2);

    // Always release the suite, even on error paths
    in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);

    return err;
}

// =============================================================================
// GPU Data Structures
// =============================================================================

#if HAS_HLSL
struct DirectXGPUData
{
    DXContextPtr mContext = nullptr;
    ShaderObjectPtr mBrightPassShader = nullptr;
    ShaderObjectPtr mBlurHShader = nullptr;
    ShaderObjectPtr mBlurVShader = nullptr;
    ShaderObjectPtr mBlendShader = nullptr;
};
#endif

// GPU Param structures must match shader constant buffer layout
typedef struct {
    int mSrcPitch;
    int mDstPitch;
    int m16f;
    unsigned int mWidth;
    unsigned int mHeight;
    float mThreshold;
    float mStrength;
    int mFactor;
} BrightPassParams;

typedef struct {
    int mSrcPitch;
    int mDstPitch;
    int m16f;
    unsigned int mWidth;
    unsigned int mHeight;
    int mRadiusH;  // Horizontal blur radius (may differ from vertical for PAR/field rendering)
    int mRadiusV;  // Vertical blur radius
    int mPadding;  // Padding for alignment
} BlurParams;

typedef struct {
    int mSrcPitch;
    int mGlowPitch;
    int mDstPitch;
    int m16f;
    unsigned int mWidth;
    unsigned int mHeight;
    float mStrength;
    int mFactor;
    float mTintR;
    float mTintG;
    float mTintB;
    int mBlendMode;
} BlendParams;

// Compile-time size validation for GPU param structs
static_assert(sizeof(BrightPassParams) == 32, "BrightPassParams size mismatch");
static_assert(sizeof(BlurParams) == 32, "BlurParams size mismatch");
static_assert(sizeof(BlendParams) == 48, "BlendParams size mismatch");

// =============================================================================
// GPU Device Setup/Setdown
// =============================================================================

static PF_Err
GPUDeviceSetup(PF_InData* in_dataP, PF_OutData* out_dataP, PF_GPUDeviceSetupExtra* extraP)
{
    PF_Err err = PF_Err_NONE;

    // This command is per-device/per-framework. Start with no GPU support,
    // then advertise support only for frameworks we actually implement.
    out_dataP->out_flags2 = 0;

    PF_GPUDeviceInfo device_info;
    AEFX_CLR_STRUCT(device_info);

    AEFX_SuiteScoper<PF_HandleSuite1> handle_suite = AEFX_SuiteScoper<PF_HandleSuite1>(
        in_dataP, kPFHandleSuite, kPFHandleSuiteVersion1, out_dataP);

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpuDeviceSuite = AEFX_SuiteScoper<PF_GPUDeviceSuite1>(
        in_dataP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, out_dataP);

    gpuDeviceSuite->GetDeviceInfo(in_dataP->effect_ref, extraP->input->device_index, &device_info);

#if HAS_HLSL
    if (extraP->input->what_gpu == PF_GPU_Framework_DIRECTX)
    {
        PF_Handle gpu_dataH = handle_suite->host_new_handle(sizeof(DirectXGPUData));
        DirectXGPUData* dx_gpu_data = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);
        memset(dx_gpu_data, 0, sizeof(DirectXGPUData));

        // Create objects
        dx_gpu_data->mContext = std::make_shared<DXContext>();
        dx_gpu_data->mBrightPassShader = std::make_shared<ShaderObject>();
        dx_gpu_data->mBlurHShader = std::make_shared<ShaderObject>();
        dx_gpu_data->mBlurVShader = std::make_shared<ShaderObject>();
        dx_gpu_data->mBlendShader = std::make_shared<ShaderObject>();

        // Initialize DXContext
        err = DX_ERR(dx_gpu_data->mContext->Initialize(
            (ID3D12Device*)device_info.devicePV,
            (ID3D12CommandQueue*)device_info.command_queuePV));
        if (err != PF_Err_NONE) {
            dx_gpu_data->mContext.reset();
            handle_suite->host_dispose_handle(gpu_dataH);
            return err;
        }

        std::wstring csoPath, sigPath;

        // Load BrightPass shader with individual error handling
        err = GetShaderPath(ShaderNames::BRIGHT_PASS, csoPath, sigPath);
        if (err == PF_Err_NONE) {
            err = DX_ERR(dx_gpu_data->mContext->LoadShader(csoPath.c_str(), sigPath.c_str(), dx_gpu_data->mBrightPassShader));
        }
        if (err != PF_Err_NONE) {
            dx_gpu_data->mContext.reset();
            dx_gpu_data->mBrightPassShader.reset();
            dx_gpu_data->mBlurHShader.reset();
            dx_gpu_data->mBlurVShader.reset();
            dx_gpu_data->mBlendShader.reset();
            handle_suite->host_dispose_handle(gpu_dataH);
            return err;
        }

        // Load BlurH shader
        err = GetShaderPath(ShaderNames::BLUR_H, csoPath, sigPath);
        if (err == PF_Err_NONE) {
            err = DX_ERR(dx_gpu_data->mContext->LoadShader(csoPath.c_str(), sigPath.c_str(), dx_gpu_data->mBlurHShader));
        }
        if (err != PF_Err_NONE) {
            dx_gpu_data->mContext.reset();
            dx_gpu_data->mBrightPassShader.reset();
            dx_gpu_data->mBlurHShader.reset();
            dx_gpu_data->mBlurVShader.reset();
            dx_gpu_data->mBlendShader.reset();
            handle_suite->host_dispose_handle(gpu_dataH);
            return err;
        }

        // Load BlurV shader
        err = GetShaderPath(ShaderNames::BLUR_V, csoPath, sigPath);
        if (err == PF_Err_NONE) {
            err = DX_ERR(dx_gpu_data->mContext->LoadShader(csoPath.c_str(), sigPath.c_str(), dx_gpu_data->mBlurVShader));
        }
        if (err != PF_Err_NONE) {
            dx_gpu_data->mContext.reset();
            dx_gpu_data->mBrightPassShader.reset();
            dx_gpu_data->mBlurHShader.reset();
            dx_gpu_data->mBlurVShader.reset();
            dx_gpu_data->mBlendShader.reset();
            handle_suite->host_dispose_handle(gpu_dataH);
            return err;
        }

        // Load Blend shader
        err = GetShaderPath(ShaderNames::BLEND, csoPath, sigPath);
        if (err == PF_Err_NONE) {
            err = DX_ERR(dx_gpu_data->mContext->LoadShader(csoPath.c_str(), sigPath.c_str(), dx_gpu_data->mBlendShader));
        }
        if (err != PF_Err_NONE) {
            dx_gpu_data->mContext.reset();
            dx_gpu_data->mBrightPassShader.reset();
            dx_gpu_data->mBlurHShader.reset();
            dx_gpu_data->mBlurVShader.reset();
            dx_gpu_data->mBlendShader.reset();
            handle_suite->host_dispose_handle(gpu_dataH);
            return err;
        }

        extraP->output->gpu_data = gpu_dataH;
        out_dataP->out_flags2 =
            PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 |
            PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    }
#endif

    return err;
}

static PF_Err
GPUDeviceSetdown(PF_InData* in_dataP, PF_OutData* out_dataP, PF_GPUDeviceSetdownExtra* extraP)
{
    PF_Err err = PF_Err_NONE;

#if HAS_HLSL
    if (extraP->input->what_gpu == PF_GPU_Framework_DIRECTX)
    {
        PF_Handle gpu_dataH = (PF_Handle)extraP->input->gpu_data;
        if (gpu_dataH) {
            DirectXGPUData* dx_gpu_data = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);
            if (!dx_gpu_data) {
                return PF_Err_NONE;  // Nothing to clean up
            }

            dx_gpu_data->mContext.reset();
            dx_gpu_data->mBrightPassShader.reset();
            dx_gpu_data->mBlurHShader.reset();
            dx_gpu_data->mBlurVShader.reset();
            dx_gpu_data->mBlendShader.reset();

            AEFX_SuiteScoper<PF_HandleSuite1> handle_suite = AEFX_SuiteScoper<PF_HandleSuite1>(
                in_dataP, kPFHandleSuite, kPFHandleSuiteVersion1, out_dataP);
            handle_suite->host_dispose_handle(gpu_dataH);
        }
    }
#endif

    return err;
}

// =============================================================================
// GPU Render
// =============================================================================

static size_t DivideRoundUp(const size_t inValue, const size_t inMultiple)
{
    return inValue ? (inValue + inMultiple - 1) / inMultiple : 0;
}

static PF_Err
SmartRenderGPU(PF_InData* in_dataP, PF_OutData* out_dataP,
               PF_PixelFormat pixel_format,
               PF_EffectWorld* input_worldP, PF_EffectWorld* output_worldP,
               PF_SmartRenderExtra* extraP, const LiteGlowSettings* settings)
{
    PF_Err err = PF_Err_NONE;

#if HAS_HLSL
    if (extraP->input->what_gpu != PF_GPU_Framework_DIRECTX) {
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }

    if (pixel_format != PF_PixelFormat_GPU_BGRA128) {
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu_suite = AEFX_SuiteScoper<PF_GPUDeviceSuite1>(
        in_dataP, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, out_dataP);

    PF_GPUDeviceInfo device_info;
    ERR(gpu_suite->GetDeviceInfo(in_dataP->effect_ref, extraP->input->device_index, &device_info));

    PF_Handle gpu_dataH = (PF_Handle)extraP->input->gpu_data;
    if (!gpu_dataH) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    DirectXGPUData* dx_gpu_data = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);

    // FIX 5: Use common ValidateSettings function
    ERR(ValidateSettings(settings));
    if (err) return err;

    A_long bytes_per_pixel = BYTES_PER_PIXEL_BGRA128;

    // Calculate downsample factor
    // FIX 2: Clamp quality to valid range before array access
    int quality = settings->quality;
    if (quality < 0) quality = 0;
    if (quality >= QUALITY_NUM_CHOICES) quality = QUALITY_NUM_CHOICES - 1;
    // Quality to downsample factor mapping (lower quality = higher downsample for performance)
    static const int kQualityToDownsample[] = { 4, 2, 1 };  // Index 0=LOW, 1=MEDIUM, 2=HIGH
    int ds = kQualityToDownsample[quality];
    unsigned int dsW = MAX(1, (unsigned int)(output_worldP->width / ds));
    unsigned int dsH = MAX(1, (unsigned int)(output_worldP->height / ds));
    // Calculate ds_radius with quality bonus, clamp after to prevent overflow beyond 24
    // Blur iterations: 2 (H+V twice) for high quality, 1 for medium/low.
    const int blur_iterations = (quality == QUALITY_HIGH) ? 2 : 1;
    int ds_radius = MAX(1, (int)(settings->radius / ds));
    if (blur_iterations == 2) {
        ds_radius += 2;
    } else {
        // Compensate for fewer passes (single box blur vs. repeated box blurs).
        ds_radius = (int)(ds_radius * 1.4f + 0.5f);
    }
    ds_radius = MIN(ds_radius, 24);

    // Calculate separate horizontal and vertical blur radii based on PAR and field rendering
    int ds_radius_h, ds_radius_v;
    AdjustBlurRadiusForPARAndField(ds_radius, settings->pixel_aspect_ratio, settings->field, ds_radius_h, ds_radius_v);
    // Clamp adjusted radii to reasonable limits
    ds_radius_h = MIN(ds_radius_h, MAX_ADJUSTED_BLUR_RADIUS);
    ds_radius_v = MIN(ds_radius_v, 32);

    float strength_norm = settings->strength / 2000.0f;
    float threshold_norm = settings->threshold / 255.0f;

    // Allocate intermediate GPU buffers
    PF_EffectWorld* brightWorld = nullptr;
    PF_EffectWorld* blur1World = nullptr;
    PF_EffectWorld* blur2World = nullptr;

    // Create GPU worlds with individual null checks after each allocation
    ERR(gpu_suite->CreateGPUWorld(in_dataP->effect_ref, extraP->input->device_index,
        dsW, dsH, input_worldP->pix_aspect_ratio, in_dataP->field,
        pixel_format, false, &brightWorld));
    if (err || !brightWorld) {
        err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
        goto cleanup;
    }

    ERR(gpu_suite->CreateGPUWorld(in_dataP->effect_ref, extraP->input->device_index,
        dsW, dsH, input_worldP->pix_aspect_ratio, in_dataP->field,
        pixel_format, false, &blur1World));
    if (err || !blur1World) {
        err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
        goto cleanup;
    }

    ERR(gpu_suite->CreateGPUWorld(in_dataP->effect_ref, extraP->input->device_index,
        dsW, dsH, input_worldP->pix_aspect_ratio, in_dataP->field,
        pixel_format, false, &blur2World));
    if (err || !blur2World) {
        err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED;
        goto cleanup;
    }

    {
        void* src_mem = nullptr;
        void* dst_mem = nullptr;
        void* bright_mem = nullptr;
        void* blur1_mem = nullptr;
        void* blur2_mem = nullptr;

        ERR(gpu_suite->GetGPUWorldData(in_dataP->effect_ref, input_worldP, &src_mem));
        if (err || !src_mem) { err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

        ERR(gpu_suite->GetGPUWorldData(in_dataP->effect_ref, output_worldP, &dst_mem));
        if (err || !dst_mem) { err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

        ERR(gpu_suite->GetGPUWorldData(in_dataP->effect_ref, brightWorld, &bright_mem));
        if (err || !bright_mem) { err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

        ERR(gpu_suite->GetGPUWorldData(in_dataP->effect_ref, blur1World, &blur1_mem));
        if (err || !blur1_mem) { err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

        ERR(gpu_suite->GetGPUWorldData(in_dataP->effect_ref, blur2World, &blur2_mem));
        if (err || !blur2_mem) { err = err ? err : PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

        // 1) Bright Pass
        {
            float knee_norm = settings->knee / 100.0f;
            float intensity_norm = settings->bloomIntensity / 100.0f;
            BrightPassParams params;
            params.mSrcPitch = input_worldP->rowbytes / bytes_per_pixel;
            params.mDstPitch = brightWorld->rowbytes / bytes_per_pixel;
            params.m16f = 0;
            params.mWidth = dsW;
            params.mHeight = dsH;
            params.mThreshold = threshold_norm;
            params.mStrength = intensity_norm;
            params.mFactor = ds;

            DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBrightPassShader, 3);
            DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BrightPassParams)));
            DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)bright_mem, dsH * brightWorld->rowbytes));
            DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)src_mem, input_worldP->height * input_worldP->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(dsW, 16), (UINT)DivideRoundUp(dsH, 16)));
            if (err) goto cleanup;
        }

        // 2) Blur Pass 1: Horizontal
        {
            BlurParams params;
            params.mSrcPitch = brightWorld->rowbytes / bytes_per_pixel;
            params.mDstPitch = blur1World->rowbytes / bytes_per_pixel;
            params.m16f = 0;
            params.mWidth = dsW;
            params.mHeight = dsH;
            params.mRadiusH = ds_radius_h;  // Use horizontal radius
            params.mRadiusV = ds_radius_h;  // H blur uses H radius
            params.mPadding = 0;

            DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBlurHShader, 3);
            DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BlurParams)));
            DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)blur1_mem, dsH * blur1World->rowbytes));
            DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)bright_mem, dsH * brightWorld->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(dsW, 16), (UINT)DivideRoundUp(dsH, 16)));
            if (err) goto cleanup;
        }

        // 3) Blur Pass 2: Vertical
        {
            BlurParams params;
            params.mSrcPitch = blur1World->rowbytes / bytes_per_pixel;
            params.mDstPitch = blur2World->rowbytes / bytes_per_pixel;
            params.m16f = 0;
            params.mWidth = dsW;
            params.mHeight = dsH;
            params.mRadiusH = ds_radius_v;  // V blur uses V radius
            params.mRadiusV = ds_radius_v;
            params.mPadding = 0;

            DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBlurVShader, 3);
            DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BlurParams)));
            DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)blur2_mem, dsH * blur2World->rowbytes));
            DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)blur1_mem, dsH * blur1World->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(dsW, 16), (UINT)DivideRoundUp(dsH, 16)));
            if (err) goto cleanup;
        }

        if (blur_iterations == 2) {
            // 4) Blur Pass 3: Horizontal
            {
                BlurParams params;
                params.mSrcPitch = blur2World->rowbytes / bytes_per_pixel;
                params.mDstPitch = blur1World->rowbytes / bytes_per_pixel;
                params.m16f = 0;
                params.mWidth = dsW;
                params.mHeight = dsH;
                params.mRadiusH = ds_radius_h;  // Use horizontal radius
                params.mRadiusV = ds_radius_h;
                params.mPadding = 0;

                DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBlurHShader, 3);
                DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BlurParams)));
                DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)blur1_mem, dsH * blur1World->rowbytes));
                DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)blur2_mem, dsH * blur2World->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(dsW, 16), (UINT)DivideRoundUp(dsH, 16)));
            if (err) goto cleanup;
            }

            // 5) Blur Pass 4: Vertical
            {
                BlurParams params;
                params.mSrcPitch = blur1World->rowbytes / bytes_per_pixel;
                params.mDstPitch = blur2World->rowbytes / bytes_per_pixel;
                params.m16f = 0;
                params.mWidth = dsW;
                params.mHeight = dsH;
                params.mRadiusH = ds_radius_v;  // V blur uses V radius
                params.mRadiusV = ds_radius_v;
                params.mPadding = 0;

                DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBlurVShader, 3);
                DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BlurParams)));
                DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)blur2_mem, dsH * blur2World->rowbytes));
                DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)blur1_mem, dsH * blur1World->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(dsW, 16), (UINT)DivideRoundUp(dsH, 16)));
            if (err) goto cleanup;
            }
        }

        // 6) Screen Blend
        {
            BlendParams params;
            params.mSrcPitch = input_worldP->rowbytes / bytes_per_pixel;
            params.mGlowPitch = blur2World->rowbytes / bytes_per_pixel;
            params.mDstPitch = output_worldP->rowbytes / bytes_per_pixel;
            params.m16f = 0;
            params.mWidth = output_worldP->width;
            params.mHeight = output_worldP->height;
            params.mStrength = strength_norm * SCREEN_BLEND_STRENGTH_MULTIPLIER;
            params.mFactor = ds;
            params.mTintR = settings->tintR;
            params.mTintG = settings->tintG;
            params.mTintB = settings->tintB;
            params.mBlendMode = settings->blendMode;

            DXShaderExecution shaderExec(dx_gpu_data->mContext, dx_gpu_data->mBlendShader, 4);
            DX_ERR(shaderExec.SetParamBuffer(&params, sizeof(BlendParams)));
            DX_ERR(shaderExec.SetUnorderedAccessView((ID3D12Resource*)dst_mem, output_worldP->height * output_worldP->rowbytes));
            DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)src_mem, input_worldP->height * input_worldP->rowbytes));
            DX_ERR(shaderExec.SetShaderResourceView((ID3D12Resource*)blur2_mem, dsH * blur2World->rowbytes));
            err = DX_ERR(shaderExec.Execute((UINT)DivideRoundUp(output_worldP->width, THREAD_GROUP_SIZE_X), (UINT)DivideRoundUp(output_worldP->height, THREAD_GROUP_SIZE_Y)));
            if (err) goto cleanup;
        }
    }

cleanup:
    if (brightWorld) {
        gpu_suite->DisposeGPUWorld(in_dataP->effect_ref, brightWorld);
        brightWorld = nullptr;
    }
    if (blur1World) {
        gpu_suite->DisposeGPUWorld(in_dataP->effect_ref, blur1World);
        blur1World = nullptr;
    }
    if (blur2World) {
        gpu_suite->DisposeGPUWorld(in_dataP->effect_ref, blur2World);
        blur2World = nullptr;
    }

#endif // HAS_HLSL

    return err;
}

// =============================================================================
// Smart Render Handlers
// =============================================================================

static PF_Err
SmartPreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* pre)
{
    PF_Err err = PF_Err_NONE;
    PF_RenderRequest req = pre->input->output_request;
    PF_CheckoutResult in_result;

    // Signal that GPU rendering is possible
    pre->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

    ERR(pre->cb->checkout_layer(
        in_data->effect_ref,
        LITEGLOW_INPUT,
        LITEGLOW_INPUT,
        &req,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &in_result));

    pre->output->result_rect = in_result.result_rect;
    pre->output->max_result_rect = in_result.max_result_rect;

    return err;
}

static PF_Err
SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extraP, bool isGPU)
{
    PF_Err err = PF_Err_NONE;
    PF_Err err2 = PF_Err_NONE;
    PF_EffectWorld* input_worldP = nullptr;
    PF_EffectWorld* output_worldP = nullptr;

    PF_ParamDef strength_param, radius_param, threshold_param, quality_param;
    PF_ParamDef bloom_intensity_param, knee_param, blend_mode_param, tint_color_param;
    AEFX_CLR_STRUCT(strength_param);
    AEFX_CLR_STRUCT(radius_param);
    AEFX_CLR_STRUCT(threshold_param);
    AEFX_CLR_STRUCT(quality_param);
    AEFX_CLR_STRUCT(bloom_intensity_param);
    AEFX_CLR_STRUCT(knee_param);
    AEFX_CLR_STRUCT(blend_mode_param);
    AEFX_CLR_STRUCT(tint_color_param);

    ERR(extraP->cb->checkout_layer_pixels(in_data->effect_ref, LITEGLOW_INPUT, &input_worldP));
    ERR(extraP->cb->checkout_output(in_data->effect_ref, &output_worldP));

    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_STRENGTH, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &strength_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_RADIUS, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &radius_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_THRESHOLD, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &threshold_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_QUALITY, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &quality_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_BLOOM_INTENSITY, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &bloom_intensity_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_KNEE, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &knee_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_BLEND_MODE, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &blend_mode_param));
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_TINT_COLOR, in_data->current_time,
                          in_data->time_step, in_data->time_scale, &tint_color_param));

    if (!err && input_worldP && output_worldP) {
        LiteGlowSettings settings;
        settings.strength = strength_param.u.fs_d.value;
        settings.radius = radius_param.u.fs_d.value;
        settings.threshold = threshold_param.u.fs_d.value;
        settings.quality = quality_param.u.pd.value;
        settings.bloomIntensity = bloom_intensity_param.u.fs_d.value;
        settings.knee = knee_param.u.fs_d.value;
        settings.blendMode = blend_mode_param.u.pd.value;
        // Convert tint color from 0-65535 to 0-1 range
        settings.tintR = tint_color_param.u.cd.value.red / 65535.0f;
        settings.tintG = tint_color_param.u.cd.value.green / 65535.0f;
        settings.tintB = tint_color_param.u.cd.value.blue / 65535.0f;
        // Get pixel aspect ratio from input world
        settings.pixel_aspect_ratio = input_worldP->pix_aspect_ratio;
        // Get field rendering info from in_data
        settings.field = in_data->field;

        if (isGPU) {
            AEFX_SuiteScoper<PF_WorldSuite2> world_suite = AEFX_SuiteScoper<PF_WorldSuite2>(
                in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
            PF_PixelFormat pixel_format = PF_PixelFormat_INVALID;
            ERR(world_suite->PF_GetPixelFormat(input_worldP, &pixel_format));

            err = SmartRenderGPU(in_data, out_data, pixel_format, input_worldP, output_worldP, extraP, &settings);
        } else {
            err = ProcessWorlds(in_data, out_data, &settings, input_worldP, output_worldP);
        }
    }

    ERR2(PF_CHECKIN_PARAM(in_data, &strength_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &radius_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &threshold_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &quality_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &bloom_intensity_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &knee_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &blend_mode_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &tint_color_param));

    return err;
}

static PF_Err
Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_EffectWorld* inputW = &params[LITEGLOW_INPUT]->u.ld;
    PF_EffectWorld* outputW = reinterpret_cast<PF_EffectWorld*>(output);
    LiteGlowSettings s;
    s.strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    s.radius = params[LITEGLOW_RADIUS]->u.fs_d.value;
    s.threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value;
    s.quality = params[LITEGLOW_QUALITY]->u.pd.value;
    s.bloomIntensity = params[LITEGLOW_BLOOM_INTENSITY]->u.fs_d.value;
    s.knee = params[LITEGLOW_KNEE]->u.fs_d.value;
    s.blendMode = params[LITEGLOW_BLEND_MODE]->u.pd.value;
    // Convert tint color from 0-65535 to 0-1 range
    s.tintR = params[LITEGLOW_TINT_COLOR]->u.cd.value.red / 65535.0f;
    s.tintG = params[LITEGLOW_TINT_COLOR]->u.cd.value.green / 65535.0f;
    s.tintB = params[LITEGLOW_TINT_COLOR]->u.cd.value.blue / 65535.0f;
    // Get pixel aspect ratio from input world
    s.pixel_aspect_ratio = inputW->pix_aspect_ratio;
    // Get field rendering info from in_data
    s.field = in_data->field;
    return ProcessWorlds(in_data, out_data, &s, inputW, outputW);
}

// =============================================================================
// Entry Point
// =============================================================================

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "LiteGlow",
        "361do LiteGlow",
        "361do_plugins",
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com/rebuildup/Ae_LiteGlow");
    return result;
}

PF_Err
EffectMain(
    PF_Cmd cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra)
{
    PF_Err err = PF_Err_NONE;
    
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            err = About(in_data, out_data, params, output);
            break;
        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data, params, output);
            break;
        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data, params, output);
            break;
        case PF_Cmd_GPU_DEVICE_SETUP:
            err = GPUDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);
            break;
        case PF_Cmd_GPU_DEVICE_SETDOWN:
            err = GPUDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra);
            break;
        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = SmartPreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, false);
            break;
        case PF_Cmd_SMART_RENDER_GPU:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, true);
            break;
        case PF_Cmd_COMPLETELY_GENERAL:
            err = PF_Err_NONE;
            break;
        default:
            break;
        }
    }
    catch (PF_Err& thrown) {
        // Re-throw AE plugin errors - error value is already captured in 'thrown'
        // No additional logging needed as AE framework handles error reporting
        err = thrown;
    }
    
    return err;
}
