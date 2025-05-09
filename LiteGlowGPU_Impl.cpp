/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2025 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/*******************************************************************/

/*  LiteGlowGPU_Impl.cpp

    GPU-accelerated implementation of the LiteGlow effect.
    Implements GLSL shaders and GPU resource management for:
    - Bright pass extraction
    - Gaussian blur (separable implementation)
    - Screen composite blending

    Version     Change                                      Engineer    Date
    =======     ======                                      ========    ======
    2.0         Initial GPU implementation                  Dev         5/15/2025
*/

#include "LiteGlowGPU.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AEGP_SuiteHandler.h"

#ifdef AE_OS_WIN
#include <d3d11.h>
#endif
#ifdef AE_OS_MAC
#include <Metal/Metal.h>
#endif

// Define constants for shader indices
enum {
    LITEGLOW_SHADER_BRIGHT_EXTRACT = 0,  // Extract bright areas
    LITEGLOW_SHADER_BLUR_H,              // Horizontal blur
    LITEGLOW_SHADER_BLUR_V,              // Vertical blur
    LITEGLOW_SHADER_COMPOSITE,           // Composite (Screen blend)
    LITEGLOW_SHADER_COUNT                // Total number of shaders
};

// Define shader strings - these are the GLSL shaders that will run on the GPU
const char* g_GlowShaders[LITEGLOW_SHADER_COUNT] = {
    // LITEGLOW_SHADER_BRIGHT_EXTRACT - Extract pixels above threshold
    R"(
    #version 330 core
    
    // Input texture and parameters
    uniform sampler2D inputTexture;
    uniform float threshold;
    uniform float strength;
    
    // Texture coordinates from vertex shader
    in vec2 texCoord;
    out vec4 fragColor;
    
    void main() {
        // Sample the input texture
        vec4 color = texture(inputTexture, texCoord);
        
        // Calculate pixel brightness (using common luminance formula)
        float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
        
        // Apply threshold with smooth falloff
        float contrib = smoothstep(threshold / 255.0, threshold / 255.0 + 0.1, brightness);
        
        // Scale by strength
        contrib *= strength / 1000.0;
        
        // Output bright pixels, maintaining alpha from original
        fragColor = vec4(color.rgb * contrib, color.a);
    }
    )",

    // LITEGLOW_SHADER_BLUR_H - Horizontal Gaussian blur
    R"(
    #version 330 core
    
    // Input texture and parameters
    uniform sampler2D inputTexture;
    uniform float radius;
    uniform vec2 texelSize;
    uniform float kernel[128];
    uniform int kernelSize;
    
    // Texture coordinates from vertex shader
    in vec2 texCoord;
    out vec4 fragColor;
    
    void main() {
        vec4 sum = vec4(0.0);
        int samples = min(kernelSize, 128);
        
        // Apply horizontal blur
        for (int i = -samples/2; i <= samples/2; i++) {
            float weight = kernel[i + samples/2];
            vec2 offset = vec2(float(i) * texelSize.x, 0.0);
            vec4 sample = texture(inputTexture, texCoord + offset);
            sum += sample * weight;
        }
        
        fragColor = sum;
    }
    )",

    // LITEGLOW_SHADER_BLUR_V - Vertical Gaussian blur
    R"(
    #version 330 core
    
    // Input texture and parameters
    uniform sampler2D inputTexture;
    uniform float radius;
    uniform vec2 texelSize;
    uniform float kernel[128];
    uniform int kernelSize;
    
    // Texture coordinates from vertex shader
    in vec2 texCoord;
    out vec4 fragColor;
    
    void main() {
        vec4 sum = vec4(0.0);
        int samples = min(kernelSize, 128);
        
        // Apply vertical blur
        for (int i = -samples/2; i <= samples/2; i++) {
            float weight = kernel[i + samples/2];
            vec2 offset = vec2(0.0, float(i) * texelSize.y);
            vec4 sample = texture(inputTexture, texCoord + offset);
            sum += sample * weight;
        }
        
        fragColor = sum;
    }
    )",

    // LITEGLOW_SHADER_COMPOSITE - Composite using screen blend mode
    R"(
    #version 330 core
    
    // Input textures and parameters
    uniform sampler2D originalTexture;
    uniform sampler2D glowTexture;
    uniform float blendRatio;
    
    // Texture coordinates from vertex shader
    in vec2 texCoord;
    out vec4 fragColor;
    
    void main() {
        // Get original and glow colors
        vec4 originalColor = texture(originalTexture, texCoord);
        vec4 glowColor = texture(glowTexture, texCoord);
        
        // Apply screen blending formula: 1 - (1 - a) * (1 - b)
        vec3 screenColor = 1.0 - (1.0 - originalColor.rgb) * (1.0 - glowColor.rgb);
        
        // Linear interpolation between original and blended color based on blendRatio
        float blend = blendRatio / 100.0;
        vec3 finalColor = mix(originalColor.rgb, screenColor, blend);
        
        // Keep original alpha
        fragColor = vec4(finalColor, originalColor.a);
    }
    )"
};

