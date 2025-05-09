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
    Optimized with multi-frame rendering support and preview downsampling.

    Revision History

    Version		Change													Engineer	Date
    =======		======													========	======
    1.0			First implementation										Dev         5/8/2025
    1.1         Enhanced with true Gaussian blur and edge detection      Dev         5/9/2025
    1.2         Added multi-frame rendering and performance optimizations Dev         5/10/2025
    1.3         Modified to extend glow effect outside layer boundaries   Dev         5/11/2025
    1.4         Increased max effect size, added blend ratio parameter    Dev         5/12/2025
    1.5         Fixed transparency issues and resolution dependence       Dev         5/13/2025

*/

#include "LiteGlow.h"
#include <math.h>

// Constants for Gaussian kernel generation
#define PI 3.14159265358979323846
#define KERNEL_SIZE_MAX 128

// Our sequence data counter
static A_long gSequenceCount = 0;

// Define ERR2 macro - similar to ERR but doesn't stop execution
#define ERR2(x) (error = (x) || error)

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
    PF_Err err = PF_Err_NONE;

    out_data->my_version = PF_VERSION(MAJOR_VERSION,
        MINOR_VERSION,
        BUG_VERSION,
        STAGE_VERSION,
        BUILD_VERSION);

    // Set up flags for optimizations and buffer expansion
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;        // 16bpc support
    out_data->out_flags |= PF_OutFlag_PIX_INDEPENDENT;        // Enable parallel processing
    out_data->out_flags |= PF_OutFlag_SEND_UPDATE_PARAMS_UI;  // Update UI during processing
    out_data->out_flags |= PF_OutFlag_I_EXPAND_BUFFER;        // Enable buffer expansion for glow effect
    out_data->out_flags |= PF_OutFlag_USE_OUTPUT_EXTENT;      // Tell AE we use the extent hint
    out_data->out_flags |= PF_OutFlag_WIDE_TIME_INPUT;        // More accurate time handling

    // Enable Multi-Frame Rendering support if available
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    return err;
}

static PF_Err
ParamsSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef	def;

    AEFX_CLR_STRUCT(def);

    // Add Strength slider parameter with expanded range (0-5000)
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

    // Add Blend Ratio slider for overlay control
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_Blend_Param_Name),
        BLEND_MIN,
        BLEND_MAX,
        BLEND_MIN,
        BLEND_MAX,
        BLEND_DFLT,
        PF_Precision_INTEGER,
        0,
        0,
        BLEND_DISK_ID);

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

    // Get pointer to sequence data
    LiteGlowSequenceData* sequenceData = (LiteGlowSequenceData*)suites.HandleSuite1()->host_lock_handle(sequenceDataH);
    if (!sequenceData) {
        suites.HandleSuite1()->host_dispose_handle(sequenceDataH);
        return PF_Err_OUT_OF_MEMORY;
    }

    // Initialize sequence data
    A_long id = ++gSequenceCount;
    sequenceData->sequence_id = id;
    sequenceData->gaussKernelSize = 0;
    sequenceData->kernelRadius = 0;
    sequenceData->quality = QUALITY_MEDIUM;

    // Unlock the handle
    suites.HandleSuite1()->host_unlock_handle(sequenceDataH);

    // Store handle in sequence data
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
    PF_Err err = PF_Err_NONE;

    // Nothing specific needed here as our sequence data is simple
    return err;
}

