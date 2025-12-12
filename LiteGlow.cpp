#include "LiteGlow.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "AE_EffectPixelFormat.h"

#include <math.h>
#include <cstdlib>
#include <algorithm>

// =============================================================================
// LiteGlow - High-Performance Glow Effect
// Multi-Frame Rendering + 32-bit Float Support
// =============================================================================

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
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

    // SmartRender + Multi-Frame Rendering + 32-bit float + GPU support
    out_data->out_flags2 = 
        PF_OutFlag2_SUPPORTS_SMART_RENDER |
        PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
        PF_OutFlag2_FLOAT_COLOR_AWARE |
        PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;

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

    out_data->num_params = LITEGLOW_NUM_PARAMS;
    return err;
}

// =============================================================================
// Optimized CPU Pixel Processing
// =============================================================================

inline float Luma8(const PF_Pixel8* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / 255.0f;
}

inline float Luma16(const PF_Pixel16* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / 32768.0f;
}

inline float LumaF(const PF_PixelFloat* p) {
    return 0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue;
}

// Soft knee for natural threshold transition
inline float SoftKnee(float x, float threshold, float knee) {
    float knee_start = threshold - knee;
    float knee_end = threshold + knee;
    
    if (x <= knee_start) return 0.0f;
    if (x >= knee_end) return x - threshold;
    
    float t = (x - knee_start) / (knee_end - knee_start);
    return t * t * (3.0f - 2.0f * t) * (x - threshold);
}

// Screen blend
inline float ScreenBlend(float a, float b) {
    return 1.0f - (1.0f - a) * (1.0f - b);
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

static PF_Err BrightPass8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_Pixel8* srcP = inP;
    
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_Pixel8*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    
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
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_Pixel16* srcP = inP;
    
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_Pixel16*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    
    float l = Luma16(srcP);
    float contribution = SoftKnee(l, bp->threshold, bp->knee);
    
    if (contribution > 0.0f) {
        float scale = bp->intensity * (contribution / MAX(0.001f, l - bp->threshold + contribution));
        outP->red   = (A_u_short)MIN(32768.0f, srcP->red   * scale);
        outP->green = (A_u_short)MIN(32768.0f, srcP->green * scale);
        outP->blue  = (A_u_short)MIN(32768.0f, srcP->blue  * scale);
    } else {
        outP->red = outP->green = outP->blue = 0;
    }
    outP->alpha = srcP->alpha;
    return PF_Err_NONE;
}

static PF_Err BrightPassF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    PF_PixelFloat* srcP = inP;
    
    if (bp->factor > 1 && bp->src) {
        int sx = MIN(bp->src->width - 1, (int)(x * bp->factor));
        int sy = MIN(bp->src->height - 1, (int)(y * bp->factor));
        srcP = (PF_PixelFloat*)((char*)bp->src->data + sy * bp->src->rowbytes) + sx;
    }
    
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
    int radius;
} BlurInfo;

static PF_Err BlurH8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    PF_Pixel8* row = (PF_Pixel8*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel8* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = (A_u_char)(rsum / count);
    outP->green = (A_u_char)(gsum / count);
    outP->blue = (A_u_char)(bsum / count);
    outP->alpha = (A_u_char)(asum / count);
    return PF_Err_NONE;
}

static PF_Err BlurH16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    PF_Pixel16* row = (PF_Pixel16*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel16* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = (A_u_short)(rsum / count);
    outP->green = (A_u_short)(gsum / count);
    outP->blue = (A_u_short)(bsum / count);
    outP->alpha = (A_u_short)(asum / count);
    return PF_Err_NONE;
}

static PF_Err BlurHF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    PF_PixelFloat* row = (PF_PixelFloat*)((char*)w->data + y * w->rowbytes);
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_PixelFloat* p = row + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = rsum / count;
    outP->green = gsum / count;
    outP->blue = bsum / count;
    outP->alpha = asum / count;
    return PF_Err_NONE;
}

static PF_Err BlurV8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel8* p = (PF_Pixel8*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = (A_u_char)(rsum / count);
    outP->green = (A_u_char)(gsum / count);
    outP->blue = (A_u_char)(bsum / count);
    outP->alpha = (A_u_char)(asum / count);
    return PF_Err_NONE;
}

