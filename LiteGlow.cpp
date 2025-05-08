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

    An optimized glow effect with full color support and edge diffusion.

    Revision History

    Version		Change													Engineer	Date
    =======		======													========	======
    1.0			First implementation										yourName	5/8/2025

*/

#include "LiteGlow.h"

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

    out_data->num_params = LITEGLOW_NUM_PARAMS;

    return err;
}

// Get pixel from buffer with boundary checking
inline PF_Pixel8* GetPixel8(PF_EffectWorld* world, int x, int y) {
    // Clamp coordinates to valid range
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));

    // Get pixel pointer
    return (PF_Pixel8*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel8));
}

inline PF_Pixel16* GetPixel16(PF_EffectWorld* world, int x, int y) {
    // Clamp coordinates to valid range
    x = MAX(0, MIN(x, world->width - 1));
    y = MAX(0, MIN(y, world->height - 1));

    // Get pixel pointer
    return (PF_Pixel16*)((char*)world->data + y * world->rowbytes + x * sizeof(PF_Pixel16));
}

// Calculate perceptual luminance - much better than average for color perception
inline float PerceivedBrightness8(PF_Pixel8* p) {
    // Human perception weights: R=0.299, G=0.587, B=0.114
    return (0.299f * p->red + 0.587f * p->green + 0.114f * p->blue);
}

inline float PerceivedBrightness16(PF_Pixel16* p) {
    // Human perception weights: R=0.299, G=0.587, B=0.114
    return (0.299f * p->red + 0.587f * p->green + 0.114f * p->blue);
}

// Return the maximum RGB component to respect saturated colors
inline A_u_char MaxRGB8(PF_Pixel8* p) {
    return MAX(p->red, MAX(p->green, p->blue));
}

inline A_u_short MaxRGB16(PF_Pixel16* p) {
    return MAX(p->red, MAX(p->green, p->blue));
}

// Extract bright areas for glow - 8-bit
static PF_Err
ExtractBright8(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);
    float strength = gdata->strength / 1000.0f; // Normalize strength

    // Calculate both perceptual brightness and max RGB
    float perceivedBrightness = PerceivedBrightness8(inP);
    A_u_char maxRGB = MaxRGB8(inP);

    // Combine both metrics for better color handling
    // This ensures saturated colors are properly detected
    float brightness = MAX(perceivedBrightness, maxRGB);

    // Lower threshold to capture more color
    if (brightness > 80) { // Reduced from 128 to capture more color
        // Use a smooth falloff instead of hard cutoff
        float intensity = (brightness - 80.0f) / 175.0f;
        intensity = MIN(1.0f, intensity) * strength;

        // Preserve original color by scaling
        outP->red = (A_u_char)(inP->red * intensity);
        outP->green = (A_u_char)(inP->green * intensity);
        outP->blue = (A_u_char)(inP->blue * intensity);
        outP->alpha = inP->alpha;
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
        outP->alpha = inP->alpha;
    }

    return PF_Err_NONE;
}

// Extract bright areas for glow - 16-bit
static PF_Err
ExtractBright16(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    GlowDataP gdata = reinterpret_cast<GlowDataP>(refcon);
    float strength = gdata->strength / 1000.0f; // Normalize strength

    // Calculate both perceptual brightness and max RGB
    float perceivedBrightness = PerceivedBrightness16(inP);
    A_u_short maxRGB = MaxRGB16(inP);

    // Combine both metrics for better color handling
    // This ensures saturated colors are properly detected
    float brightness = MAX(perceivedBrightness, maxRGB);

    // Lower threshold to capture more color
    float threshold = 10000.0f; // Lower threshold for 16-bit
    if (brightness > threshold) {
        // Use a smooth falloff instead of hard cutoff
        float intensity = (brightness - threshold) / (32768.0f - threshold);
        intensity = MIN(1.0f, intensity) * strength;

        // Preserve original color by scaling
        outP->red = (A_u_short)(inP->red * intensity);
        outP->green = (A_u_short)(inP->green * intensity);
        outP->blue = (A_u_short)(inP->blue * intensity);
        outP->alpha = inP->alpha;
    }
    else {
        // Dark areas don't contribute to glow
        outP->red = outP->green = outP->blue = 0;
        outP->alpha = inP->alpha;
    }

    return PF_Err_NONE;
}