static PF_Err
SequenceFlatten(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    // No special flattening needed, our data structure is simple
    return err;
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

// Add this new function to handle parameter changes
static PF_Err
HandleChangedParam(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output,
    PF_UserChangedParamExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    // Recalculate buffer expansion based on radius
    if (extra->param_index == LITEGLOW_RADIUS ||
        extra->param_index == LITEGLOW_STRENGTH ||
        extra->param_index == LITEGLOW_BLEND) {
        // Notify AE that the effect's rendered result depends on these parameters
        out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
    }

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

// Enhanced ExtractBrightAreas8 function with improved edge detection and transparency handling
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
    float resolution_factor = gdata->resolution_factor;

    PF_EffectWorld* input = gdata->input;

    // For all processing, we look at the input pixel's alpha to determine if it's part of the content
    bool is_transparent = (inP->alpha < 5); // Consider nearly fully transparent
    bool is_edge_pixel = false;

    // For edge detection, check if this is a semi-transparent pixel at the edge of content
    if (inP->alpha >= 5 && inP->alpha < 250) {
        is_edge_pixel = true;
    }

    // Check surrounding pixels - if we're near visible content, treat as edge 
    if (is_transparent) {
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0) continue;

                PF_Pixel8* neighborP = GetPixel8(input, xL + dx, yL + dy);
                if (neighborP->alpha > 5) {
                    is_edge_pixel = true;
                    break;
                }
            }
            if (is_edge_pixel) break;
        }
    }

    // Get perceived brightness - include consideration for transparent areas
    float perceivedBrightness;
    if (is_transparent) {
        // For transparent areas, check nearby pixels for glow source
        // This helps extend glow beyond the visible boundaries
        // Check a larger neighborhood for better edge detection (5x5 instead of 3x3)
        float maxNeighborBrightness = 0.0f;
        float distanceFactor = 0.0f;

        // Check in a 5x5 area
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0) continue;

                PF_Pixel8* neighborP = GetPixel8(input, xL + dx, yL + dy);
                if (neighborP->alpha > 5) {
                    // Calculate distance-based weight (closer pixels matter more)
                    float distance = sqrtf(dx * dx + dy * dy);
                    float weight = 1.0f / (1.0f + distance * 0.8f);

                    float neighborBrightness = PerceivedBrightness8(neighborP) / 255.0f * weight;
                    if (neighborBrightness > maxNeighborBrightness) {
                        maxNeighborBrightness = neighborBrightness;
                        distanceFactor = weight;
                    }
                }
            }
        }

        // Apply distance-based attenuation
        perceivedBrightness = maxNeighborBrightness * 0.85f; // Slightly reduce brightness for transparent areas
    }
    else {
        perceivedBrightness = PerceivedBrightness8(inP) / 255.0f;
    }

    // For preview modes, simplify edge detection based on resolution factor
    float edgeStrength = 0.0f;
    if (!is_transparent && resolution_factor > 0.5f) {
        // Full quality edge detection for high-res rendering
        edgeStrength = EdgeStrength8(input, xL, yL) / 255.0f;
    }
    else if (!is_transparent) {
        // Simplified edge detection for preview - just use brightness contrast
        float leftBrightness = PerceivedBrightness8(GetPixel8(input, xL - 1, yL)) / 255.0f;
        float rightBrightness = PerceivedBrightness8(GetPixel8(input, xL + 1, yL)) / 255.0f;
        float topBrightness = PerceivedBrightness8(GetPixel8(input, xL, yL - 1)) / 255.0f;
        float bottomBrightness = PerceivedBrightness8(GetPixel8(input, xL, yL + 1)) / 255.0f;

        float dx = (rightBrightness - leftBrightness) * 0.5f;
        float dy = (bottomBrightness - topBrightness) * 0.5f;

        edgeStrength = sqrtf(dx * dx + dy * dy) * 2.0f; // Scale up to match full Sobel
    }

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Boost edge pixels
    if (is_edge_pixel) {
        intensity = MAX(intensity, 0.5f);
    }

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    // For transparent areas near visible content, use a lower threshold
    float effective_threshold = is_transparent ? threshold * 0.35f : threshold;

    if (intensity > effective_threshold) {
        // Smooth falloff rather than hard cutoff
        glow_amount = MIN(1.0f, (intensity - effective_threshold) / threshold_falloff);
        glow_amount = glow_amount * strength;

        // Apply curve for more pleasing glow falloff
        glow_amount = powf(glow_amount, 0.8f);

        if (is_transparent) {
            // For transparent areas, derive color from nearby visible pixels
            PF_Pixel8* sourceP = NULL;
            float maxAlpha = 0;

            // Find the most opaque neighboring pixel to use as color source
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;

                    PF_Pixel8* neighborP = GetPixel8(input, xL + dx, yL + dy);
                    if (neighborP->alpha > maxAlpha) {
                        maxAlpha = neighborP->alpha;
                        sourceP = neighborP;
                    }
                }
            }

            if (sourceP && maxAlpha > 5) {
                // Use the color from the most opaque neighbor
                outP->red = (A_u_char)MIN(255.0f, sourceP->red * glow_amount);
                outP->green = (A_u_char)MIN(255.0f, sourceP->green * glow_amount);
                outP->blue = (A_u_char)MIN(255.0f, sourceP->blue * glow_amount);
            }
            else {
                // Fallback to original color with reduced opacity
                outP->red = (A_u_char)MIN(255.0f, inP->red * glow_amount);
                outP->green = (A_u_char)MIN(255.0f, inP->green * glow_amount);
                outP->blue = (A_u_char)MIN(255.0f, inP->blue * glow_amount);
            }
        }
        else {
            // Preserve original colors for non-transparent areas
            outP->red = (A_u_char)MIN(255.0f, inP->red * glow_amount);
            outP->green = (A_u_char)MIN(255.0f, inP->green * glow_amount);
            outP->blue = (A_u_char)MIN(255.0f, inP->blue * glow_amount);
        }

        // Set alpha for the glow - keep original alpha for source pixels,
        // but use calculated glow amount for transparent areas
        outP->alpha = is_transparent ? (A_u_char)MIN(255.0f, glow_amount * 255.0f) : inP->alpha;
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
        outP->alpha = 0; // Make fully transparent
    }

    return PF_Err_NONE;
}

