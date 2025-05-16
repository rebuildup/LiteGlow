#include "LiteGlow.h"
#include <math.h>

// Constants for Gaussian kernel generation
#define PI 3.14159265358979323846
#define KERNEL_SIZE_MAX 64

// Sequence data counter
static A_long gSequenceCount = 0;

static PF_Err
About(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION,
        MINOR_VERSION,
        BUG_VERSION,
        STAGE_VERSION,
        BUILD_VERSION);

    // Set up flags for optimizations
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;  // 16bpc support
    out_data->out_flags |= PF_OutFlag_PIX_INDEPENDENT;  // Enable parallel processing
    out_data->out_flags |= PF_OutFlag_SEND_UPDATE_PARAMS_UI; // Update UI during processing

    // Enable Multi-Frame Rendering support if available
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);

    // Add Strength slider parameter
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Strength_Param_Name),
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_DFLT,
        PF_Precision_INTEGER,
        0,
        0,
        STRENGTH_DISK_ID);

    // Add Radius slider for blur radius control
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Radius_Param_Name),
        RADIUS_MIN,
        RADIUS_MAX,
        RADIUS_MIN,
        RADIUS_MAX,
        RADIUS_DFLT,
        PF_Precision_INTEGER,
        0,
        0,
        RADIUS_DISK_ID);

    // Add Threshold slider to control which areas glow
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Threshold_Param_Name),
        THRESHOLD_MIN,
        THRESHOLD_MAX,
        THRESHOLD_MIN,
        THRESHOLD_MAX,
        THRESHOLD_DFLT,
        PF_Precision_INTEGER,
        0,
        0,
        THRESHOLD_DISK_ID);

    // Add Quality popup for different quality levels
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_Quality_Param_Name),
        QUALITY_NUM_CHOICES,
        QUALITY_DFLT,
        STR(StrID_Quality_Param_Choices),
        QUALITY_DISK_ID);

    out_data->num_params = LITEGLOW_NUM_PARAMS;

    return err;
}

// Sequence data setup and teardown for multi-frame rendering
static PF_Err
SequenceSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // Create handle for sequence data
    PF_Handle sequenceDataH = suites.HandleSuite1()->host_new_handle(sizeof(LiteGlowSequenceData));

    if (!sequenceDataH) {
        return PF_Err_OUT_OF_MEMORY;
    }

    // Initialize sequence data
    LiteGlowSequenceData* sequenceData = (LiteGlowSequenceData*)suites.HandleSuite1()->host_lock_handle(sequenceDataH);
    if (!sequenceData) {
        suites.HandleSuite1()->host_dispose_handle(sequenceDataH);
        return PF_Err_OUT_OF_MEMORY;
    }

    A_long id = ++gSequenceCount;
    sequenceData->sequence_id = id;
    sequenceData->gaussKernelSize = 0;
    sequenceData->kernelRadius = 0;
    sequenceData->quality = QUALITY_MEDIUM;

    suites.HandleSuite1()->host_unlock_handle(sequenceDataH);
    out_data->sequence_data = sequenceDataH;

    return err;
}

static PF_Err
SequenceResetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    return PF_Err_NONE;
}

static PF_Err
SequenceFlatten(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    return PF_Err_NONE;
}

static PF_Err
SequenceSetdown(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    if (in_data->sequence_data) {
        suites.HandleSuite1()->host_dispose_handle(in_data->sequence_data);
        out_data->sequence_data = NULL;
    }

    return err;
}

// Pixel access functions with boundary checking
inline PF_Pixel8* GetPixel8(PF_EffectWorld* world, int x, int y) {
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));
    return (PF_Pixel8*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel8));
}

inline PF_Pixel16* GetPixel16(PF_EffectWorld* world, int x, int y) {
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));
    return (PF_Pixel16*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel16));
}

// Perceptual luminance calculation - sRGB coefficients
inline float PerceivedBrightness8(const PF_Pixel8* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue);
}

inline float PerceivedBrightness16(const PF_Pixel16* p) {
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue);
}