// Fast horizontal blur for 8-bit
static PF_Err
FastHBlur8(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    int radius = bdata->radius;

    A_long sumR = 0, sumG = 0, sumB = 0;
    float weight_sum = 0.0f;

    // Fast horizontal sampling with distance falloff
    for (int i = -radius; i <= radius; i++) {
        // Calculate weight based on distance (approximated Gaussian)
        float weight = (radius - abs(i) + 1.0f) / (radius + 1.0f);
        weight_sum += weight;

        PF_Pixel8* sample = GetPixel8(input, xL + i, yL);
        sumR += (A_long)(sample->red * weight);
        sumG += (A_long)(sample->green * weight);
        sumB += (A_long)(sample->blue * weight);
    }

    // Average the result
    if (weight_sum > 0.0f) {
        outP->red = (A_u_char)(sumR / weight_sum);
        outP->green = (A_u_char)(sumG / weight_sum);
        outP->blue = (A_u_char)(sumB / weight_sum);
    }
    else {
        outP->red = inP->red;
        outP->green = inP->green;
        outP->blue = inP->blue;
    }
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Fast vertical blur for 8-bit
static PF_Err
FastVBlur8(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    int radius = bdata->radius;

    A_long sumR = 0, sumG = 0, sumB = 0;
    float weight_sum = 0.0f;

    // Fast vertical sampling with distance falloff
    for (int j = -radius; j <= radius; j++) {
        // Calculate weight based on distance (approximated Gaussian)
        float weight = (radius - abs(j) + 1.0f) / (radius + 1.0f);
        weight_sum += weight;

        PF_Pixel8* sample = GetPixel8(input, xL, yL + j);
        sumR += (A_long)(sample->red * weight);
        sumG += (A_long)(sample->green * weight);
        sumB += (A_long)(sample->blue * weight);
    }

    // Average the result
    if (weight_sum > 0.0f) {
        outP->red = (A_u_char)(sumR / weight_sum);
        outP->green = (A_u_char)(sumG / weight_sum);
        outP->blue = (A_u_char)(sumB / weight_sum);
    }
    else {
        outP->red = inP->red;
        outP->green = inP->green;
        outP->blue = inP->blue;
    }
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Fast horizontal blur for 16-bit
static PF_Err
FastHBlur16(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    int radius = bdata->radius;

    A_long sumR = 0, sumG = 0, sumB = 0;
    float weight_sum = 0.0f;

    // Fast horizontal sampling with distance falloff
    for (int i = -radius; i <= radius; i++) {
        // Calculate weight based on distance (approximated Gaussian)
        float weight = (radius - abs(i) + 1.0f) / (radius + 1.0f);
        weight_sum += weight;

        PF_Pixel16* sample = GetPixel16(input, xL + i, yL);
        sumR += (A_long)(sample->red * weight);
        sumG += (A_long)(sample->green * weight);
        sumB += (A_long)(sample->blue * weight);
    }

    // Average the result
    if (weight_sum > 0.0f) {
        outP->red = (A_u_short)(sumR / weight_sum);
        outP->green = (A_u_short)(sumG / weight_sum);
        outP->blue = (A_u_short)(sumB / weight_sum);
    }
    else {
        outP->red = inP->red;
        outP->green = inP->green;
        outP->blue = inP->blue;
    }
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Fast vertical blur for 16-bit
static PF_Err
FastVBlur16(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    BlurDataP bdata = reinterpret_cast<BlurDataP>(refcon);
    PF_EffectWorld* input = bdata->input;
    int radius = bdata->radius;

    A_long sumR = 0, sumG = 0, sumB = 0;
    float weight_sum = 0.0f;

    // Fast vertical sampling with distance falloff
    for (int j = -radius; j <= radius; j++) {
        // Calculate weight based on distance (approximated Gaussian)
        float weight = (radius - abs(j) + 1.0f) / (radius + 1.0f);
        weight_sum += weight;

        PF_Pixel16* sample = GetPixel16(input, xL, yL + j);
        sumR += (A_long)(sample->red * weight);
        sumG += (A_long)(sample->green * weight);
        sumB += (A_long)(sample->blue * weight);
    }

    // Average the result
    if (weight_sum > 0.0f) {
        outP->red = (A_u_short)(sumR / weight_sum);
        outP->green = (A_u_short)(sumG / weight_sum);
        outP->blue = (A_u_short)(sumB / weight_sum);
    }
    else {
        outP->red = inP->red;
        outP->green = inP->green;
        outP->blue = inP->blue;
    }
    outP->alpha = inP->alpha;

    return PF_Err_NONE;
}

// Screen blend for natural glow - 8-bit
inline void ScreenBlend8(PF_Pixel8* a, PF_Pixel8* b, PF_Pixel8* result) {
    // Screen blend formula: 1 - (1-a) * (1-b)
    // Simplified to: a + b - (a * b / 255)
    result->red = MIN(PF_MAX_CHAN8, a->red + b->red - ((a->red * b->red) >> 8));
    result->green = MIN(PF_MAX_CHAN8, a->green + b->green - ((a->green * b->green) >> 8));
    result->blue = MIN(PF_MAX_CHAN8, a->blue + b->blue - ((a->blue * b->blue) >> 8));
    // Keep original alpha
    result->alpha = a->alpha;
}

// Screen blend for natural glow - 16-bit
inline void ScreenBlend16(PF_Pixel16* a, PF_Pixel16* b, PF_Pixel16* result) {
    // Screen blend formula: 1 - (1-a) * (1-b)
    // Simplified to: a + b - (a * b / 65535)
    result->red = MIN(PF_MAX_CHAN16, a->red + b->red - ((a->red * b->red) / PF_MAX_CHAN16));
    result->green = MIN(PF_MAX_CHAN16, a->green + b->green - ((a->green * b->green) / PF_MAX_CHAN16));
    result->blue = MIN(PF_MAX_CHAN16, a->blue + b->blue - ((a->blue * b->blue) / PF_MAX_CHAN16));
    // Keep original alpha
    result->alpha = a->alpha;
}

// Combine original with glow using screen blend - 8-bit
static PF_Err
AddGlow8(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel8* inP,
    PF_Pixel8* outP)
{
    PF_EffectWorld* glowWorld = reinterpret_cast<PF_EffectWorld*>(refcon);

    // Get the glow value for this pixel
    PF_Pixel8* glowP = GetPixel8(glowWorld, xL, yL);

    // Use screen blend for more natural glow
    ScreenBlend8(inP, glowP, outP);

    return PF_Err_NONE;
}

// Combine original with glow using screen blend - 16-bit
static PF_Err
AddGlow16(
    void* refcon,
    A_long		xL,
    A_long		yL,
    PF_Pixel16* inP,
    PF_Pixel16* outP)
{
    PF_EffectWorld* glowWorld = reinterpret_cast<PF_EffectWorld*>(refcon);

    // Get the glow value for this pixel
    PF_Pixel16* glowP = GetPixel16(glowWorld, xL, yL);

    // Use screen blend for more natural glow
    ScreenBlend16(inP, glowP, outP);

    return PF_Err_NONE;
}

static PF_Err
Render(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    PF_Err				err = PF_Err_NONE;
    AEGP_SuiteHandler	suites(in_data->pica_basicP);
    A_long				linesL = output->height;

    // If strength is zero (or near zero), just copy the input to output
    float strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    if (strength <= 0.1f) {
        err = PF_COPY(&params[LITEGLOW_INPUT]->u.ld, output, NULL, NULL);
        return err;
    }

    // Create worlds for processing stages
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

        // STEP 1: Extract bright parts (color-aware)
        if (PF_WORLD_IS_DEEP(output)) {
            ERR(suites.Iterate16Suite2()->iterate(in_data,
                0,                            // progress base
                linesL,                       // progress final
                &params[LITEGLOW_INPUT]->u.ld, // src 
                NULL,                         // area - null for all pixels
                (void*)&gdata,                // refcon with parameters
                ExtractBright16,              // pixel function
                &bright_world));              // destination
        }
        else {
            ERR(suites.Iterate8Suite2()->iterate(in_data,
                0,                            // progress base
                linesL,                       // progress final
                &params[LITEGLOW_INPUT]->u.ld, // src 
                NULL,                         // area - null for all pixels
                (void*)&gdata,                // refcon with parameters
                ExtractBright8,               // pixel function
                &bright_world));              // destination
        }

        if (!err) {
            // Calculate blur radius based on strength - larger radius for boundary diffusion
            int radius = 5 + (int)(strength / 200.0f);
            radius = MIN(radius, 30); // Cap at reasonable value for performance

            // Create blur data
            BlurData bdata;
            bdata.input = &bright_world;
            bdata.radius = radius;

            // STEP 2: Apply first horizontal blur pass
            if (PF_WORLD_IS_DEEP(output)) {
                ERR(suites.Iterate16Suite2()->iterate(in_data,
                    0,                // progress base
                    linesL,           // progress final
                    &bright_world,    // src 
                    NULL,             // area - null for all pixels
                    (void*)&bdata,    // refcon with blur data
                    FastHBlur16,      // pixel function
                    &blur_h_world));  // destination
            }
            else {
                ERR(suites.Iterate8Suite2()->iterate(in_data,
                    0,                // progress base
                    linesL,           // progress final
                    &bright_world,    // src 
                    NULL,             // area - null for all pixels
                    (void*)&bdata,    // refcon with blur data
                    FastHBlur8,       // pixel function
                    &blur_h_world));  // destination
            }

            if (!err) {
                // Update blur data for vertical pass
                bdata.input = &blur_h_world;

                // STEP 3: Apply first vertical blur pass
                if (PF_WORLD_IS_DEEP(output)) {
                    ERR(suites.Iterate16Suite2()->iterate(in_data,
                        0,               // progress base
                        linesL,          // progress final
                        &blur_h_world,   // src 
                        NULL,            // area - null for all pixels
                        (void*)&bdata,   // refcon with blur data
                        FastVBlur16,     // pixel function
                        &blur_v_world)); // destination
                }
                else {
                    ERR(suites.Iterate8Suite2()->iterate(in_data,
                        0,               // progress base
                        linesL,          // progress final
                        &blur_h_world,   // src 
                        NULL,            // area - null for all pixels
                        (void*)&bdata,   // refcon with blur data
                        FastVBlur8,      // pixel function
                        &blur_v_world)); // destination
                }

                // For higher strength values, apply a second blur pass for more diffusion
                if (strength > 800.0f && !err) {
                    // Update blur data
                    bdata.input = &blur_v_world;

                    // Second horizontal blur
                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            &blur_v_world,   // src 
                            NULL,            // area - null for all pixels
                            (void*)&bdata,   // refcon with blur data
                            FastHBlur16,     // pixel function
                            &bright_world)); // destination (reuse)
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,               // progress base
                            linesL,          // progress final
                            &blur_v_world,   // src 
                            NULL,            // area - null for all pixels
                            (void*)&bdata,   // refcon with blur data
                            FastHBlur8,      // pixel function
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
                                FastVBlur16,     // pixel function
                                &blur_v_world)); // destination
                        }
                        else {
                            ERR(suites.Iterate8Suite2()->iterate(in_data,
                                0,               // progress base
                                linesL,          // progress final
                                &bright_world,   // src 
                                NULL,            // area - null for all pixels
                                (void*)&bdata,   // refcon with blur data
                                FastVBlur8,      // pixel function
                                &blur_v_world)); // destination
                        }
                    }
                }

                if (!err) {
                    // STEP 4: Apply final screen blend
                    if (PF_WORLD_IS_DEEP(output)) {
                        ERR(suites.Iterate16Suite2()->iterate(in_data,
                            0,                            // progress base
                            linesL,                       // progress final
                            &params[LITEGLOW_INPUT]->u.ld, // src (original)
                            NULL,                         // area - null for all pixels
                            (void*)&blur_v_world,         // refcon - blurred glow image
                            AddGlow16,                    // pixel function
                            output));                     // destination
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(in_data,
                            0,                            // progress base
                            linesL,                       // progress final
                            &params[LITEGLOW_INPUT]->u.ld, // src (original)
                            NULL,                         // area - null for all pixels
                            (void*)&blur_v_world,         // refcon - blurred glow image
                            AddGlow8,                     // pixel function
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