// Enhanced ExtractBrightAreas16 function with improved edge detection and transparency handling
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
    float resolution_factor = gdata->resolution_factor;

    PF_EffectWorld* input = gdata->input;

    // For all processing, we look at the input pixel's alpha to determine if it's part of the content
    bool is_transparent = (inP->alpha < 1024); // Consider nearly fully transparent for 16-bit
    bool is_edge_pixel = false;

    // For edge detection, check if this is a semi-transparent pixel at the edge of content
    if (inP->alpha >= 1024 && inP->alpha < 30000) {
        is_edge_pixel = true;
    }

    // Check surrounding pixels - if we're near visible content, treat as edge 
    if (is_transparent) {
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0) continue;

                PF_Pixel16* neighborP = GetPixel16(input, xL + dx, yL + dy);
                if (neighborP->alpha > 1024) {
                    is_edge_pixel = true;
                    break;
                }
            }
            if (is_edge_pixel) break;
        }
    }

    // Get perceived brightness - include consideration for transparent areas
    float perceivedBrightness;
    if (is_transparent) {
        // For transparent areas, check nearby pixels for glow source
        // This helps extend glow beyond the visible boundaries
        // Check a larger neighborhood for better edge detection (5x5 instead of 3x3)
        float maxNeighborBrightness = 0.0f;
        float distanceFactor = 0.0f;

        // Check in a 5x5 area
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0) continue;

                PF_Pixel16* neighborP = GetPixel16(input, xL + dx, yL + dy);
                if (neighborP->alpha > 1024) {
                    // Calculate distance-based weight (closer pixels matter more)
                    float distance = sqrtf(dx * dx + dy * dy);
                    float weight = 1.0f / (1.0f + distance * 0.8f);

                    float neighborBrightness = PerceivedBrightness16(neighborP) / 32768.0f * weight;
                    if (neighborBrightness > maxNeighborBrightness) {
                        maxNeighborBrightness = neighborBrightness;
                        distanceFactor = weight;
                    }
                }
            }
        }

        // Apply distance-based attenuation
        perceivedBrightness = maxNeighborBrightness * 0.85f; // Slightly reduce brightness for transparent areas
    }
    else {
        perceivedBrightness = PerceivedBrightness16(inP) / 32768.0f;
    }

    // For preview modes, simplify edge detection based on resolution factor
    float edgeStrength = 0.0f;
    if (!is_transparent && resolution_factor > 0.5f) {
        // Full quality edge detection for high-res rendering
        edgeStrength = EdgeStrength16(input, xL, yL) / 32768.0f;
    }
    else if (!is_transparent) {
        // Simplified edge detection for preview - just use brightness contrast
        float leftBrightness = PerceivedBrightness16(GetPixel16(input, xL - 1, yL)) / 32768.0f;
        float rightBrightness = PerceivedBrightness16(GetPixel16(input, xL + 1, yL)) / 32768.0f;
        float topBrightness = PerceivedBrightness16(GetPixel16(input, xL, yL - 1)) / 32768.0f;
        float bottomBrightness = PerceivedBrightness16(GetPixel16(input, xL, yL + 1)) / 32768.0f;

        float dx = (rightBrightness - leftBrightness) * 0.5f;
        float dy = (bottomBrightness - topBrightness) * 0.5f;

        edgeStrength = sqrtf(dx * dx + dy * dy) * 2.0f; // Scale up to match full Sobel
    }

    // Combine brightness and edge detection
    float intensity = MAX(perceivedBrightness, edgeStrength * 0.5f);

    // Boost edge pixels
    if (is_edge_pixel) {
        intensity = MAX(intensity, 0.5f);
    }

    // Apply threshold with smooth falloff
    float threshold_falloff = 0.1f;
    float glow_amount = 0.0f;

    // For transparent areas near visible content, use a lower threshold
    float effective_threshold = is_transparent ? threshold * 0.35f : threshold;

    if (intensity > effective_threshold) {
        // Smooth falloff rather than hard cutoff
        glow_amount = MIN(1.0f, (intensity - effective_threshold) / threshold_falloff);
        glow_amount = glow_amount * strength;

        // Apply curve for more pleasing glow falloff
        glow_amount = powf(glow_amount, 0.8f);

        if (is_transparent) {
            // For transparent areas, derive color from nearby visible pixels
            PF_Pixel16* sourceP = NULL;
            float maxAlpha = 0;

            // Find the most opaque neighboring pixel to use as color source
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;

                    PF_Pixel16* neighborP = GetPixel16(input, xL + dx, yL + dy);
                    if (neighborP->alpha > maxAlpha) {
                        maxAlpha = neighborP->alpha;
                        sourceP = neighborP;
                    }
                }
            }

            if (sourceP && maxAlpha > 1024) {
                // Use the color from the most opaque neighbor
                outP->red = (A_u_short)MIN(32768.0f, sourceP->red * glow_amount);
                outP->green = (A_u_short)MIN(32768.0f, sourceP->green * glow_amount);
                outP->blue = (A_u_short)MIN(32768.0f, sourceP->blue * glow_amount);
            }
            else {
                // Fallback to original color with reduced opacity
                outP->red = (A_u_short)MIN(32768.0f, inP->red * glow_amount);
                outP->green = (A_u_short)MIN(32768.0f, inP->green * glow_amount);
                outP->blue = (A_u_short)MIN(32768.0f, inP->blue * glow_amount);
            }
        }
        else {
            // Preserve original colors for non-transparent areas
            outP->red = (A_u_short)MIN(32768.0f, inP->red * glow_amount);
            outP->green = (A_u_short)MIN(32768.0f, inP->green * glow_amount);
            outP->blue = (A_u_short)MIN(32768.0f, inP->blue * glow_amount);
        }

        // Set alpha for the glow - keep original alpha for source pixels,
        // but use calculated glow amount for transparent areas
        outP->alpha = is_transparent ? (A_u_short)MIN(32768.0f, glow_amount * 32768.0f) : inP->alpha;
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
        outP->alpha = 0; // Make fully transparent
    }

    return PF_Err_NONE;
}

