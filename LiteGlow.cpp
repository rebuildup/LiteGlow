#include "LiteGlow.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "AE_EffectPixelFormat.h"
#include <math.h>

// Simple, reliable CPU-only glow rebuilt from Skeleton template.
// Keeps existing UI: Strength, Radius, Threshold, Quality.

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

    // Enable threaded rendering + Smart Render (8/16bpc)
    out_data->out_flags2 = 0; // no smart render / no float

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

// -------- Pixel utilities --------
inline float Luma8(const PF_Pixel8* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / 255.0f;
}
inline float Luma16(const PF_Pixel16* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue) / 32768.0f;
}
inline float LumaF(const PF_PixelFloat* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue);
}

// -------- Bright pass --------
typedef struct {
    float threshold;   // 0..1
    float strength;    // 0..1 normalized
} BrightPassInfo;

static PF_Err BrightPass8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP)
{
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    float l = Luma8(inP);
    if (l > bp->threshold) {
        float s = bp->strength;
        outP->red   = (A_u_char)MIN(255.0f, inP->red   * s);
        outP->green = (A_u_char)MIN(255.0f, inP->green * s);
        outP->blue  = (A_u_char)MIN(255.0f, inP->blue  * s);
    } else {
        outP->red = outP->green = outP->blue = 0;
    }
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BrightPassF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP)
{
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    float l = LumaF(inP);
    if (l > bp->threshold) {
        float s = bp->strength;
        outP->red   = MIN(1.0f, inP->red   * s);
        outP->green = MIN(1.0f, inP->green * s);
        outP->blue  = MIN(1.0f, inP->blue  * s);
    } else {
        outP->red = outP->green = outP->blue = 0.0f;
    }
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BrightPass16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP)
{
    BrightPassInfo* bp = reinterpret_cast<BrightPassInfo*>(refcon);
    float l = Luma16(inP);
    if (l > bp->threshold) {
        float s = bp->strength;
        outP->red   = (A_u_short)MIN(32768.0f, inP->red   * s);
        outP->green = (A_u_short)MIN(32768.0f, inP->green * s);
        outP->blue  = (A_u_short)MIN(32768.0f, inP->blue  * s);
    } else {
        outP->red = outP->green = outP->blue = 0;
    }
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

// -------- Box blur (separable) --------
typedef struct {
    PF_EffectWorld* src;
    int radius;
} BlurInfo;

static PF_Err BlurHF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_PixelFloat* p = (PF_PixelFloat*)((char*)w->data + y * w->rowbytes) + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = rsum / count;
    outP->green = gsum / count;
    outP->blue = bsum / count;
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlurVF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_PixelFloat* p = (PF_PixelFloat*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = rsum / count;
    outP->green = gsum / count;
    outP->blue = bsum / count;
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlurH8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel8* p = (PF_Pixel8*)((char*)w->data + y * w->rowbytes) + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = (A_u_char)(rsum / count);
    outP->green = (A_u_char)(gsum / count);
    outP->blue = (A_u_char)(bsum / count);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlurH16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int i = -r; i <= r; ++i) {
        int sx = MAX(0, MIN(w->width - 1, x + i));
        PF_Pixel16* p = (PF_Pixel16*)((char*)w->data + y * w->rowbytes) + sx;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = (A_u_short)(rsum / count);
    outP->green = (A_u_short)(gsum / count);
    outP->blue = (A_u_short)(bsum / count);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlurV8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel8* p = (PF_Pixel8*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = (A_u_char)(rsum / count);
    outP->green = (A_u_char)(gsum / count);
    outP->blue = (A_u_char)(bsum / count);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlurV16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP)
{
    BlurInfo* bi = reinterpret_cast<BlurInfo*>(refcon);
    PF_EffectWorld* w = bi->src;
    int r = bi->radius;
    float rsum = 0, gsum = 0, bsum = 0;
    int count = 0;
    for (int j = -r; j <= r; ++j) {
        int sy = MAX(0, MIN(w->height - 1, y + j));
        PF_Pixel16* p = (PF_Pixel16*)((char*)w->data + sy * w->rowbytes) + x;
        rsum += p->red; gsum += p->green; bsum += p->blue;
        ++count;
    }
    outP->red = (A_u_short)(rsum / count);
    outP->green = (A_u_short)(gsum / count);
    outP->blue = (A_u_short)(bsum / count);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

// -------- Screen blend --------
typedef struct {
    PF_EffectWorld* glow;
    float strength; // 0..1
} BlendInfo;

static PF_Err BlendScreenF(void* refcon, A_long x, A_long y, PF_PixelFloat* inP, PF_PixelFloat* outP)
{
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    PF_PixelFloat* g = (PF_PixelFloat*)((char*)bi->glow->data + y * bi->glow->rowbytes) + x;

    float s = bi->strength;
    float gr = g->red   * s;
    float gg = g->green * s;
    float gb = g->blue  * s;

    auto screen = [](float a, float b) { return 1.0f - (1.0f - a) * (1.0f - b); };
    float r = screen(inP->red, gr);
    float gch = screen(inP->green, gg);
    float b = screen(inP->blue, gb);

    outP->red = r;
    outP->green = gch;
    outP->blue = b;
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlendScreen8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP)
{
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    PF_Pixel8* g = (PF_Pixel8*)((char*)bi->glow->data + y * bi->glow->rowbytes) + x;

    float s = bi->strength;
    float gr = (g->red   / 255.0f) * s;
    float gg = (g->green / 255.0f) * s;
    float gb = (g->blue  / 255.0f) * s;

    auto screen = [](float a, float b) { return 1.0f - (1.0f - a) * (1.0f - b); };
    float r = screen(inP->red   / 255.0f, gr);
    float gch = screen(inP->green / 255.0f, gg);
    float b = screen(inP->blue  / 255.0f, gb);

    outP->red = (A_u_char)(r * 255.0f);
    outP->green = (A_u_char)(gch * 255.0f);
    outP->blue = (A_u_char)(b * 255.0f);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

static PF_Err BlendScreen16(void* refcon, A_long x, A_long y, PF_Pixel16* inP, PF_Pixel16* outP)
{
    BlendInfo* bi = reinterpret_cast<BlendInfo*>(refcon);
    PF_Pixel16* g = (PF_Pixel16*)((char*)bi->glow->data + y * bi->glow->rowbytes) + x;

    float s = bi->strength;
    float gr = (g->red   / 32768.0f) * s;
    float gg = (g->green / 32768.0f) * s;
    float gb = (g->blue  / 32768.0f) * s;

    auto screen = [](float a, float b) { return 1.0f - (1.0f - a) * (1.0f - b); };
    float r = screen(inP->red   / 32768.0f, gr);
    float gch = screen(inP->green / 32768.0f, gg);
    float b = screen(inP->blue  / 32768.0f, gb);

    outP->red = (A_u_short)(r * 32768.0f);
    outP->green = (A_u_short)(gch * 32768.0f);
    outP->blue = (A_u_short)(b * 32768.0f);
    outP->alpha = inP->alpha;
    return PF_Err_NONE;
}

// -------- Render --------
static PF_Err
ProcessWorlds(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EffectWorld* inputW, PF_EffectWorld* outputW)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    const float strength_slider = params[LITEGLOW_STRENGTH]->u.fs_d.value; // 0..2000
    const float radius_slider = params[LITEGLOW_RADIUS]->u.fs_d.value;     // 1..50
    const float threshold_slider = params[LITEGLOW_THRESHOLD]->u.fs_d.value; // 0..255
    const int quality = params[LITEGLOW_QUALITY]->u.pd.value;

    // Normalize values
    float strength_norm = strength_slider / 2000.0f; // 0..1
    float threshold_norm = threshold_slider / 255.0f; // 0..1
    int radius = (int)radius_slider;
    if (quality == QUALITY_HIGH) radius += 4;
    if (quality == QUALITY_LOW)  radius = MAX(1, radius / 2);
    radius = MIN(radius, 32);

    // Early out
    if (strength_norm <= 0.0001f || radius <= 0) {
        return PF_COPY(inputW, outputW, NULL, NULL);
    }

    AEFX_SuiteHelperT<PF_WorldSuite2> worldSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2);
    PF_PixelFormat pixfmt = PF_PixelFormat_INVALID;
    ERR(worldSuite->PF_GetPixelFormat(inputW, &pixfmt));
    if (pixfmt == PF_PixelFormat_INVALID) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    if (pixfmt == PF_PixelFormat_ARGB128) {
        // We no longer support 32f directly; let AE convert or fall back.
        return PF_COPY(inputW, outputW, NULL, NULL);
    }

    // Allocate temporary worlds
    PF_EffectWorld brightW, blurH, blurV;
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, outputW->width, outputW->height, TRUE, pixfmt, &brightW));
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, outputW->width, outputW->height, TRUE, pixfmt, &blurH));
    ERR(worldSuite->PF_NewWorld(in_data->effect_ref, outputW->width, outputW->height, TRUE, pixfmt, &blurV));

    if (!err) {
        // 1) Bright pass
        BrightPassInfo bp{ threshold_norm, 1.5f }; // extra gain to ensure visibility
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32) {
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines,
                inputW, NULL, &bp, BrightPass8, &brightW));
        }
        else if (pixfmt == PF_PixelFormat_ARGB64) {
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines,
                inputW, NULL, &bp, BrightPass16, &brightW));
        }
        else { /* no-op */ }
    }

    if (!err) {
        // 2) Blur horizontal
        BlurInfo bi{ &brightW, radius };
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32) {
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH8, &blurH));
        }
        else if (pixfmt == PF_PixelFormat_ARGB64) {
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &brightW, NULL, &bi, BlurH16, &blurH));
        }
        else { /* no-op */ }
    }

    if (!err) {
        // 3) Blur vertical
        BlurInfo bi{ &blurH, radius };
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32) {
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines, &blurH, NULL, &bi, BlurV8, &blurV));
        }
        else if (pixfmt == PF_PixelFormat_ARGB64) {
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines, &blurH, NULL, &bi, BlurV16, &blurV));
        }
        else { /* no-op */ }
    }

    if (!err) {
        // 4) Screen blend
        BlendInfo bl{ &blurV, MAX(0.1f, strength_norm * 2.0f) };
        A_long lines = outputW->height;
        if (pixfmt == PF_PixelFormat_ARGB32) {
            ERR(suites.Iterate8Suite2()->iterate(in_data, 0, lines,
                inputW, NULL, &bl, BlendScreen8, outputW));
        }
        else if (pixfmt == PF_PixelFormat_ARGB64) {
            ERR(suites.Iterate16Suite2()->iterate(in_data, 0, lines,
                inputW, NULL, &bl, BlendScreen16, outputW));
        }
        else { /* no-op */ }
    }

    // Dispose temps
    worldSuite->PF_DisposeWorld(in_data->effect_ref, &brightW);
    worldSuite->PF_DisposeWorld(in_data->effect_ref, &blurH);
    worldSuite->PF_DisposeWorld(in_data->effect_ref, &blurV);

    return err;
}

static PF_Err
Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_EffectWorld* inputW = &params[LITEGLOW_INPUT]->u.ld;
    PF_EffectWorld* outputW = reinterpret_cast<PF_EffectWorld*>(output);
    return ProcessWorlds(in_data, out_data, params, inputW, outputW);
}

// -------- Entry ----------
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
        case PF_Cmd_ABOUT:          err = About(in_data, out_data, params, output); break;
        case PF_Cmd_GLOBAL_SETUP:   err = GlobalSetup(in_data, out_data, params, output); break;
        case PF_Cmd_PARAMS_SETUP:   err = ParamsSetup(in_data, out_data, params, output); break;
        case PF_Cmd_RENDER:         err = Render(in_data, out_data, params, output); break;
        default: break;
        }
    }
    catch (PF_Err& thrown) { err = thrown; }
    return err;
}