// GPU resource management struct
typedef struct {
    bool initialized;
    AEGP_PlatformWindowRef platformWindowRef;
    AEGP_PlatformWindowRef stagingWindowRef;

    // Shader program handles
    void* shaderPrograms[LITEGLOW_SHADER_COUNT];

    // Render target handles
    void* brightPassTarget;
    void* blurHTarget;
    void* blurVTarget;

    // Texture handles
    void* inputTexture;
    void* outputTexture;

    // Kernel data
    float kernelData[128];
    int kernelSize;

} GPUResourceData;

// Initialize GPU resources
static PF_Err
InitGPUResources(
    AEGP_SuiteHandler* suites,
    PF_InData* in_data,
    GPUResourceData* gpuData)
{
    PF_Err err = PF_Err_NONE;

    if (!gpuData->initialized) {
        AEGP_GPUSuite1* gpuSuite = suites->AEGP_GPUSuite1();

        if (gpuSuite) {
            // Create platform window ref
            ERR(gpuSuite->AEGP_GetGPUPlatformWindowRef(in_data->effect_ref, &gpuData->platformWindowRef));

            // Get staging window ref for data transfer
            ERR(gpuSuite->AEGP_GetGPUStgPlatformWindowRef(in_data->effect_ref, &gpuData->stagingWindowRef));

            if (!err) {
                // Compile shaders
                for (int i = 0; i < LITEGLOW_SHADER_COUNT; i++) {
                    ERR(gpuSuite->AEGP_CreateGPUShaderProgram(
                        in_data->effect_ref,
                        g_GlowShaders[i],
                        strlen(g_GlowShaders[i]),
                        &gpuData->shaderPrograms[i]));
                }

                // Create render targets (will be resized as needed)
                ERR(gpuSuite->AEGP_CreateGPURenderTarget(
                    in_data->effect_ref,
                    1, 1,  // Initial size, will be resized
                    AEGP_GPU_Format_RGBA16F,
                    &gpuData->brightPassTarget));

                ERR(gpuSuite->AEGP_CreateGPURenderTarget(
                    in_data->effect_ref,
                    1, 1,  // Initial size, will be resized
                    AEGP_GPU_Format_RGBA16F,
                    &gpuData->blurHTarget));

                ERR(gpuSuite->AEGP_CreateGPURenderTarget(
                    in_data->effect_ref,
                    1, 1,  // Initial size, will be resized
                    AEGP_GPU_Format_RGBA16F,
                    &gpuData->blurVTarget));

                gpuData->initialized = true;
            }
        }
        else {
            err = PF_Err_BAD_CALLBACK_PARAM;
        }
    }

    return err;
}