// Enhanced Gaussian blur horizontal pass - 8-bit
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

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float totalWeight = 0.0f;  // Track total applied weight for edge handling

    // Apply horizontal convolution with pre-computed kernel
    for (int i = -radius; i <= radius; i++) {
        PF_Pixel8* src = GetPixel8(input, xL + i, yL);
        float weight = kernel[i + radius];

        // For large kernels, prevent artifacts at buffer edges with alpha-weighted blending
        if (src->alpha > 0) {
            float alphaFactor = src->alpha / 255.0f;
            float adjustedWeight = weight * alphaFactor;

            r += src->red * adjustedWeight;
            g += src->green * adjustedWeight;
            b += src->blue * adjustedWeight;
            a += src->alpha * weight;  // Alpha uses original weight

            totalWeight += adjustedWeight;
        }
    }

    // Normalize by total weight to handle edges correctly
    if (totalWeight > 0.0f) {
        r /= totalWeight;
        g /= totalWeight;
        b /= totalWeight;
    }

    // Write blurred result
    outP->red = (A_u_char)MIN(255.0f, MAX(0.0f, r));
    outP->green = (A_u_char)MIN(255.0f, MAX(0.0f, g));
    outP->blue = (A_u_char)MIN(255.0f, MAX(0.0f, b));
    outP->alpha = (A_u_char)MIN(255.0f, MAX(0.0f, a)); // Preserve alpha blur

    return PF_Err_NONE;
}