static PF_Err BlurV16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    int rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel16* p = (PF_Pixel16*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = (A_u_short)(rsum / count);
    outP->green = (A_u_short)(gsum / count);
    outP->blue = (A_u_short)(bsum / count);
    outP->alpha = (A_u_short)(asum / count);
    return PF_Err_NONE;
}

static PF_Err BlurVF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0, asum = 0;
    int count = 0;
    
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_PixelFloat* p = (PF_PixelFloat*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue; asum += p->alpha;
        ++count;
    }
    outP->red = rsum / count;
    outP->green = gsum / count;
    outP->blue = bsum / count;
    outP->alpha = asum / count;
    return PF_Err_NONE;
}

// =============================================================================
// Screen Blend Functions
// =============================================================================

typedef struct {
    PF_EffectWorld* glow;
    float strength;
    int factor;
} BlendInfo;

static PF_Err BlendScreen8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    
    int gx = MIN(bi->glow->width - 1, x / bi->factor);
    int gy = MIN(bi->glow->height - 1, y / bi->factor);
    PF_Pixel8* g = (PF_Pixel8*)((char*)bi->glow->data + gy * bi->glow->rowbytes) + gx;
    
    float s = bi->strength;
    float ir = inP->red / 255.0f;
    float ig = inP->green / 255.0f;
    float ib = inP->blue / 255.0f;
    float gr = g->red / 255.0f * s;
    float gg = g->green / 255.0f * s;
    float gb = g->blue / 255.0f * s;
    
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
    float ir = inP->red / 32768.0f;
    float ig = inP->green / 32768.0f;
    float ib = inP->blue / 32768.0f;
    float gr = g->red / 32768.0f * s;
    float gg = g->green / 32768.0f * s;
    float gb = g->blue / 32768.0f * s;
    
    outP->red   = (A_u_short)(ScreenBlend(ir, gr) * 32768.0f);
    outP->green = (A_u_short)(ScreenBlend(ig, gg) * 32768.0f);
    outP->blue  = (A_u_short)(ScreenBlend(ib, gb) * 32768.0f);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlendScreenF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    
    int gx = MIN(bi->glow->width - 1, x / bi->factor);
    int gy = MIN(bi->glow->height - 1, y / bi->factor);
    PF_PixelFloat* g = (PF_PixelFloat*)((char*)bi->glow->data + gy * bi->glow->rowbytes) + gx;
    
    float s = bi->strength;
    outP->red   = ScreenBlend(inP->red, g->red * s);
    outP->green = ScreenBlend(inP->green, g->green * s);
    outP->blue  = ScreenBlend(inP->blue, g->blue * s);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

// =============================================================================
// Main Render Function
// =============================================================================

typedef struct {
    float strength;
    float radius;
    float threshold;
    int quality;
} LiteGlowSettings;