// Edge detection using Sobel operators
inline float EdgeStrength8(PF_EffectWorld* world, int x, int y) {
    const int sobel_x[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1}
    };

    const int sobel_y[3][3] = {
        {-1, -2, -1},
        {0, 0, 0},
        {1, 2, 1}
    };

    float gx = 0.0f, gy = 0.0f;

    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            PF_Pixel8* p = GetPixel8(world, x + i, y + j);
            float brightness = PerceivedBrightness8(p);

            gx += brightness * sobel_x[j + 1][i + 1];
            gy += brightness * sobel_y[j + 1][i + 1];
        }
    }

    return sqrt(gx * gx + gy * gy);
}

inline float EdgeStrength16(PF_EffectWorld* world, int x, int y) {
    const int sobel_x[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1}
    };

    const int sobel_y[3][3] = {
        {-1, -2, -1},
        {0, 0, 0},
        {1, 2, 1}
    };

    float gx = 0.0f, gy = 0.0f;

    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            PF_Pixel16* p = GetPixel16(world, x + i, y + j);
            float brightness = PerceivedBrightness16(p);

            gx += brightness * sobel_x[j + 1][i + 1];
            gy += brightness * sobel_y[j + 1][i + 1];
        }
    }

    return sqrt(gx * gx + gy * gy);
}

// Generate 1D Gaussian kernel
void GenerateGaussianKernel(float sigma, float* kernel, int* radius) {
    *radius = MIN(KERNEL_SIZE_MAX / 2, (int)(3.0f * sigma + 0.5f));

    float sum = 0.0f;

    // Fill kernel with Gaussian values
    for (int i = -(*radius); i <= (*radius); i++) {
        float x = (float)i;
        kernel[i + (*radius)] = exp(-(x * x) / (2.0f * sigma * sigma));
        sum += kernel[i + (*radius)];
    }

    // Normalize kernel
    for (int i = 0; i < 2 * (*radius) + 1; i++) {
        kernel[i] /= sum;
    }
}