// Enhanced Gaussian blur vertical pass - 8-bit
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

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float totalWeight = 0.0f;  // Track total applied weight for edge handling

    // Apply vertical convolution with pre-computed kernel
    for (int j = -radius; j <= radius; j++) {
        PF_Pixel8* src = GetPixel8(input, xL, yL + j);
        float weight = kernel[j + radius];

        // For large kernels, prevent artifacts at buffer edges with alpha-weighted blending
        if (src->alpha > 0) {
            float alphaFactor = src->alpha / 255.0f;
            float adjustedWeight = weight * alphaFactor;

            r += src->red * adjustedWeight;
            g += src->green * adjustedWeight;
            b += src->blue * adjustedWeight;
            a += src->alpha * weight;  // Alpha uses original weight

            totalWeight += adjustedWeight;
        }
    }

    // Normalize by total weight to handle edges correctly
    if (totalWeight > 0.0f) {
        r /= totalWeight;
        g /= totalWeight;
        b /= totalWeight;
    }

    // Write blurred result
    outP->red = (A_u_char)MIN(255.0f, MAX(0.0f, r));
    outP->green = (A_u_char)MIN(255.0f, MAX(0.0f, g));
    outP->blue = (A_u_char)MIN(255.0f, MAX(0.0f, b));
    outP->alpha = (A_u_char)MIN(255.0f, MAX(0.0f, a)); // Preserve alpha blur

    return PF_Err_NONE;
}

// Enhanced Gaussian blur horizontal pass - 16-bit
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

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float totalWeight = 0.0f;  // Track total applied weight for edge handling

    // Apply horizontal convolution with pre-computed kernel
    for (int i = -radius; i <= radius; i++) {
        PF_Pixel16* src = GetPixel16(input, xL + i, yL);
        float weight = kernel[i + radius];

        // For large kernels, prevent artifacts at buffer edges with alpha-weighted blending
        if (src->alpha > 0) {
            float alphaFactor = src->alpha / 32768.0f;
            float adjustedWeight = weight * alphaFactor;

            r += src->red * adjustedWeight;
            g += src->green * adjustedWeight;
            b += src->blue * adjustedWeight;
            a += src->alpha * weight;  // Alpha uses original weight

            totalWeight += adjustedWeight;
        }
    }

    // Normalize by total weight to handle edges correctly
    if (totalWeight > 0.0f) {
        r /= totalWeight;
        g /= totalWeight;
        b /= totalWeight;
    }

    // Write blurred result
    outP->red = (A_u_short)MIN(32768.0f, MAX(0.0f, r));
    outP->green = (A_u_short)MIN(32768.0f, MAX(0.0f, g));
    outP->blue = (A_u_short)MIN(32768.0f, MAX(0.0f, b));
    outP->alpha = (A_u_short)MIN(32768.0f, MAX(0.0f, a)); // Preserve alpha blur

    return PF_Err_NONE;
}

// Enhanced Gaussian blur vertical pass - 16-bit
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

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float totalWeight = 0.0f;  // Track total applied weight for edge handling

    // Apply vertical convolution with pre-computed kernel
    for (int j = -radius; j <= radius; j++) {
        PF_Pixel16* src = GetPixel16(input, xL, yL + j);
        float weight = kernel[j + radius];

        // For large kernels, prevent artifacts at buffer edges with alpha-weighted blending
        if (src->alpha > 0) {
            float alphaFactor = src->alpha / 32768.0f;
            float adjustedWeight = weight * alphaFactor;

            r += src->red * adjustedWeight;
            g += src->green * adjustedWeight;
            b += src->blue * adjustedWeight;
            a += src->alpha * weight;  // Alpha uses original weight

            totalWeight += adjustedWeight;
        }
    }

    // Normalize by total weight to handle edges correctly
    if (totalWeight > 0.0f) {
        r /= totalWeight;
        g /= totalWeight;
        b /= totalWeight;
    }

    // Write blurred result
    outP->red = (A_u_short)MIN(32768.0f, MAX(0.0f, r));
    outP->green = (A_u_short)MIN(32768.0f, MAX(0.0f, g));
    outP->blue = (A_u_short)MIN(32768.0f, MAX(0.0f, b));
    outP->alpha = (A_u_short)MIN(32768.0f, MAX(0.0f, a)); // Preserve alpha blur

    return PF_Err_NONE;
}

