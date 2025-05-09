/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2025 Adobe Inc.                                  */
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

    An enhanced glow effect with true Gaussian blur, advanced edge detection,
    and improved color handling for creating realistic luminescence effects.

    Revision History

    Version		Change													Engineer	Date
    =======		======													========	======
    1.0			First implementation										Dev         5/8/2025
    1.1         Enhanced with true Gaussian blur and edge detection      Dev         5/9/2025

*/

#include "LiteGlow.h"
#include <math.h>

// Constants for Gaussian kernel generation
#define PI 3.14159265358979323846
#define KERNEL_SIZE_MAX 64

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

    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;	// 16bpc support

    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err		err = PF_Err_NONE;
    PF_ParamDef	def;

    AEFX_CLR_STRUCT(def);

    // Add Strength slider parameter with expanded range (0-3000)
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Strength_Param_Name),
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_DFLT,
        PF_Precision_HUNDREDTHS,
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

// Get pixel from buffer with boundary checking - 8-bit version
inline PF_Pixel8* GetPixel8(PF_EffectWorld* world, int x, int y) {
    // Clamp coordinates to valid range
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));

    // Get pixel pointer
    return (PF_Pixel8*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel8));
}

// Get pixel from buffer with boundary checking - 16-bit version
inline PF_Pixel16* GetPixel16(PF_EffectWorld* world, int x, int y) {
    // Clamp coordinates to valid range
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));

    // Get pixel pointer
    return (PF_Pixel16*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel16));
}

// Calculate perceptual luminance with more accurate coefficients - 8-bit
inline float PerceivedBrightness8(const PF_Pixel8* p) {
    // sRGB luminance factors for more accurate perceptual brightness
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue);
}

// Calculate perceptual luminance with more accurate coefficients - 16-bit
inline float PerceivedBrightness16(const PF_Pixel16* p) {
    // sRGB luminance factors for more accurate perceptual brightness
    return (0.2126f * p->red + 0.7152f * p->green + 0.0722f * p->blue);
}

// Calculate edge strength using Sobel operators - 8-bit
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

    // Apply Sobel operators
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            PF_Pixel8* p = GetPixel8(world, x + i, y + j);
            float brightness = PerceivedBrightness8(p);

            gx += brightness * sobel_x[j + 1][i + 1];
            gy += brightness * sobel_y[j + 1][i + 1];
        }
    }

    // Calculate gradient magnitude
    return sqrt(gx * gx + gy * gy);
}

// Calculate edge strength using Sobel operators - 16-bit
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

    // Apply Sobel operators
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            PF_Pixel16* p = GetPixel16(world, x + i, y + j);
            float brightness = PerceivedBrightness16(p);

            gx += brightness * sobel_x[j + 1][i + 1];
            gy += brightness * sobel_y[j + 1][i + 1];
        }
    }

    // Calculate gradient magnitude
    return sqrt(gx * gx + gy * gy);
}