// Release GPU resources
static PF_Err
ReleaseGPUResources(
    AEGP_SuiteHandler* suites,
    PF_InData* in_data,
    GPUResourceData* gpuData)
{
    PF_Err err = PF_Err_NONE;

    if (gpuData->initialized) {
        AEGP_GPUSuite1* gpuSuite = suites->AEGP_GPUSuite1();

        if (gpuSuite) {
            // Release shader programs
            for (int i = 0; i < LITEGLOW_SHADER_COUNT; i++) {
                if (gpuData->shaderPrograms[i]) {
                    ERR(gpuSuite->AEGP_ReleaseGPUShaderProgram(
                        in_data->effect_ref,
                        gpuData->shaderPrograms[i]));
                    gpuData->shaderPrograms[i] = NULL;
                }
            }

            // Release render targets
            if (gpuData->brightPassTarget) {
                ERR(gpuSuite->AEGP_ReleaseGPURenderTarget(
                    in_data->effect_ref,
                    gpuData->brightPassTarget));
                gpuData->brightPassTarget = NULL;
            }

            if (gpuData->blurHTarget) {
                ERR(gpuSuite->AEGP_ReleaseGPURenderTarget(
                    in_data->effect_ref,
                    gpuData->blurHTarget));
                gpuData->blurHTarget = NULL;
            }

            if (gpuData->blurVTarget) {
                ERR(gpuSuite->AEGP_ReleaseGPURenderTarget(
                    in_data->effect_ref,
                    gpuData->blurVTarget));
                gpuData->blurVTarget = NULL;
            }

            gpuData->initialized = false;
        }
        else {
            err = PF_Err_BAD_CALLBACK_PARAM;
        }
    }

    return err;
}

// Resize render targets if dimensions have changed
static PF_Err
ResizeRenderTargets(
    AEGP_SuiteHandler* suites,
    PF_InData* in_data,
    GPUResourceData* gpuData,
    int width,
    int height)
{
    PF_Err err = PF_Err_NONE;

    if (gpuData->initialized) {
        AEGP_GPUSuite1* gpuSuite = suites->AEGP_GPUSuite1();

        if (gpuSuite) {
            // Resize bright pass target
            ERR(gpuSuite->AEGP_ResizeGPURenderTarget(
                in_data->effect_ref,
                gpuData->brightPassTarget,
                width, height));

            // Resize horizontal blur target
            ERR(gpuSuite->AEGP_ResizeGPURenderTarget(
                in_data->effect_ref,
                gpuData->blurHTarget,
                width, height));

            // Resize vertical blur target
            ERR(gpuSuite->AEGP_ResizeGPURenderTarget(
                in_data->effect_ref,
                gpuData->blurVTarget,
                width, height));
        }
        else {
            err = PF_Err_BAD_CALLBACK_PARAM;
        }
    }

    return err;
}

// Generate a Gaussian kernel for the GPU
static void
GenerateGaussianKernelGPU(
    float sigma,
    float* kernel,
    int* kernelSize)
{
    // Calculate kernel size based on sigma (3*sigma covers 99.7% of the distribution)
    // Ensure odd kernel size for centered blur
    *kernelSize = 2 * (int)(3.0f * sigma + 0.5f) + 1;
    *kernelSize = MIN(127, *kernelSize); // Limit to 127 samples max (fits in 128-element array)

    float sum = 0.0f;
    int halfSize = *kernelSize / 2;

    // Fill kernel with Gaussian values
    for (int i = -halfSize; i <= halfSize; i++) {
        float x = (float)i;
        kernel[i + halfSize] = exp(-(x * x) / (2.0f * sigma * sigma));
        sum += kernel[i + halfSize];
    }

    // Normalize kernel
    for (int i = 0; i < *kernelSize; i++) {
        kernel[i] /= sum;
    }
}