// Composite function for final combining of original image and glow - 8-bit
static PF_Err
CompositeOriginalAndGlow8(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlendDataP bdata = reinterpret_cast<BlendDataP>(refcon);
    PF_EffectWorld* glowWorld = bdata->glow;
    PF_EffectWorld* originalWorld = bdata->original;
    float blend_ratio = bdata->blend_ratio / 100.0f; // Convert 0-100 to 0.0-1.0

    // Get the glow value for this pixel
    PF_Pixel8* glowP = GetPixel8(glowWorld, xL, yL);

    // Get the original pixel value
    PF_Pixel8* originalP = GetPixel8(originalWorld, xL, yL);

    // Start with the original pixel
    *outP = *originalP;

    // If glow has some opacity, blend it
    if (glowP->alpha > 0) {
        // Apply the blend ratio to the glow's alpha
        float glowAlpha = (glowP->alpha / 255.0f) * blend_ratio;

        // Skip if glow is fully transparent after blend ratio
        if (glowAlpha < 0.001f) {
            return PF_Err_NONE;
        }

        // Use screen blend mode: 1 - (1-a)(1-b)
        float srcAlpha = originalP->alpha / 255.0f;
        float dstAlpha = glowAlpha;

        // Calculate resulting alpha (standard "over" compositing)
        float resultAlpha = srcAlpha + dstAlpha * (1.0f - srcAlpha);

        if (resultAlpha > 0.0f) {
            // Screen blend calculation with alpha
            float rs = 1.0f - ((1.0f - (originalP->red / 255.0f)) * (1.0f - (glowP->red / 255.0f) * dstAlpha));
            float gs = 1.0f - ((1.0f - (originalP->green / 255.0f)) * (1.0f - (glowP->green / 255.0f) * dstAlpha));
            float bs = 1.0f - ((1.0f - (originalP->blue / 255.0f)) * (1.0f - (glowP->blue / 255.0f) * dstAlpha));

            // Store the results
            outP->red = (A_u_char)MIN(255.0f, rs * 255.0f);
            outP->green = (A_u_char)MIN(255.0f, gs * 255.0f);
            outP->blue = (A_u_char)MIN(255.0f, bs * 255.0f);
            outP->alpha = (A_u_char)MIN(255.0f, resultAlpha * 255.0f);
        }
    }

    return PF_Err_NONE;
}

// Composite function for final combining of original image and glow - 16-bit
static PF_Err
CompositeOriginalAndGlow16(
    void* refcon,
    A_long xL,
    A_long yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlendDataP bdata = reinterpret_cast<BlendDataP>(refcon);
    PF_EffectWorld* glowWorld = bdata->glow;
    PF_EffectWorld* originalWorld = bdata->original;
    float blend_ratio = bdata->blend_ratio / 100.0f; // Convert 0-100 to 0.0-1.0

    // Get the glow value for this pixel
    PF_Pixel16* glowP = GetPixel16(glowWorld, xL, yL);

    // Get the original pixel value
    PF_Pixel16* originalP = GetPixel16(originalWorld, xL, yL);

    // Start with the original pixel
    *outP = *originalP;

    // If glow has some opacity, blend it
    if (glowP->alpha > 0) {
        // Apply the blend ratio to the glow's alpha
        float glowAlpha = (glowP->alpha / 32768.0f) * blend_ratio;

        // Skip if glow is fully transparent after blend ratio
        if (glowAlpha < 0.001f) {
            return PF_Err_NONE;
        }

        // Use screen blend mode: 1 - (1-a)(1-b)
        float srcAlpha = originalP->alpha / 32768.0f;
        float dstAlpha = glowAlpha;

        // Calculate resulting alpha (standard "over" compositing)
        float resultAlpha = srcAlpha + dstAlpha * (1.0f - srcAlpha);

        if (resultAlpha > 0.0f) {
            // Screen blend calculation with alpha
            float rs = 1.0f - ((1.0f - (originalP->red / 32768.0f)) * (1.0f - (glowP->red / 32768.0f) * dstAlpha));
            float gs = 1.0f - ((1.0f - (originalP->green / 32768.0f)) * (1.0f - (glowP->green / 32768.0f) * dstAlpha));
            float bs = 1.0f - ((1.0f - (originalP->blue / 32768.0f)) * (1.0f - (glowP->blue / 32768.0f) * dstAlpha));

            // Store the results
            outP->red = (A_u_short)MIN(32768.0f, rs * 32768.0f);
            outP->green = (A_u_short)MIN(32768.0f, gs * 32768.0f);
            outP->blue = (A_u_short)MIN(32768.0f, bs * 32768.0f);
            outP->alpha = (A_u_short)MIN(32768.0f, resultAlpha * 32768.0f);
        }
    }

    return PF_Err_NONE;
}