// Extract bright areas for glow - 8-bit version
static PF_Err
ExtractBrightAreas8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);

    // Scale strength for more powerful effect
    float strength = 0.0f;
    if (gdata->strength <= 3000.0f) {
        strength = gdata->strength / 1000.0f;
    }
    else {
        float base = 3.0f;
        float excess = (gdata->strength - 3000.0f) / 7000.0f;
        strength = base + (excess * excess * 10.0f);
    }

    float threshold = gdata->threshold / 255.0f;
    float resolution_factor = gdata->resolution_factor;
    PF_EffectWorld* input = gdata->input;

    // Get perceived brightness
    float perceivedBrightness = PerceivedBrightness8(inP) / 255.0f;

    // Edge detection based on resolution factor
    float edgeStrength = 0.0f;
    if (resolution_factor > 0.5f) {
        // Full quality edge detection
        edgeStrength = EdgeStrength8(input, xL, yL) / 255.0f;
    }
    else {
        // Simplified edge detection for preview
        float leftBrightness = PerceivedBrightness8(GetPixel8(input, xL - 1, yL)) / 255.0f;
        float rightBrightness = PerceivedBrightness8(GetPixel8(input, xL + 1, yL)) / 255.0f;
        float topBrightness = PerceivedBrightness8(GetPixel8(input, xL, yL - 1)) / 255.0f;
        float bottomBrightness = PerceivedBrightness8(GetPixel8(input, xL, yL + 1)) / 255.0f;

        float dx = (rightBrightness - leftBrightness) * 0.5f;
        float dy = (bottomBrightness - topBrightness) * 0.5f;

        edgeStrength = sqrtf(dx * dx + dy * dy) * 2.0f;
    }

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    if (intensity > threshold) {
        // Apply threshold with falloff
        glow_amount = MIN(1.0f, (intensity - threshold) / threshold_falloff);

        // Apply strength with enhanced curve
        float power_curve = (strength > 5.0f) ? 0.6f : 0.8f;
        glow_amount = glow_amount * strength;
        glow_amount = powf(glow_amount, power_curve);

        // Preserve original colors with enhanced boost
        outP->red = (A_u_char)MIN(255.0f, inP->red * glow_amount);
        outP->green = (A_u_char)MIN(255.0f, inP->green * glow_amount);
        outP->blue = (A_u_char)MIN(255.0f, inP->blue * glow_amount);

        // Enhanced color boost for high-intensity glows
        float max_component = MAX(MAX(outP->red, outP->green), outP->blue);
        if (max_component > 0) {
            float saturation_boost = 1.2f + (strength * 0.05f);
            saturation_boost = MIN(saturation_boost, 2.5f);

            outP->red = (A_u_char)MIN(255.0f, outP->red * saturation_boost);
            outP->green = (A_u_char)MIN(255.0f, outP->green * saturation_boost);
            outP->blue = (A_u_char)MIN(255.0f, outP->blue * saturation_boost);
        }
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
    }

    // Keep original alpha
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Extract bright areas for glow - 16-bit version
static PF_Err
ExtractBrightAreas16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);

    // Scale strength for more powerful effect
    float strength = 0.0f;
    if (gdata->strength <= 3000.0f) {
        strength = gdata->strength / 1000.0f;
    }
    else {
        float base = 3.0f;
        float excess = (gdata->strength - 3000.0f) / 7000.0f;
        strength = base + (excess * excess * 10.0f);
    }

    float threshold = gdata->threshold / 255.0f;
    float resolution_factor = gdata->resolution_factor;
    PF_EffectWorld* input = gdata->input;

    // Get perceived brightness
    float perceivedBrightness = PerceivedBrightness16(inP) / 32768.0f;

    // Edge detection based on resolution factor
    float edgeStrength = 0.0f;
    if (resolution_factor > 0.5f) {
        // Full quality edge detection
        edgeStrength = EdgeStrength16(input, xL, yL) / 32768.0f;
    }
    else {
        // Simplified edge detection for preview
        float leftBrightness = PerceivedBrightness16(GetPixel16(input, xL - 1, yL)) / 32768.0f;
        float rightBrightness = PerceivedBrightness16(GetPixel16(input, xL + 1, yL)) / 32768.0f;
        float topBrightness = PerceivedBrightness16(GetPixel16(input, xL, yL - 1)) / 32768.0f;
        float bottomBrightness = PerceivedBrightness16(GetPixel16(input, xL, yL + 1)) / 32768.0f;

        float dx = (rightBrightness - leftBrightness) * 0.5f;
        float dy = (bottomBrightness - topBrightness) * 0.5f;

        edgeStrength = sqrtf(dx * dx + dy * dy) * 2.0f;
    }

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    if (intensity > threshold) {
        // Apply threshold with falloff
        glow_amount = MIN(1.0f, (intensity - threshold) / threshold_falloff);

        // Apply strength with enhanced curve
        float power_curve = (strength > 5.0f) ? 0.6f : 0.8f;
        glow_amount = glow_amount * strength;
        glow_amount = powf(glow_amount, power_curve);

        // Preserve original colors with enhanced boost
        outP->red = (A_u_short)MIN(32768.0f, inP->red * glow_amount);
        outP->green = (A_u_short)MIN(32768.0f, inP->green * glow_amount);
        outP->blue = (A_u_short)MIN(32768.0f, inP->blue * glow_amount);

        // Enhanced color boost for high-intensity glows
        float max_component = MAX(MAX(outP->red, outP->green), outP->blue);
        if (max_component > 0) {
            float saturation_boost = 1.2f + (strength * 0.05f);
            saturation_boost = MIN(saturation_boost, 2.5f);

            outP->red = (A_u_short)MIN(32768.0f, outP->red * saturation_boost);
            outP->green = (A_u_short)MIN(32768.0f, outP->green * saturation_boost);
            outP->blue = (A_u_short)MIN(32768.0f, outP->blue * saturation_boost);
        }
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
    }

    // Keep original alpha
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Gaussian blur horizontal pass - 8-bit
static PF_Err
GaussianBlurH8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    float* kernel = bdata->kernel;
    int radius = bdata->radius;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // Apply horizontal convolution with pre-computed kernel
    for (int i = -radius; i <= radius; i++) {
        PF_Pixel8* src = GetPixel8(input, xL + i, yL);
        float weight = kernel[i + radius];

        r += src->red * weight;
        g += src->green * weight;
        b += src->blue * weight;
    }

    // Write blurred result
    outP->red = (A_u_char)MIN(255.0f, MAX(0.0f, r));
    outP->green = (A_u_char)MIN(255.0f, MAX(0.0f, g));
    outP->blue = (A_u_char)MIN(255.0f, MAX(0.0f, b));
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Gaussian blur vertical pass - 8-bit
static PF_Err
GaussianBlurV8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    float* kernel = bdata->kernel;
    int radius = bdata->radius;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // Apply vertical convolution with pre-computed kernel
    for (int j = -radius; j <= radius; j++) {
        PF_Pixel8* src = GetPixel8(input, xL, yL + j);
        float weight = kernel[j + radius];

        r += src->red * weight;
        g += src->green * weight;
        b += src->blue * weight;
    }

    // Write blurred result
    outP->red = (A_u_char)MIN(255.0f, MAX(0.0f, r));
    outP->green = (A_u_char)MIN(255.0f, MAX(0.0f, g));
    outP->blue = (A_u_char)MIN(255.0f, MAX(0.0f, b));
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Gaussian blur horizontal pass - 16-bit
static PF_Err
GaussianBlurH16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    float* kernel = bdata->kernel;
    int radius = bdata->radius;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // Apply horizontal convolution with pre-computed kernel
    for (int i = -radius; i <= radius; i++) {
        PF_Pixel16* src = GetPixel16(input, xL + i, yL);
        float weight = kernel[i + radius];

        r += src->red * weight;
        g += src->green * weight;
        b += src->blue * weight;
    }

    // Write blurred result
    outP->red = (A_u_short)MIN(32768.0f, MAX(0.0f, r));
    outP->green = (A_u_short)MIN(32768.0f, MAX(0.0f, g));
    outP->blue = (A_u_short)MIN(32768.0f, MAX(0.0f, b));
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Gaussian blur vertical pass - 16-bit
static PF_Err
GaussianBlurV16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    float* kernel = bdata->kernel;
    int radius = bdata->radius;

    float r = 0.0f, g = 0.0f, b = 0.0f;

    // Apply vertical convolution with pre-computed kernel
    for (int j = -radius; j <= radius; j++) {
        PF_Pixel16* src = GetPixel16(input, xL, yL + j);
        float weight = kernel[j + radius];

        r += src->red * weight;
        g += src->green * weight;
        b += src->blue * weight;
    }

    // Write blurred result
    outP->red = (A_u_short)MIN(32768.0f, MAX(0.0f, r));
    outP->green = (A_u_short)MIN(32768.0f, MAX(0.0f, g));
    outP->blue = (A_u_short)MIN(32768.0f, MAX(0.0f, b));
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Blend original and glow - 8-bit
static PF_Err
BlendGlow8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlendDataP bdata = reinterpret_cast<BlendDataP>(refcon);
    PF_EffectWorld* glowWorld = bdata->glow;
    int quality = bdata->quality;
    float strength = bdata->strength;

    // Get the glow value for this pixel
    PF_Pixel8* glowP = GetPixel8(glowWorld, xL, yL);

    // Enhanced blending logic
    if (quality == QUALITY_HIGH || strength > 3000.0f) {
        // Screen blend with additional highlight preservation
        float rs = 1.0f - ((1.0f - inP->red / 255.0f) * (1.0f - glowP->red / 255.0f));
        float gs = 1.0f - ((1.0f - inP->green / 255.0f) * (1.0f - glowP->green / 255.0f));
        float bs = 1.0f - ((1.0f - inP->blue / 255.0f) * (1.0f - glowP->blue / 255.0f));

        // Add highlight boost where glow is concentrated
        float glow_intensity = (glowP->red + glowP->green + glowP->blue) / (3.0f * 255.0f);

        // Scale highlight boost with strength
        float highlight_factor = (strength > 3000.0f) ?
            0.2f + ((strength - 3000.0f) / 7000.0f) * 0.4f : 0.2f;

        float highlight_boost = 1.0f + glow_intensity * highlight_factor;

        // Apply final blend with highlight boost
        outP->red = (A_u_char)MIN(255.0f, rs * 255.0f * highlight_boost);
        outP->green = (A_u_char)MIN(255.0f, gs * 255.0f * highlight_boost);
        outP->blue = (A_u_char)MIN(255.0f, bs * 255.0f * highlight_boost);

        // For extreme high strength (> 7000), add extra glow intensity boost
        if (strength > 7000.0f) {
            float extreme_boost = (strength - 7000.0f) / 3000.0f * 0.5f;
            outP->red = (A_u_char)MIN(255.0f, outP->red * (1.0f + extreme_boost));
            outP->green = (A_u_char)MIN(255.0f, outP->green * (1.0f + extreme_boost));
            outP->blue = (A_u_char)MIN(255.0f, outP->blue * (1.0f + extreme_boost));
        }
    }
    else {
        // Standard screen blend for medium/low quality
        outP->red = (A_u_char)MIN(255, inP->red + glowP->red - ((inP->red * glowP->red) >> 8));
        outP->green = (A_u_char)MIN(255, inP->green + glowP->green - ((inP->green * glowP->green) >> 8));
        outP->blue = (A_u_char)MIN(255, inP->blue + glowP->blue - ((inP->blue * glowP->blue) >> 8));
    }

    // Keep original alpha
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Blend original and glow - 16-bit
static PF_Err
BlendGlow16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlendDataP bdata = reinterpret_cast<BlendDataP>(refcon);
    PF_EffectWorld* glowWorld = bdata->glow;
    int quality = bdata->quality;
    float strength = bdata->strength;

    // Get the glow value for this pixel
    PF_Pixel16* glowP = GetPixel16(glowWorld, xL, yL);

    // Enhanced blending logic
    if (quality == QUALITY_HIGH || strength > 3000.0f) {
        // Screen blend with additional highlight preservation
        float rs = 1.0f - ((1.0f - inP->red / 32768.0f) * (1.0f - glowP->red / 32768.0f));
        float gs = 1.0f - ((1.0f - inP->green / 32768.0f) * (1.0f - glowP->green / 32768.0f));
        float bs = 1.0f - ((1.0f - inP->blue / 32768.0f) * (1.0f - glowP->blue / 32768.0f));

        // Add highlight boost where glow is concentrated
        float glow_intensity = (glowP->red + glowP->green + glowP->blue) / (3.0f * 32768.0f);

        // Scale highlight boost with strength
        float highlight_factor = (strength > 3000.0f) ?
            0.2f + ((strength - 3000.0f) / 7000.0f) * 0.4f : 0.2f;

        float highlight_boost = 1.0f + glow_intensity * highlight_factor;

        // Apply final blend with highlight boost
        outP->red = (A_u_short)MIN(32768.0f, rs * 32768.0f * highlight_boost);
        outP->green = (A_u_short)MIN(32768.0f, gs * 32768.0f * highlight_boost);
        outP->blue = (A_u_short)MIN(32768.0f, bs * 32768.0f * highlight_boost);

        // For extreme high strength (> 7000), add extra glow intensity boost
        if (strength > 7000.0f) {
            float extreme_boost = (strength - 7000.0f) / 3000.0f * 0.5f;
            outP->red = (A_u_short)MIN(32768.0f, outP->red * (1.0f + extreme_boost));
            outP->green = (A_u_short)MIN(32768.0f, outP->green * (1.0f + extreme_boost));
            outP->blue = (A_u_short)MIN(32768.0f, outP->blue * (1.0f + extreme_boost));
        }
    }
    else {
        // Standard screen blend for medium/low quality
        outP->red = (A_u_short)MIN(32768.0f, inP->red + glowP->red - ((inP->red * glowP->red) / 32768));
        outP->green = (A_u_short)MIN(32768.0f, inP->green + glowP->green - ((inP->green * glowP->green) / 32768));
        outP->blue = (A_u_short)MIN(32768.0f, inP->blue + glowP->blue - ((inP->blue * glowP->blue) / 32768));
    }

    // Keep original alpha
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