// Main GPU implementation
PF_Err
LiteGlowGPU_Render(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_EffectWorld* input_worldP,
    PF_EffectWorld* output_worldP,
    float strength,
    float radius,
    float threshold,
    int quality,
    float blend_ratio)
{
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    // Ensure we have GPU suites available
    AEGP_GPUSuite1* gpuSuite = suites.AEGP_GPUSuite1();
    if (!gpuSuite) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Get sequence data for our GPU resources
    LiteGlowSequenceData* seq_data = NULL;
    if (in_data->sequence_data) {
        seq_data = (LiteGlowSequenceData*)suites.HandleSuite1()->host_lock_handle(in_data->sequence_data);
    }

    if (!seq_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Initialize or retrieve GPU data
    GPUResourceData gpuData = { 0 };
    gpuData.initialized = false;

    // Check if we have GPU data cached
    if (seq_data->gpuData) {
        // In a real implementation, we'd retrieve the cached GPU data
        // For simplicity, we'll just initialize it fresh
    }

    // Initialize GPU resources
    ERR(InitGPUResources(&suites, in_data, &gpuData));

    if (!err) {
        // Get dimensions
        int width = output_worldP->width;
        int height = output_worldP->height;

        // Resize render targets if needed
        ERR(ResizeRenderTargets(&suites, in_data, &gpuData, width, height));

        // Generate Gaussian kernel based on radius and quality
        float sigma;
        switch (quality) {
        case QUALITY_LOW:
            sigma = radius * 0.5f;
            break;
        case QUALITY_MEDIUM:
            sigma = radius * 0.75f;
            break;
        case QUALITY_HIGH:
        default:
            sigma = radius;
            break;
        }

        int kernelSize;
        GenerateGaussianKernelGPU(sigma, gpuData.kernelData, &kernelSize);
        gpuData.kernelSize = kernelSize;

        // Create AEGP_GPU textures from input and output worlds
        AEGP_GPUTextureRef inputTexture = NULL;
        AEGP_GPUTextureRef outputTexture = NULL;

        ERR(gpuSuite->AEGP_GetGPUWorldTextureRef(
            in_data->effect_ref,
            input_worldP,
            AEGP_GPU_TexMap_RGBA_Float,
            &inputTexture));

        ERR(gpuSuite->AEGP_GetGPUWorldTextureRef(
            in_data->effect_ref,
            output_worldP,
            AEGP_GPU_TexMap_RGBA_Float,
            &outputTexture));

        if (!err) {
            // Set up viewport
            ERR(gpuSuite->AEGP_SetGPUViewportSize(
                in_data->effect_ref,
                width, height));

            // STEP 1: Extract bright areas
            ERR(gpuSuite->AEGP_SetGPURenderTarget(
                in_data->effect_ref,
                gpuData.brightPassTarget));

            if (!err) {
                // Bind bright extract shader
                ERR(gpuSuite->AEGP_UseGPUShaderProgram(
                    in_data->effect_ref,
                    gpuData.shaderPrograms[LITEGLOW_SHADER_BRIGHT_EXTRACT]));

                // Set shader uniforms
                ERR(gpuSuite->AEGP_SetGPUShaderFloat(
                    in_data->effect_ref,
                    "threshold",
                    threshold));

                ERR(gpuSuite->AEGP_SetGPUShaderFloat(
                    in_data->effect_ref,
                    "strength",
                    strength));

                // Bind input texture
                ERR(gpuSuite->AEGP_SetGPUShaderTexture(
                    in_data->effect_ref,
                    "inputTexture",
                    inputTexture));

                // Draw fullscreen quad
                ERR(gpuSuite->AEGP_DrawGPUFullScreenQuad(in_data->effect_ref));
            }

            // STEP 2: Apply horizontal blur
            ERR(gpuSuite->AEGP_SetGPURenderTarget(
                in_data->effect_ref,
                gpuData.blurHTarget));

            if (!err) {
                // Bind horizontal blur shader
                ERR(gpuSuite->AEGP_UseGPUShaderProgram(
                    in_data->effect_ref,
                    gpuData.shaderPrograms[LITEGLOW_SHADER_BLUR_H]));

                // Set shader uniforms
                ERR(gpuSuite->AEGP_SetGPUShaderFloat(
                    in_data->effect_ref,
                    "radius",
                    radius));

                ERR(gpuSuite->AEGP_SetGPUShaderVec2(
                    in_data->effect_ref,
                    "texelSize",
                    1.0f / width, 1.0f / height));

                ERR(gpuSuite->AEGP_SetGPUShaderFloatArray(
                    in_data->effect_ref,
                    "kernel",
                    gpuData.kernelData,
                    gpuData.kernelSize));

                ERR(gpuSuite->AEGP_SetGPUShaderInt(
                    in_data->effect_ref,
                    "kernelSize",
                    gpuData.kernelSize));

                // Bind bright pass texture as input
                ERR(gpuSuite->AEGP_GetGPURenderTargetTexture(
                    in_data->effect_ref,
                    gpuData.brightPassTarget,
                    &inputTexture));

                ERR(gpuSuite->AEGP_SetGPUShaderTexture(
                    in_data->effect_ref,
                    "inputTexture",
                    inputTexture));

                // Draw fullscreen quad
                ERR(gpuSuite->AEGP_DrawGPUFullScreenQuad(in_data->effect_ref));
            }

            // STEP 3: Apply vertical blur
            ERR(gpuSuite->AEGP_SetGPURenderTarget(
                in_data->effect_ref,
                gpuData.blurVTarget));

            if (!err) {
                // Bind vertical blur shader
                ERR(gpuSuite->AEGP_UseGPUShaderProgram(
                    in_data->effect_ref,
                    gpuData.shaderPrograms[LITEGLOW_SHADER_BLUR_V]));

                // Set shader uniforms
                ERR(gpuSuite->AEGP_SetGPUShaderFloat(
                    in_data->effect_ref,
                    "radius",
                    radius));

                ERR(gpuSuite->AEGP_SetGPUShaderVec2(
                    in_data->effect_ref,
                    "texelSize",
                    1.0f / width, 1.0f / height));

                ERR(gpuSuite->AEGP_SetGPUShaderFloatArray(
                    in_data->effect_ref,
                    "kernel",
                    gpuData.kernelData,
                    gpuData.kernelSize));

                ERR(gpuSuite->AEGP_SetGPUShaderInt(
                    in_data->effect_ref,
                    "kernelSize",
                    gpuData.kernelSize));

                // Bind horizontal blur texture as input
                ERR(gpuSuite->AEGP_GetGPURenderTargetTexture(
                    in_data->effect_ref,
                    gpuData.blurHTarget,
                    &inputTexture));

                ERR(gpuSuite->AEGP_SetGPUShaderTexture(
                    in_data->effect_ref,
                    "inputTexture",
                    inputTexture));

                // Draw fullscreen quad
                ERR(gpuSuite->AEGP_DrawGPUFullScreenQuad(in_data->effect_ref));
            }

            // STEP 4: Composite with original
            ERR(gpuSuite->AEGP_SetGPURenderTarget(
                in_data->effect_ref,
                outputTexture));

            if (!err) {
                // Bind composite shader
                ERR(gpuSuite->AEGP_UseGPUShaderProgram(
                    in_data->effect_ref,
                    gpuData.shaderPrograms[LITEGLOW_SHADER_COMPOSITE]));

                // Set shader uniforms
                ERR(gpuSuite->AEGP_SetGPUShaderFloat(
                    in_data->effect_ref,
                    "blendRatio",
                    blend_ratio));

                // Bind original image texture
                AEGP_GPUTextureRef originalTexture = NULL;
                ERR(gpuSuite->AEGP_GetGPUWorldTextureRef(
                    in_data->effect_ref,
                    input_worldP,
                    AEGP_GPU_TexMap_RGBA_Float,
                    &originalTexture));

                ERR(gpuSuite->AEGP_SetGPUShaderTexture(
                    in_data->effect_ref,
                    "originalTexture",
                    originalTexture));

                // Bind blurred glow texture
                AEGP_GPUTextureRef glowTexture = NULL;
                ERR(gpuSuite->AEGP_GetGPURenderTargetTexture(
                    in_data->effect_ref,
                    gpuData.blurVTarget,
                    &glowTexture));

                ERR(gpuSuite->AEGP_SetGPUShaderTexture(
                    in_data->effect_ref,
                    "glowTexture",
                    glowTexture));

                // Draw fullscreen quad
                ERR(gpuSuite->AEGP_DrawGPUFullScreenQuad(in_data->effect_ref));

                // Release textures
                if (originalTexture) {
                    ERR(gpuSuite->AEGP_ReleaseGPUTextureRef(in_data->effect_ref, originalTexture));
                }
                if (glowTexture) {
                    ERR(gpuSuite->AEGP_ReleaseGPUTextureRef(in_data->effect_ref, glowTexture));
                }
            }

            // Release textures
            if (inputTexture) {
                ERR(gpuSuite->AEGP_ReleaseGPUTextureRef(in_data->effect_ref, inputTexture));
            }
            if (outputTexture) {
                ERR(gpuSuite->AEGP_ReleaseGPUTextureRef(in_data->effect_ref, outputTexture));
            }
        }
    }

    // Cache GPU resources for reuse (in a real implementation)
    // For now, we'll just clean up
    ReleaseGPUResources(&suites, in_data, &gpuData);

    // Unlock sequence data
    if (seq_data) {
        suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
    }

    return err;
}