static PF_Err
ProcessWorlds(PF_InData* in_data, PF_OutData* out_data,
              const LiteGlowSettings* settings,
              PF_EffectWorld* inputW, PF_EffectWorld* outputW)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    float strength_norm = settings->strength / 2000.0f;
    float threshold_norm = settings->threshold / 255.0f;
    int base_radius = (int)settings->radius;
    int quality = settings->quality;

    if (strength_norm <= 0.0001f || base_radius <= 0) {
        return PF_COPY(inputW, outputW, NULL, NULL);
    }

    AEFX_SuiteHelperT<PF_WorldSuite2> worldSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2);
    PF_PixelFormat pixfmt = PF_PixelFormat_INVALID;
    ERR(worldSuite->PF_GetPixelFormat(inputW, &pixfmt));

    // Downsample for performance: Low=4x, Medium=2x, High=1x
    int ds = (quality == QUALITY_HIGH) ? 1 : (quality == QUALITY_MEDIUM ? 2 : 4);
    int dsW = MAX(1, outputW->width / ds);
    int dsH = MAX(1, outputW->height / ds);
    int ds_radius = MAX(1, base_radius / ds);
    if (quality == QUALITY_HIGH) ds_radius += 2;
    ds_radius = MIN(ds_radius, 24);

    // Allocate temporary worlds for 4-pass blur
    PF_EffectWorld brightW, blur1, blur2;
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &brightW));
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &blur1));
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, dsW, dsH, TRUE, pixfmt, &blur2));

    if (err) {
        worldSuite->PF_DisposeWorld(in_data->effect_ref, &brightW);
        worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur1);
        worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur2);
        return err;
    }

    // 1) Bright pass with soft knee for natural threshold
    if (!err) {
        float knee = 0.1f;
        BrightPassInfo bp{ threshold_norm, knee, 1.5f, inputW, ds };
        A_long lines = brightW.height;
        
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPass8, &brightW));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPass16, &brightW));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &brightW, NULL, &bp, BrightPassF, &brightW));
    }

    // 2) 4-pass blur for smooth Gaussian approximation
    // Pass 1: Horizontal
    if (!err) {
        BlurInfo bi{ &brightW, ds_radius };
        A_long lines = brightW.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH8, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH16, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurHF, &blur1));
    }

    // Pass 2: Vertical
    if (!err) {
        BlurInfo bi{ &blur1, ds_radius };
        A_long lines = blur1.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV8, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV16, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurVF, &blur2));
    }

    // Pass 3: Horizontal
    if (!err) {
        BlurInfo bi{ &blur2, ds_radius };
        A_long lines = blur2.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurH8, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurH16, &blur1));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur2, NULL, &bi, BlurHF, &blur1));
    }

    // Pass 4: Vertical
    if (!err) {
        BlurInfo bi{ &blur1, ds_radius };
        A_long lines = blur1.height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV8, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurV16, &blur2));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, &blur1, NULL, &bi, BlurVF, &blur2));
    }

    // 3) Screen blend
    if (!err) {
        BlendInfo bl{ &blur2, strength_norm * 2.0f, ds };
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32)
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreen8, outputW));
        else if (pixfmt == PF_PixelFormat_ARGB64)
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreen16, outputW));
        else if (pixfmt == PF_PixelFormat_ARGB128)
            ERR(suites.IterateFloatSuite2()->iterate(in_data, 0, lines, inputW, NULL, &bl, BlendScreenF, outputW));
    }

    worldSuite->PF_DisposeWorld(in_data->effect_ref, &brightW);
    worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur1);
    worldSuite->PF_DisposeWorld(in_data->effect_ref, &blur2);

    return err;
}

// =============================================================================
// GPU Device Setup/Setdown
// =============================================================================

static PF_Err
GPUDeviceSetup(PF_InData* in_dataP, PF_OutData* out_dataP, PF_GPUDeviceSetupExtra* extraP)
{
    PF_Err err = PF_Err_NONE;
    
    // Signal that GPU rendering is supported (will fall back to CPU)
    out_dataP->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    
    return err;
}

static PF_Err
GPUDeviceSetdown(PF_InData* in_dataP, PF_OutData* out_dataP, PF_GPUDeviceSetdownExtra* extraP)
{
    return PF_Err_NONE;
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

    // Enable GPU rendering possibility
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
SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extraP)
{
    PF_Err err = PF_Err_NONE;
    PF_Err err2 = PF_Err_NONE;
    PF_EffectWorld* input_worldP = nullptr;
    PF_EffectWorld* output_worldP = nullptr;

    PF_ParamDef strength_param, radius_param, threshold_param, quality_param;
    AEFX_CLR_STRUCT(strength_param);
    AEFX_CLR_STRUCT(radius_param);
    AEFX_CLR_STRUCT(threshold_param);
    AEFX_CLR_STRUCT(quality_param);

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

    if (!err && input_worldP && output_worldP) {
        LiteGlowSettings settings;
        settings.strength = strength_param.u.fs_d.value;
        settings.radius = radius_param.u.fs_d.value;
        settings.threshold = threshold_param.u.fs_d.value;
        settings.quality = quality_param.u.pd.value;

        err = ProcessWorlds(in_data, out_data, &settings, input_worldP, output_worldP);
    }

    ERR2(PF_CHECKIN_PARAM(in_data, &strength_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &radius_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &threshold_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &quality_param));

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
        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;
        case PF_Cmd_SMART_PRE_RENDER:
            err = SmartPreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        case PF_Cmd_GPU_DEVICE_SETUP:
            err = GPUDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);
            break;
        case PF_Cmd_GPU_DEVICE_SETDOWN:
            err = GPUDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra);
            break;
        case PF_Cmd_SMART_RENDER_GPU:
            // GPU render falls back to CPU implementation
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
            break;
        default:
            break;
        }
    }
    catch (PF_Err& thrown) { err = thrown; }
    
    return err;
}