static PF_Err
Render(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    A_long linesL = output->height;
    PF_LayerDef* inputP = &params[LITEGLOW_INPUT]->u.ld;

    // If strength is zero (or near zero), just copy the input to output
    float strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    if (strength <= 0.1f) {
        err = PF_COPY(inputP, output, NULL, NULL);
        return err;
    }

    // Get user parameters
    float radius_param = params[LITEGLOW_RADIUS]->u.fs_d.value;
    float threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value;
    int quality = params[LITEGLOW_QUALITY]->u.pd.value;

    // Handle downsampling for preview
    float downscale_x = static_cast<float>(in_data->downsample_x.den) / static_cast<float>(in_data->downsample_x.num);
    float downscale_y = static_cast<float>(in_data->downsample_y.den) / static_cast<float>(in_data->downsample_y.num);
    float resolution_factor = MIN(downscale_x, downscale_y);

    // Adjust radius based on downsampling
    float adjusted_radius = radius_param;
    if (resolution_factor < 0.9f) {
        // Scale down the radius for previews to improve performance
        adjusted_radius = radius_param * MAX(0.5f, resolution_factor);
    }

    // Create temporary buffers for processing
    PF_EffectWorld bright_world, blur_h_world, blur_v_world;

    // Create temporary worlds
    ERR(suites.WorldSuite1()->new_world(in_data->effect_ref,
        output->width,
        output->height,
        0, // No flags
        &bright_world));

    ERR(suites.WorldSuite1()->new_world(in_data->effect_ref,
        output->width,
        output->height,
        0, // No flags
        &blur_h_world));

    ERR(suites.WorldSuite1()->new_world(in_data->effect_ref,
        output->width,
        output->height,
        0, // No flags
        &blur_v_world));

    if (!err) {
        // Create glow parameters
        GlowData gdata;
        gdata.strength = strength;
        gdata.threshold = threshold;
        gdata.input = inputP;
        gdata.resolution_factor = resolution_factor;

        // STEP 1: Extract bright areas with edge detection
        if (PF_WORLD_IS_DEEP(output)) {
            ERR(suites.Iterate16Suite2()->iterate(in_data,
                0,                // progress base
                linesL,           // progress final
                inputP,           // src 
                NULL,             // area - null for all pixels
                (void*)&gdata,    // refcon with parameters
                ExtractBrightAreas16, // pixel function
                &bright_world));  // destination
        }
        else {
            ERR(suites.Iterate8Suite2()->iterate(in_data,
                0,                // progress base
                linesL,           // progress final
                inputP,           // src 
                NULL,             // area - null for all pixels
                (void*)&gdata,    // refcon with parameters
                ExtractBrightAreas8,  // pixel function
                &bright_world));  // destination
        }

        if (!err) {
            // Calculate blur parameters based on quality setting
            float sigma;
            switch (quality) {
            case QUALITY_LOW:
                sigma = adjusted_radius * 0.5f;
                break;
            case QUALITY_MEDIUM:
                sigma = adjusted_radius * 0.75f;
                break;
            case QUALITY_HIGH:
            default:
                sigma = adjusted_radius;
                break;
            }

            // Generate Gaussian kernel or use cached one if available
            int kernel_radius;
            float kernel[KERNEL_SIZE_MAX * 2 + 1];

            // Check if we can use a cached kernel from sequence data
            LiteGlowSequenceData* seq_data = NULL;
            if (in_data->sequence_data) {
                seq_data = (LiteGlowSequenceData*)suites.HandleSuite1()->host_lock_handle(in_data->sequence_data);
            }

            if (seq_data && seq_data->gaussKernelSize > 0 &&
                fabsf(sigma - seq_data->sigma) < 0.01f) {
                // Use cached kernel
                kernel_radius = seq_data->kernelRadius;
                memcpy(kernel, seq_data->gaussKernel, seq_data->gaussKernelSize * sizeof(float));
            }
            else {
                // Generate new kernel
                GenerateGaussianKernel(sigma, kernel, &kernel_radius);

                // Cache the kernel if we have sequence data
                if (seq_data) {
                    seq_data->kernelRadius = kernel_radius;
                    seq_data->sigma = sigma;
                    seq_data->gaussKernelSize = 2 * kernel_radius + 1;
                    memcpy(seq_data->gaussKernel, kernel, seq_data->gaussKernelSize * sizeof(float));
                }
            }

            // Unlock sequence data if we locked it
            if (seq_data) {
                suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
            }

            // Create blur data with kernel
            BlurData bdata;
            bdata.input = &bright_world;
            bdata.radius = kernel_radius;
            bdata.kernel = kernel;

            // STEP 2: Apply horizontal Gaussian blur
            if (PF_WORLD_IS_DEEP(output)) {
                ERR(suites.Iterate16Suite2()->iterate(in_data,
                    0,                // progress base
                    linesL,           // progress final
                    &bright_world,    // src 
                    NULL,             // area - null for all pixels
                    (void*)&bdata,    // refcon with blur data
                    GaussianBlurH16,  // pixel function
                    &blur_h_world));  // destination
            }
            else {
                ERR(suites.Iterate8Suite2()->iterate(in_data,
                    0,                // progress base
                    linesL,           // progress final
                    &bright_world,    // src 
                    NULL,             // area - null for all pixels
                    (void*)&bdata,    // refcon with blur data
                    GaussianBlurH8,   // pixel function
                    &blur_h_world));  // destination
            }

            if (!err) {
                // Update blur data for vertical pass
                bdata.input = &blur_h_world;

                // STEP 3: Apply vertical Gaussian blur
                if (PF_WORLD_IS_DEEP(output)) {
                    ERR(suites.Iterate16Suite2()->iterate(in_data,
                        0,               // progress base
                        linesL,          // progress final
                        &blur_h_world,   // src 
                        NULL,            // area - null for all pixels
                        (void*)&bdata,   // refcon with blur data
                        GaussianBlurV16, // pixel function
                        &blur_v_world)); // destination
                }
                else {
                    ERR(suites.Iterate8Suite2()->iterate(in_data,
                        0,               // progress base
                        linesL,          // progress final
                        &blur_h_world,   // src 
                        NULL,            // area - null for all pixels
                        (void*)&bdata,   // refcon with blur data
                        GaussianBlurV8,  // pixel function
                        &blur_v_world)); // destination
                }

                // For high quality, apply a second blur pass when not in preview mode
                if (quality == QUALITY_HIGH && !err && strength > 500.0f && resolution_factor > 0.9f) {
                    // Second horizontal blur
                    bdata.input = &blur_v_world;

                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            &blur_v_world,   // src 
                            NULL,            // area - null for all pixels
                            (void*)&bdata,   // refcon with blur data
                            GaussianBlurH16, // pixel function
                            &bright_world)); // destination (reuse)
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            &blur_v_world,   // src 
                            NULL,            // area - null for all pixels
                            (void*)&bdata,   // refcon with blur data
                            GaussianBlurH8,  // pixel function
                            &bright_world)); // destination (reuse)
                    }

                    if (!err) {
                        // Second vertical blur
                        bdata.input = &bright_world;

                        if (PF_WORLD_IS_DEEP(output)) {
                            ERR(suites.Iterate16Suite2()->iterate(in_data,
                                0,               // progress base
                                linesL,          // progress final
                                &bright_world,   // src 
                                NULL,            // area - null for all pixels
                                (void*)&bdata,   // refcon with blur data
                                GaussianBlurV16, // pixel function
                                &blur_v_world)); // destination
                        }
                        else {
                            ERR(suites.Iterate8Suite2()->iterate(in_data,
                                0,               // progress base
                                linesL,          // progress final
                                &bright_world,   // src 
                                NULL,            // area - null for all pixels
                                (void*)&bdata,   // refcon with blur data
                                GaussianBlurV8,  // pixel function
                                &blur_v_world)); // destination
                        }
                    }
                }

                if (!err) {
                    // STEP 4: Blend original and glow
                    BlendData blend_data;
                    blend_data.glow = &blur_v_world;
                    blend_data.quality = quality;
                    blend_data.strength = strength;

                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            inputP,          // src (original)
                            NULL,            // area - null for all pixels
                            (void*)&blend_data, // refcon - blend parameters
                            BlendGlow16,     // pixel function
                            output));        // destination
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            inputP,          // src (original)
                            NULL,            // area - null for all pixels
                            (void*)&blend_data, // refcon - blend parameters
                            BlendGlow8,      // pixel function
                            output));        // destination
                    }
                }
            }
        }

        // Dispose of temporary worlds
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &bright_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_h_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_v_world));
    }

    return err;
}

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
        "LiteGlow",          // Name
        "ADBE LiteGlow",     // Match Name
        "LiteGlow",          // Category
        AE_RESERVED_INFO,    // Reserved Info
        "EffectMain",        // Entry point
        "https://www.adobe.com"); // Support URL

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

        case PF_Cmd_SEQUENCE_SETUP:
            err = SequenceSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_SEQUENCE_RESETUP:
            err = SequenceResetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_SEQUENCE_FLATTEN:
            err = SequenceFlatten(in_data, out_data, params, output);
            break;

        case PF_Cmd_SEQUENCE_SETDOWN:
            err = SequenceSetdown(in_data, out_data, params, output);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;

        default:
            break;
        }
    }
    catch (PF_Err& thrown_err) {
        err = thrown_err;
    }
    return err;
}