// Generate 1D Gaussian kernel
void GenerateGaussianKernel(float sigma, float* kernel, int* radius) {
    // Calculate radius based on sigma (3*sigma covers 99.7% of the distribution)
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

// Extract bright areas with edge enhancement - 8-bit
static PF_Err
ExtractBrightAreas8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);
    float strength = gdata->strength / 1000.0f; // Normalize strength
    float threshold = gdata->threshold / 255.0f;

    PF_EffectWorld* input = gdata->input;

    // Get perceived brightness
    float perceivedBrightness = PerceivedBrightness8(inP) / 255.0f;

    // Detect edges using Sobel for additional glow emphasis
    float edgeStrength = EdgeStrength8(input, xL, yL) / 255.0f;

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    if (intensity > threshold) {
        // Smooth falloff rather than hard cutoff
        glow_amount = MIN(1.0f, (intensity - threshold) / threshold_falloff);
        glow_amount = glow_amount * strength;

        // Apply curve for more pleasing glow falloff
        glow_amount = powf(glow_amount, 0.8f);

        // Preserve original colors
        outP->red = (A_u_char)MIN(255.0f, inP->red * glow_amount);
        outP->green = (A_u_char)MIN(255.0f, inP->green * glow_amount);
        outP->blue = (A_u_char)MIN(255.0f, inP->blue * glow_amount);

        // Boost highly saturated colors
        float max_component = MAX(MAX(outP->red, outP->green), outP->blue);
        if (max_component > 0) {
            float saturation_boost = 1.2f;
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

// Extract bright areas with edge enhancement - 16-bit
static PF_Err
ExtractBrightAreas16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);
    float strength = gdata->strength / 1000.0f; // Normalize strength
    float threshold = gdata->threshold / 255.0f;

    PF_EffectWorld* input = gdata->input;

    // Get perceived brightness
    float perceivedBrightness = PerceivedBrightness16(inP) / 32768.0f;

    // Detect edges using Sobel for additional glow emphasis
    float edgeStrength = EdgeStrength16(input, xL, yL) / 32768.0f;

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    if (intensity > threshold) {
        // Smooth falloff rather than hard cutoff
        glow_amount = MIN(1.0f, (intensity - threshold) / threshold_falloff);
        glow_amount = glow_amount * strength;

        // Apply curve for more pleasing glow falloff
        glow_amount = powf(glow_amount, 0.8f);

        // Preserve original colors
        outP->red = (A_u_short)MIN(32768.0f, inP->red * glow_amount);
        outP->green = (A_u_short)MIN(32768.0f, inP->green * glow_amount);
        outP->blue = (A_u_short)MIN(32768.0f, inP->blue * glow_amount);

        // Boost highly saturated colors
        float max_component = MAX(MAX(outP->red, outP->green), outP->blue);
        if (max_component > 0) {
            float saturation_boost = 1.2f;
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

// Advanced blend mode for glow - 8-bit
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

    // Get the glow value for this pixel
    PF_Pixel8* glowP = GetPixel8(glowWorld, xL, yL);

    if (quality == QUALITY_HIGH) {
        // Screen blend with additional highlight preservation
        // 1 - (1-a)(1-b) formula with enhanced highlight handling
        float rs = 1.0f - ((1.0f - inP->red / 255.0f) * (1.0f - glowP->red / 255.0f));
        float gs = 1.0f - ((1.0f - inP->green / 255.0f) * (1.0f - glowP->green / 255.0f));
        float bs = 1.0f - ((1.0f - inP->blue / 255.0f) * (1.0f - glowP->blue / 255.0f));

        // Add highlight boost where glow is concentrated
        float glow_intensity = (glowP->red + glowP->green + glowP->blue) / (3.0f * 255.0f);
        float highlight_boost = 1.0f + glow_intensity * 0.2f;

        // Apply final blend with highlight boost
        outP->red = (A_u_char)MIN(255.0f, rs * 255.0f * highlight_boost);
        outP->green = (A_u_char)MIN(255.0f, gs * 255.0f * highlight_boost);
        outP->blue = (A_u_char)MIN(255.0f, bs * 255.0f * highlight_boost);
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

// Advanced blend mode for glow - 16-bit
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

    // Get the glow value for this pixel
    PF_Pixel16* glowP = GetPixel16(glowWorld, xL, yL);

    if (quality == QUALITY_HIGH) {
        // Screen blend with additional highlight preservation
        // 1 - (1-a)(1-b) formula with enhanced highlight handling
        float rs = 1.0f - ((1.0f - inP->red / 32768.0f) * (1.0f - glowP->red / 32768.0f));
        float gs = 1.0f - ((1.0f - inP->green / 32768.0f) * (1.0f - glowP->green / 32768.0f));
        float bs = 1.0f - ((1.0f - inP->blue / 32768.0f) * (1.0f - glowP->blue / 32768.0f));

        // Add highlight boost where glow is concentrated
        float glow_intensity = (glowP->red + glowP->green + glowP->blue) / (3.0f * 32768.0f);
        float highlight_boost = 1.0f + glow_intensity * 0.2f;

        // Apply final blend with highlight boost
        outP->red = (A_u_short)MIN(32768.0f, rs * 32768.0f * highlight_boost);
        outP->green = (A_u_short)MIN(32768.0f, gs * 32768.0f * highlight_boost);
        outP->blue = (A_u_short)MIN(32768.0f, bs * 32768.0f * highlight_boost);
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

    // If strength is zero (or near zero), just copy the input to output
    float strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    if (strength <= 0.1f) {
        err = PF_COPY(&params[LITEGLOW_INPUT]->u.ld, output, NULL, NULL);
        return err;
    }

    // Get user parameters
    float radius_param = params[LITEGLOW_RADIUS]->u.fs_d.value;
    float threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value;
    int quality = params[LITEGLOW_QUALITY]->u.pd.value;

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
        gdata.input = &params[LITEGLOW_INPUT]->u.ld;

        // STEP 1: Extract bright areas with edge detection
        if (PF_WORLD_IS_DEEP(output)) {
            ERR(suites.Iterate16Suite2()->iterate(in_data,
                0,                            // progress base
                linesL,                       // progress final
                &params[LITEGLOW_INPUT]->u.ld, // src 
                NULL,                         // area - null for all pixels
                (void*)&gdata,                // refcon with parameters
                ExtractBrightAreas16,         // pixel function
                &bright_world));              // destination
        }
        else {
            ERR(suites.Iterate8Suite2()->iterate(in_data,
                0,                            // progress base
                linesL,                       // progress final
                &params[LITEGLOW_INPUT]->u.ld, // src 
                NULL,                         // area - null for all pixels
                (void*)&gdata,                // refcon with parameters
                ExtractBrightAreas8,          // pixel function
                &bright_world));              // destination
        }

        if (!err) {
            // Calculate blur parameters based on quality setting
            float sigma;
            int kernel_radius;
            float kernel[KERNEL_SIZE_MAX * 2 + 1]; // Static array instead of std::vector

            // Adjust sigma based on quality and radius parameter
            switch (quality) {
            case QUALITY_LOW:
                sigma = radius_param * 0.5f;
                break;
            case QUALITY_MEDIUM:
                sigma = radius_param * 0.75f;
                break;
            case QUALITY_HIGH:
            default:
                sigma = radius_param;
                break;
            }

            // Generate Gaussian kernel
            GenerateGaussianKernel(sigma, kernel, &kernel_radius);

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

                // For high quality, apply a second blur pass
                if (quality == QUALITY_HIGH && !err && strength > 500.0f) {
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

                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,                            // progress base
                            linesL,                       // progress final
                            &params[LITEGLOW_INPUT]->u.ld, // src (original)
                            NULL,                         // area - null for all pixels
                            (void*)&blend_data,           // refcon - blend parameters
                            BlendGlow16,                  // pixel function
                            output));                     // destination
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,                            // progress base
                            linesL,                       // progress final
                            &params[LITEGLOW_INPUT]->u.ld, // src (original)
                            NULL,                         // area - null for all pixels
                            (void*)&blend_data,           // refcon - blend parameters
                            BlendGlow8,                   // pixel function
                            output));                     // destination
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
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "LiteGlow", // Name
        "ADBE LiteGlow", // Match Name
        "LiteGlow", // Category
        AE_RESERVED_INFO, // Reserved Info
        "EffectMain",	// Entry point
        "https://www.adobe.com");	// support URL

    return result;
}


PF_Err
EffectMain(
    PF_Cmd			cmd,
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    void* extra)
{
    PF_Err		err = PF_Err_NONE;

    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            err = About(in_data,
                out_data,
                params,
                output);
            break;

        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data,
                out_data,
                params,
                output);
            break;

        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data,
                out_data,
                params,
                output);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data,
                out_data,
                params,
                output);
            break;
        }
    }
    catch (PF_Err& thrown_err) {
        err = thrown_err;
    }
    return err;
}