// Properly fixed Render function with correct downsampling approach
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

    // Get user parameters
    float strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    float radius_param = params[LITEGLOW_RADIUS]->u.fs_d.value;
    float threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value;
    int quality = params[LITEGLOW_QUALITY]->u.pd.value;
    float blend_ratio = params[LITEGLOW_BLEND]->u.fs_d.value;

    // If strength is zero (or near zero) or blend ratio is zero, just copy the input to output
    if (strength <= 0.1f || blend_ratio <= 0.1f) {
        err = PF_COPY(inputP, output, NULL, NULL);
        return err;
    }

    // Calculate downsampling factors - CORRECTLY FIXED to match the MultiSlicer reference
    float downscale_x = static_cast<float>(in_data->downsample_x.den) / static_cast<float>(in_data->downsample_x.num);
    float downscale_y = static_cast<float>(in_data->downsample_y.den) / static_cast<float>(in_data->downsample_y.num);
    float resolution_factor = MIN(downscale_x, downscale_y);

    // Correct approach: DIVIDE by resolution_factor to maintain visual size at lower resolutions
    // This matches the approach used in MultiSlicer.cpp
    float adjusted_radius = radius_param / resolution_factor;

    // For debugging
    // char debug_msg[256];
    // sprintf(debug_msg, "Resolution factor: %.2f, Raw radius: %.1f, Adjusted radius: %.1f", 
    //         resolution_factor, radius_param, adjusted_radius);
    // PF_STRCPY(out_data->return_msg, debug_msg);

    // Create temporary buffers for processing
    PF_EffectWorld bright_world, blur_h_world, blur_v_world, original_copy;

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

    // Create a copy of the original input
    ERR(suites.WorldSuite1()->new_world(in_data->effect_ref,
        output->width,
        output->height,
        0, // No flags
        &original_copy));

    // Copy the original input to our copy buffer
    ERR(PF_COPY(inputP, &original_copy, NULL, NULL));

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
            float kernel[KERNEL_SIZE_MAX * 2 + 1]; // Static array for kernel

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

                    // Make sure we don't exceed the cached kernel size limit
                    if (seq_data->gaussKernelSize <= KERNEL_SIZE_MAX * 2 + 1) {
                        memcpy(seq_data->gaussKernel, kernel, seq_data->gaussKernelSize * sizeof(float));
                    }
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

                // For high quality, apply a second blur pass (when not in preview mode)
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
                    // STEP 4: Composite the original image and the glow
                    BlendData blend_data;
                    blend_data.glow = &blur_v_world;
                    blend_data.original = &original_copy;
                    blend_data.quality = quality;
                    blend_data.blend_ratio = blend_ratio;

                    // Use the improved compositing function
                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,                      // progress base
                            linesL,                 // progress final
                            inputP,                 // src
                            NULL,                   // area - null for all pixels
                            (void*)&blend_data,     // refcon - blend parameters
                            CompositeOriginalAndGlow16, // New composite function
                            output));               // destination
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,                      // progress base
                            linesL,                 // progress final
                            inputP,                 // src
                            NULL,                   // area - null for all pixels
                            (void*)&blend_data,     // refcon - blend parameters
                            CompositeOriginalAndGlow8, // New composite function
                            output));               // destination
                    }
                }
            }
        }

        // Dispose of temporary worlds
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &bright_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_h_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_v_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &original_copy));
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

        case PF_Cmd_USER_CHANGED_PARAM:
            err = HandleChangedParam(in_data, out_data, params, output, (PF_UserChangedParamExtra*)extra);
            break;

        case PF_Cmd_QUERY_DYNAMIC_FLAGS:
            // Respond with dynamic flags based on current parameters
        {
            PF_ParamDef radius_param;
            AEFX_CLR_STRUCT(radius_param);
            PF_Err error = PF_Err_NONE;
            error = PF_CHECKOUT_PARAM(in_data, LITEGLOW_RADIUS, in_data->current_time, in_data->time_step, in_data->time_scale, &radius_param);

            if (!error) {
                // Always enable buffer expansion for radius > 0.5
                if (radius_param.u.fs_d.value <= 0.5f) {
                    out_data->out_flags &= ~PF_OutFlag_I_EXPAND_BUFFER;
                }
                else {
                    out_data->out_flags |= PF_OutFlag_I_EXPAND_BUFFER;
                }

                error = PF_CHECKIN_PARAM(in_data, &radius_param);
            }
        }
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