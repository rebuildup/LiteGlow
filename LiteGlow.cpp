#include "LiteGlow.h"
#include "AEFX_SuiteHelper.h"
#include "AE_EffectGPUSuites.h"
#include "PrSDKAESupport.h"
#include "Smart_Utils.h"

#ifdef AE_OS_WIN
    #include "DirectXUtils.h"
    #include <d3dcompiler.h>
#endif

#include <math.h>
#include <string.h>

// Constants for Gaussian kernel generation
#define PI 3.14159265358979323846
#define KERNEL_SIZE_MAX 64

#ifdef AE_OS_WIN
    #define HAS_HLSL 1
#else
    #define HAS_HLSL 0
#endif
#define HAS_METAL 0

// Parameters cached during pre-render for both CPU and GPU paths.
typedef struct
{
    float strength;           // slider value (0 - STRENGTH_MAX)
    float radius;             // in pixels
    float threshold;          // 0.0 - 1.0 normalized
    int   quality;            // QUALITY_* enum
    float resolution_factor;  // preview / downsample factor
} LiteGlowRenderParams;

#if HAS_HLSL
// DirectX GPU data cached per device
struct LiteGlowDirectXGPUData
{
    DXContextPtr    context;
    ShaderObjectPtr glowShader;
};

// GPU constant buffer layout – must match LiteGlowKernel parameters.
typedef struct
{
    int   srcPitch;
    int   dstPitch;
    int   width;
    int   height;
    float strength;
    float threshold;
    float radius;
    int   quality;
} LiteGlowGPUParams;

inline PF_Err DXErr(bool success)
{
    return success ? PF_Err_NONE : PF_Err_INTERNAL_STRUCT_DAMAGED;
}
#define DX_ERR(FUNC) ERR(DXErr(FUNC))

static size_t
DivideRoundUpSizeT(size_t value, size_t multiple)
{
    return value ? (value + multiple - 1) / multiple : 0;
}

// Fallback: compile embedded HLSL when external CSO/RS files are missing.
static bool CompileEmbeddedLiteGlowShader(const DXContextPtr& ctx, const ShaderObjectPtr& shader)
{
    static const char* kLiteGlowHLSL = R"(cbuffer LiteGlowGPUParams : register(b0)
{
    int   srcPitch;
    int   dstPitch;
    int   width;
    int   height;
    float strengthNorm;
    float threshold;
    float radius;
    int   quality;
};

RWByteAddressBuffer gDst : register(u0);
ByteAddressBuffer  gSrc : register(t0);

static const float3 kLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

uint4 LoadPixel(uint x, uint y)
{
    const uint bytesPerPixel = 16;
    uint index = (y * (uint)srcPitch + x) * bytesPerPixel;
    return gSrc.Load4(index);
}

void StorePixel(uint x, uint y, float4 value)
{
    const uint bytesPerPixel = 16;
    uint index = (y * (uint)dstPitch + x) * bytesPerPixel;
    gDst.Store4(index, asuint(value));
}

float Gaussian1D(float dist, float sigma)
{
    return exp(-dist * dist / (2.0f * sigma * sigma));
}

[numthreads(16,16,1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint x = dtid.x;
    uint y = dtid.y;
    if (x >= (uint)width || y >= (uint)height)
        return;

    uint4 baseRaw = LoadPixel(x, y);
    float4 basePixel = asfloat(baseRaw);

    float baseLuma = dot(basePixel.rgb, kLumaWeights);
    float mask = saturate((baseLuma - threshold) * 4.0f);

    if (mask <= 0.0f || strengthNorm <= 0.01f)
    {
        StorePixel(x, y, basePixel);
        return;
    }

    const int SAMPLES_PER_DIR = 4;
    float rad = max(radius, 1.0f);
    float sigma = max(rad * 0.75f, 1.0f);

    static const float2 dirs[8] = {
        float2(1.0f, 0.0f),
        float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(0.7071f, 0.7071f),
        float2(-0.7071f, 0.7071f),
        float2(0.7071f, -0.7071f),
        float2(-0.7071f, -0.7071f)
    };

    float3 accum = basePixel.rgb;
    float totalW = 1.0f;

    [unroll]
    for (int d = 0; d < 8; ++d)
    {
        float2 dir = dirs[d];

        [unroll]
        for (int s = 1; s <= SAMPLES_PER_DIR; ++s)
        {
            float dist = rad * (s / (float)SAMPLES_PER_DIR);
            float w = Gaussian1D(dist, sigma);

            int sx = (int)(x + dir.x * dist + 0.5f);
            int sy = (int)(y + dir.y * dist + 0.5f);

            sx = clamp(sx, 0, width - 1);
            sy = clamp(sy, 0, height - 1);

            float4 sample = asfloat(LoadPixel((uint)sx, (uint)sy));
            float lum = dot(sample.rgb, kLumaWeights);

            if (lum > threshold)
            {
                accum += sample.rgb * w;
                totalW += w;
            }
        }
    }

    float3 glow = accum / totalW;
    glow *= saturate(0.5f + strengthNorm * 1.5f);
    glow = saturate(glow);

    float3 screenColor = 1.0f - (1.0f - basePixel.rgb) * (1.0f - glow);
    float3 finalColor = lerp(basePixel.rgb, screenColor, mask);

    StorePixel(x, y, float4(finalColor, basePixel.a));
})";

    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;

    HRESULT hr = D3DCompile(
        kLiteGlowHLSL,
        strlen(kLiteGlowHLSL),
        "LiteGlow_Kernel",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &csBlob,
        &errBlob);

    if (FAILED(hr) || !csBlob) {
        return false;
    }

    // Root signature: b0 (CBV), u0 (UAV), t0 (SRV)
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = _countof(params);
    rootDesc.pParameters = params;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr) || !sigBlob) {
        return false;
    }

    hr = ctx->mDevice->CreateRootSignature(
        0,
        sigBlob->GetBufferPointer(),
        sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&shader->mRootSignature));
    if (FAILED(hr)) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = shader->mRootSignature.Get();
    psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
    psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();

    hr = ctx->mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&shader->mPipelineState));
    return SUCCEEDED(hr);
}
#endif // HAS_HLSL

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
    static const PFVersionInfo kLiteGlowVersion = (PFVersionInfo)LITEGLOW_VERSION_VALUE;
    out_data->my_version = kLiteGlowVersion;

    // Basic flags: deep color, pixel independence, UI updates.
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE |
        PF_OutFlag_PIX_INDEPENDENT |
        PF_OutFlag_SEND_UPDATE_PARAMS_UI;

    // Smart Render + threaded rendering.
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER |
        PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
        PF_OutFlag2_FLOAT_COLOR_AWARE;

    // GPU support: Premiere uses pixel format suite, AE uses GPU flags directly.
    if (in_data->appl_id == 'PrMr') {
        AEFX_SuiteScoper<PF_PixelFormatSuite1> pixelFormatSuite(
            in_data,
            kPFPixelFormatSuite,
            kPFPixelFormatSuiteVersion1,
            out_data);

        // Add the pixel formats we support in order of preference.
        (*pixelFormatSuite->ClearSupportedPixelFormats)(in_data->effect_ref);
        (*pixelFormatSuite->AddSupportedPixelFormat)(
            in_data->effect_ref,
            PrPixelFormat_VUYA_4444_32f);
    }
#if HAS_HLSL
    else {
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 |
            PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    }
#endif

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

// Utility helpers for resampling
static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

struct NormalizedPixel {
    float r, g, b, a;
};

static NormalizedPixel ReadNormalized(PF_EffectWorld* world, int x, int y, PF_Boolean is_deep) {
    NormalizedPixel result;
    if (is_deep) {
        PF_Pixel16* p = GetPixel16(world, x, y);
        result = {
            p->red / 32768.0f,
            p->green / 32768.0f,
            p->blue / 32768.0f,
            p->alpha / 32768.0f
        };
    }
    else {
        PF_Pixel8* p = GetPixel8(world, x, y);
        result = {
            p->red / 255.0f,
            p->green / 255.0f,
            p->blue / 255.0f,
            p->alpha / 255.0f
        };
    }
    return result;
}

static void WriteNormalized(PF_EffectWorld* world, int x, int y, const NormalizedPixel& px, PF_Boolean is_deep) {
    if (is_deep) {
        PF_Pixel16* p = GetPixel16(world, x, y);
        p->red = (A_u_short)MIN(32768.0f, MAX(0.0f, px.r * 32768.0f));
        p->green = (A_u_short)MIN(32768.0f, MAX(0.0f, px.g * 32768.0f));
        p->blue = (A_u_short)MIN(32768.0f, MAX(0.0f, px.b * 32768.0f));
        p->alpha = (A_u_short)MIN(32768.0f, MAX(0.0f, px.a * 32768.0f));
    }
    else {
        PF_Pixel8* p = GetPixel8(world, x, y);
        p->red = (A_u_char)MIN(255.0f, MAX(0.0f, px.r * 255.0f));
        p->green = (A_u_char)MIN(255.0f, MAX(0.0f, px.g * 255.0f));
        p->blue = (A_u_char)MIN(255.0f, MAX(0.0f, px.b * 255.0f));
        p->alpha = (A_u_char)MIN(255.0f, MAX(0.0f, px.a * 255.0f));
    }
}

static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static NormalizedPixel LerpPixel(const NormalizedPixel& a, const NormalizedPixel& b, float t) {
    return { lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t) };
}

static void ResampleWorld(PF_EffectWorld* src, PF_EffectWorld* dst, PF_Boolean is_deep) {
    const float scaleX = (float)dst->width / (float)src->width;
    const float scaleY = (float)dst->height / (float)src->height;

    for (int y = 0; y < dst->height; ++y) {
        const float srcY = (y + 0.5f) / scaleY - 0.5f;
        const int y0 = MAX(0, MIN(src->height - 1, (int)floor(srcY)));
        const int y1 = MIN(src->height - 1, y0 + 1);
        const float fy = clamp01(srcY - y0);

        for (int x = 0; x < dst->width; ++x) {
            const float srcX = (x + 0.5f) / scaleX - 0.5f;
            const int x0 = MAX(0, MIN(src->width - 1, (int)floor(srcX)));
            const int x1 = MIN(src->width - 1, x0 + 1);
            const float fx = clamp01(srcX - x0);

            NormalizedPixel c00 = ReadNormalized(src, x0, y0, is_deep);
            NormalizedPixel c10 = ReadNormalized(src, x1, y0, is_deep);
            NormalizedPixel c01 = ReadNormalized(src, x0, y1, is_deep);
            NormalizedPixel c11 = ReadNormalized(src, x1, y1, is_deep);

            NormalizedPixel top = LerpPixel(c00, c10, fx);
            NormalizedPixel bottom = LerpPixel(c01, c11, fx);
            NormalizedPixel result = LerpPixel(top, bottom, fy);

            WriteNormalized(dst, x, y, result, is_deep);
        }
    }
}

// Resample only a destination area with an explicit offset (used for ROI upsample).
static void ResampleWorldArea(
    PF_EffectWorld* src,
    PF_EffectWorld* dst,
    PF_Boolean is_deep,
    const PF_LRect& dst_area)
{
    const float scaleX = (float)(dst_area.right - dst_area.left) / (float)src->width;
    const float scaleY = (float)(dst_area.bottom - dst_area.top) / (float)src->height;

    for (int y = dst_area.top; y < dst_area.bottom; ++y) {
        const float srcY = ((float)(y - dst_area.top) + 0.5f) / scaleY - 0.5f;
        const int y0 = MAX(0, MIN(src->height - 1, (int)floor(srcY)));
        const int y1 = MIN(src->height - 1, y0 + 1);
        const float fy = clamp01(srcY - y0);

        for (int x = dst_area.left; x < dst_area.right; ++x) {
            const float srcX = ((float)(x - dst_area.left) + 0.5f) / scaleX - 0.5f;
            const int x0 = MAX(0, MIN(src->width - 1, (int)floor(srcX)));
            const int x1 = MIN(src->width - 1, x0 + 1);
            const float fx = clamp01(srcX - x0);

            NormalizedPixel c00 = ReadNormalized(src, x0, y0, is_deep);
            NormalizedPixel c10 = ReadNormalized(src, x1, y0, is_deep);
            NormalizedPixel c01 = ReadNormalized(src, x0, y1, is_deep);
            NormalizedPixel c11 = ReadNormalized(src, x1, y1, is_deep);

            NormalizedPixel top = LerpPixel(c00, c10, fx);
            NormalizedPixel bottom = LerpPixel(c01, c11, fx);
            NormalizedPixel result = LerpPixel(top, bottom, fy);

            WriteNormalized(dst, x, y, result, is_deep);
        }
    }
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

// Core CPU processing used by both the legacy PF_Cmd_RENDER path and Smart Render.
static PF_Err
LiteGlowProcess(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_EffectWorld* inputW,
    PF_EffectWorld* outputW,
    const LiteGlowRenderParams* rp,
    const PF_LRect* areaP)
{
    if (!inputW || !outputW || !rp) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    // Working area (full frame by default; SmartFX ROI when provided)
    PF_LRect work_area = { 0, 0, outputW->width, outputW->height };
    if (areaP) {
        work_area = *areaP;
        work_area.left   = MAX(0, MIN(work_area.left, outputW->width));
        work_area.top    = MAX(0, MIN(work_area.top, outputW->height));
        work_area.right  = MAX(work_area.left, MIN(work_area.right, outputW->width));
        work_area.bottom = MAX(work_area.top, MIN(work_area.bottom, outputW->height));
    }

    const float strength = rp->strength;

    // If strength is zero (or near zero), just copy the input to output
    if (strength <= 0.1f) {
        err = PF_COPY(inputW, outputW, NULL, NULL);
        return err;
    }

    float radius_param = rp->radius;
    const float threshold_norm = rp->threshold; // already 0..1
    const int quality = rp->quality;
    const float resolution_factor = rp->resolution_factor;

    // Adjust radius based on downsampling
    float adjusted_radius = radius_param;
    if (resolution_factor < 0.9f) {
        // Scale down the radius for previews to improve performance
        adjusted_radius = radius_param * MAX(0.5f, resolution_factor);
    }

    // For ROI mode, pre-fill output so untouched pixels stay correct.
    if (areaP) {
        ERR(PF_COPY(inputW, outputW, NULL, NULL));
    }

    // Create temporary buffers for processing
    PF_EffectWorld bright_world, blur_h_world, blur_v_world;
    PF_EffectWorld scaled_input, scaled_bright, scaled_blur_h, scaled_blur_v;

    // Create temporary worlds with matching bit depth
    // Use output's depth to ensure consistency
    PF_Boolean is_deep = PF_WORLD_IS_DEEP(outputW);

    ERR(suites.WorldSuite1()->new_world(
        in_data->effect_ref,
        outputW->width,
        outputW->height,
        is_deep,
        &bright_world));

    ERR(suites.WorldSuite1()->new_world(
        in_data->effect_ref,
        outputW->width,
        outputW->height,
        is_deep,
        &blur_h_world));

    ERR(suites.WorldSuite1()->new_world(
        in_data->effect_ref,
        outputW->width,
        outputW->height,
        is_deep,
        &blur_v_world));

    float downsample_scale = 1.0f;
    if (adjusted_radius > 48.0f) {
        downsample_scale = 0.25f;
    }
    else if (adjusted_radius > 24.0f) {
        downsample_scale = 0.5f;
    }

    PF_LRect scaled_area = { 0, 0, 0, 0 };
    PF_Boolean use_scaled = (downsample_scale < 1.0f);
    if (use_scaled) {
        int scaled_width = MAX(1, (int)((work_area.right - work_area.left) * downsample_scale));
        int scaled_height = MAX(1, (int)((work_area.bottom - work_area.top) * downsample_scale));

        ERR(suites.WorldSuite1()->new_world(
            in_data->effect_ref,
            scaled_width,
            scaled_height,
            is_deep,
            &scaled_input));
        ERR(suites.WorldSuite1()->new_world(
            in_data->effect_ref,
            scaled_width,
            scaled_height,
            is_deep,
            &scaled_bright));
        ERR(suites.WorldSuite1()->new_world(
            in_data->effect_ref,
            scaled_width,
            scaled_height,
            is_deep,
            &scaled_blur_h));
        ERR(suites.WorldSuite1()->new_world(
            in_data->effect_ref,
            scaled_width,
            scaled_height,
            is_deep,
            &scaled_blur_v));

        // Scaled area corresponding to ROI
        PF_LRect scaled_area;
        scaled_area.left   = 0;
        scaled_area.top    = 0;
        scaled_area.right  = scaled_width;
        scaled_area.bottom = scaled_height;

        if (!err) {
            // Downsample full ROI; cost is reduced because destination is smaller.
            ResampleWorld(inputW, &scaled_input, is_deep);
        }
    }

    if (!err) {
        // Create glow parameters
        GlowData gdata;
        gdata.strength = strength;
        gdata.threshold = threshold_norm * 255.0f;
        gdata.input = use_scaled ? &scaled_input : inputW;
        gdata.resolution_factor = resolution_factor * (use_scaled ? downsample_scale : 1.0f);

        PF_EffectWorld* bright_dest = use_scaled ? &scaled_bright : &bright_world;
        PF_LRect bright_area = use_scaled ? scaled_area : work_area;
        A_long bright_lines = bright_area.bottom - bright_area.top;

        if (is_deep) {
            ERR(suites.Iterate16Suite2()->iterate(
                in_data,
                0,
                bright_lines,
                gdata.input,
                use_scaled ? &bright_area : &bright_area,
                (void*)&gdata,
                ExtractBrightAreas16,
                bright_dest));
        }
        else {
            ERR(suites.Iterate8Suite2()->iterate(
                in_data,
                0,
                bright_lines,
                gdata.input,
                use_scaled ? &bright_area : &bright_area,
                (void*)&gdata,
                ExtractBrightAreas8,
                bright_dest));
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

            BlurData bdata;
            PF_EffectWorld* blur_source = bright_dest;
            PF_EffectWorld* blur_h_dest = use_scaled ? &scaled_blur_h : &blur_h_world;
            PF_EffectWorld* blur_v_dest = use_scaled ? &scaled_blur_v : &blur_v_world;
            bdata.input = blur_source;
            bdata.radius = kernel_radius;
            bdata.kernel = kernel;
            PF_LRect blur_area = use_scaled ? scaled_area : work_area;
            A_long blur_h_lines = blur_area.bottom - blur_area.top;
            A_long blur_v_lines = blur_area.bottom - blur_area.top;

            if (is_deep) {
                ERR(suites.Iterate16Suite2()->iterate(
                    in_data,
                    0,
                    blur_h_lines,
                    blur_source,
                    &blur_area,
                    (void*)&bdata,
                    GaussianBlurH16,
                    blur_h_dest));
            }
            else {
                ERR(suites.Iterate8Suite2()->iterate(
                    in_data,
                    0,
                    blur_h_lines,
                    blur_source,
                    &blur_area,
                    (void*)&bdata,
                    GaussianBlurH8,
                    blur_h_dest));
            }

            if (!err) {
                // update blur data for vertical pass
                bdata.input = blur_h_dest;

                if (is_deep) {
                    ERR(suites.Iterate16Suite2()->iterate(
                        in_data,
                        0,
                        blur_v_lines,
                        blur_h_dest,
                        &blur_area,
                        (void*)&bdata,
                        GaussianBlurV16,
                        blur_v_dest));
                }
                else {
                    ERR(suites.Iterate8Suite2()->iterate(
                        in_data,
                        0,
                        blur_v_lines,
                        blur_h_dest,
                        &blur_area,
                        (void*)&bdata,
                        GaussianBlurV8,
                        blur_v_dest));
                }

                if (quality == QUALITY_HIGH && !err && strength > 500.0f && resolution_factor > 0.9f) {
                    PF_EffectWorld* extra_world = use_scaled ? &scaled_bright : &bright_world;
                    bdata.input = blur_v_dest;

                    PF_LRect extra_area = use_scaled ? scaled_area : work_area;
                    A_long extra_lines = extra_area.bottom - extra_area.top;

                    if (is_deep) {
                        ERR(suites.Iterate16Suite2()->iterate(
                            in_data,
                            0,
                            extra_lines,
                            blur_v_dest,
                            &extra_area,
                            (void*)&bdata,
                            GaussianBlurH16,
                            extra_world));
                    }
                    else {
                        ERR(suites.Iterate8Suite2()->iterate(
                            in_data,
                            0,
                            extra_lines,
                            blur_v_dest,
                            &extra_area,
                            (void*)&bdata,
                            GaussianBlurH8,
                            extra_world));
                    }

                    if (!err) {
                        bdata.input = extra_world;

                        if (is_deep) {
                            ERR(suites.Iterate16Suite2()->iterate(
                                in_data,
                                0,
                                blur_v_lines,
                                extra_world,
                                &extra_area,
                                (void*)&bdata,
                                GaussianBlurV16,
                                blur_v_dest));
                        }
                        else {
                            ERR(suites.Iterate8Suite2()->iterate(
                                in_data,
                                0,
                                blur_v_lines,
                                extra_world,
                                &extra_area,
                                (void*)&bdata,
                                GaussianBlurV8,
                                blur_v_dest));
                        }
                    }
                }

                if (use_scaled) {
                    ResampleWorldArea(&scaled_blur_v, &blur_v_world, is_deep, work_area);
                }

                // STEP 4: Blend original and glow
                BlendData blend_data;
                blend_data.glow = &blur_v_world;
                blend_data.quality = quality;
                blend_data.strength = strength;

                if (is_deep) {
                    ERR(suites.Iterate16Suite2()->iterate(
                        in_data,
                        0,               // progress base
                        work_area.bottom,          // progress final
                        inputW,          // src (original)
                        areaP ? &work_area : NULL,            // area - ROI when provided
                        (void*)&blend_data, // refcon - blend parameters
                        BlendGlow16,     // pixel function
                        outputW));       // destination
                }
                else {
                    ERR(suites.Iterate8Suite2()->iterate(
                        in_data,
                        0,               // progress base
                        work_area.bottom,          // progress final
                        inputW,          // src (original)
                        areaP ? &work_area : NULL,            // area - ROI when provided
                        (void*)&blend_data, // refcon - blend parameters
                        BlendGlow8,      // pixel function
                        outputW));       // destination
                }
            }
        }

        // Dispose of temporary worlds
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &bright_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_h_world));
        ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &blur_v_world));
        if (use_scaled) {
            ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &scaled_input));
            ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &scaled_bright));
            ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &scaled_blur_h));
            ERR(suites.WorldSuite1()->dispose_world(in_data->effect_ref, &scaled_blur_v));
        }
    }

    return err;
}

// GPU device setup / teardown
#if HAS_HLSL
static PF_Err
GPUDeviceSetup(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_GPUDeviceSetupExtra* extraP)
{
    PF_Err err = PF_Err_NONE;

    AEFX_SuiteScoper<PF_HandleSuite1> handle_suite(
        in_data,
        kPFHandleSuite,
        kPFHandleSuiteVersion1,
        out_data);

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpuDeviceSuite(
        in_data,
        kPFGPUDeviceSuite,
        kPFGPUDeviceSuiteVersion1,
        out_data);

    PF_GPUDeviceInfo device_info;
    AEFX_CLR_STRUCT(device_info);

    ERR(gpuDeviceSuite->GetDeviceInfo(
        in_data->effect_ref,
        extraP->input->device_index,
        &device_info));

    if (extraP->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = handle_suite->host_new_handle(sizeof(LiteGlowDirectXGPUData));
        if (!gpu_dataH) {
            return PF_Err_OUT_OF_MEMORY;
        }

        LiteGlowDirectXGPUData* dx_gpu_data =
            reinterpret_cast<LiteGlowDirectXGPUData*>(*gpu_dataH);
        memset(dx_gpu_data, 0, sizeof(LiteGlowDirectXGPUData));

        dx_gpu_data->context = std::make_shared<DXContext>();
        dx_gpu_data->glowShader = std::make_shared<ShaderObject>();

        DX_ERR(dx_gpu_data->context->Initialize(
            (ID3D12Device*)device_info.devicePV,
            (ID3D12CommandQueue*)device_info.command_queuePV));
        bool loaded = false;

        std::wstring csoPath, sigPath;
        if (GetShaderPath(L"LiteGlowKernel", csoPath, sigPath))
        {
            loaded = dx_gpu_data->context->LoadShader(
                csoPath.c_str(),
                sigPath.c_str(),
                dx_gpu_data->glowShader);
        }

        // Fallback: compile embedded HLSL so GPU path never silently drops to CPU.
        if (!loaded)
        {
            loaded = CompileEmbeddedLiteGlowShader(dx_gpu_data->context, dx_gpu_data->glowShader);
        }

        DX_ERR(loaded);

        extraP->output->gpu_data = gpu_dataH;
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    }

    return err;
}

static PF_Err
GPUDeviceSetdown(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_GPUDeviceSetdownExtra* extraP)
{
    PF_Err err = PF_Err_NONE;

    if (extraP->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = (PF_Handle)extraP->input->gpu_data;
        LiteGlowDirectXGPUData* dx_gpu_data =
            reinterpret_cast<LiteGlowDirectXGPUData*>(*gpu_dataH);

        dx_gpu_data->context.reset();
        dx_gpu_data->glowShader.reset();

        AEFX_SuiteScoper<PF_HandleSuite1> handle_suite(
            in_data,
            kPFHandleSuite,
            kPFHandleSuiteVersion1,
            out_data);

        handle_suite->host_dispose_handle(gpu_dataH);
    }

    return err;
}
#else
static PF_Err
GPUDeviceSetup(
    PF_InData* /*in_data*/,
    PF_OutData* /*out_data*/,
    PF_GPUDeviceSetupExtra* /*extraP*/)
{
    return PF_Err_NONE;
}

static PF_Err
GPUDeviceSetdown(
    PF_InData* /*in_data*/,
    PF_OutData* /*out_data*/,
    PF_GPUDeviceSetdownExtra* /*extraP*/)
{
    return PF_Err_NONE;
}
#endif

// CPU Smart Render entry point – delegates to the shared CPU pipeline.
static PF_Err
SmartRenderCPU(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_PixelFormat /*pixel_format*/,
    PF_EffectWorld* input_worldP,
    PF_EffectWorld* output_worldP,
    PF_SmartRenderExtra* /*extraP*/,
    LiteGlowRenderParams* params,
    const PF_LRect* areaP)
{
    return LiteGlowProcess(in_data, out_data, input_worldP, output_worldP, params, areaP);
}

#if HAS_HLSL
// GPU Smart Render using DirectX compute shader.
static PF_Err
SmartRenderGPU(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_PixelFormat pixel_format,
    PF_EffectWorld* input_worldP,
    PF_EffectWorld* output_worldP,
    PF_SmartRenderExtra* extraP,
    LiteGlowRenderParams* params,
    const PF_LRect* areaP)
{
    PF_Err err = PF_Err_NONE;

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu_suite(
        in_data,
        kPFGPUDeviceSuite,
        kPFGPUDeviceSuiteVersion1,
        out_data);

    if (pixel_format != PF_PixelFormat_GPU_BGRA128) {
        // Fall back to CPU if GPU format is not what we expect.
        return LiteGlowProcess(in_data, out_data, input_worldP, output_worldP, params, areaP);
    }

    PF_GPUDeviceInfo device_info;
    ERR(gpu_suite->GetDeviceInfo(
        in_data->effect_ref,
        extraP->input->device_index,
        &device_info));

    void* src_mem = nullptr;
    ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, input_worldP, &src_mem));

    void* dst_mem = nullptr;
    ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, output_worldP, &dst_mem));

    LiteGlowGPUParams gpuParams{};
    gpuParams.width = input_worldP->width;
    gpuParams.height = input_worldP->height;

    const A_long bytes_per_pixel = 16; // BGRA128
    A_long src_row_bytes = input_worldP->rowbytes;
    A_long dst_row_bytes = output_worldP->rowbytes;

    gpuParams.srcPitch = (int)(src_row_bytes / bytes_per_pixel);
    gpuParams.dstPitch = (int)(dst_row_bytes / bytes_per_pixel);

    // Convert effect parameters from pre-render cache
    gpuParams.strength = params->strength / (float)STRENGTH_MAX;
    gpuParams.threshold = params->threshold; // already 0..1
    gpuParams.radius = params->radius;
    gpuParams.quality = params->quality;

    if (!err && extraP->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = (PF_Handle)extraP->input->gpu_data;
        LiteGlowDirectXGPUData* dx_gpu_data =
            reinterpret_cast<LiteGlowDirectXGPUData*>(*gpu_dataH);

        DXShaderExecution shaderExecution(
            dx_gpu_data->context,
            dx_gpu_data->glowShader,
            3); // CBV + UAV + SRV

        DX_ERR(shaderExecution.SetParamBuffer(&gpuParams, sizeof(LiteGlowGPUParams)));
        DX_ERR(shaderExecution.SetUnorderedAccessView(
            (ID3D12Resource*)dst_mem,
            (UINT)(gpuParams.height * dst_row_bytes)));
        DX_ERR(shaderExecution.SetShaderResourceView(
            (ID3D12Resource*)src_mem,
            (UINT)(gpuParams.height * src_row_bytes)));

        DX_ERR(shaderExecution.Execute(
            (UINT)DivideRoundUpSizeT((size_t)gpuParams.width, 16),
            (UINT)DivideRoundUpSizeT((size_t)gpuParams.height, 16)));
    }
    else {
        // Unsupported GPU framework – fall back to CPU implementation.
        err = LiteGlowProcess(in_data, out_data, input_worldP, output_worldP, params, areaP);
    }

    return err;
}
#else
// No GPU support compiled in – always fall back to CPU.
static PF_Err
SmartRenderGPU(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_PixelFormat /*pixel_format*/,
    PF_EffectWorld* input_worldP,
    PF_EffectWorld* output_worldP,
    PF_SmartRenderExtra* /*extraP*/,
    LiteGlowRenderParams* params,
    const PF_LRect* areaP)
{
    return LiteGlowProcess(in_data, out_data, input_worldP, output_worldP, params, areaP);
}
#endif

// PreRender data cleanup
static void
DisposePreRenderData(void* pre_render_dataPV)
{
    if (pre_render_dataPV) {
        LiteGlowRenderParams* params = reinterpret_cast<LiteGlowRenderParams*>(pre_render_dataPV);
        free(params);
    }
}

// Smart PreRender – cache parameters and declare GPU render possibility.
static PF_Err
PreRender(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_PreRenderExtra* extraP)
{
    PF_Err err = PF_Err_NONE;
    PF_CheckoutResult in_result;
    PF_RenderRequest req = extraP->input->output_request;

    // Hint AE that GPU render is possible for this frame.
    extraP->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

    LiteGlowRenderParams* infoP = reinterpret_cast<LiteGlowRenderParams*>(
        malloc(sizeof(LiteGlowRenderParams)));

    if (!infoP) {
        return PF_Err_OUT_OF_MEMORY;
    }

    // Query parameters at pre-render time
    PF_ParamDef cur_param;

    AEFX_CLR_STRUCT(cur_param);
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_STRENGTH,
        in_data->current_time, in_data->time_step, in_data->time_scale, &cur_param));
    infoP->strength = cur_param.u.fs_d.value;

    AEFX_CLR_STRUCT(cur_param);
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_RADIUS,
        in_data->current_time, in_data->time_step, in_data->time_scale, &cur_param));
    infoP->radius = cur_param.u.fs_d.value;

    AEFX_CLR_STRUCT(cur_param);
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_THRESHOLD,
        in_data->current_time, in_data->time_step, in_data->time_scale, &cur_param));
    infoP->threshold = cur_param.u.fs_d.value / 255.0f;

    AEFX_CLR_STRUCT(cur_param);
    ERR(PF_CHECKOUT_PARAM(in_data, LITEGLOW_QUALITY,
        in_data->current_time, in_data->time_step, in_data->time_scale, &cur_param));
    infoP->quality = cur_param.u.pd.value;

    // Downsample info (SmartFX coordinates)
    const float downscale_x = static_cast<float>(in_data->downsample_x.den) / static_cast<float>(in_data->downsample_x.num);
    const float downscale_y = static_cast<float>(in_data->downsample_y.den) / static_cast<float>(in_data->downsample_y.num);
    infoP->resolution_factor = MIN(downscale_x, downscale_y);

    extraP->output->pre_render_data = infoP;
    extraP->output->delete_pre_render_data_func = DisposePreRenderData;

    // ------------------------------------------------------------------
    // ROI tightening: expand only by the blur kernel footprint so AE will
    // allocate/checkout the minimum rectangle for SmartFX processing.
    // ------------------------------------------------------------------
    const float adjusted_radius =
        (infoP->resolution_factor < 0.9f)
        ? infoP->radius * MAX(0.5f, infoP->resolution_factor)
        : infoP->radius;

    float sigma = adjusted_radius;
    switch (infoP->quality) {
    case QUALITY_LOW:    sigma = adjusted_radius * 0.5f; break;
    case QUALITY_MEDIUM: sigma = adjusted_radius * 0.75f; break;
    case QUALITY_HIGH:
    default:             sigma = adjusted_radius; break;
    }

    int kernel_radius = MIN(KERNEL_SIZE_MAX / 2, (int)(3.0f * sigma + 0.5f));
    const A_long margin = (A_long)MAX(1, kernel_radius); // safe expansion per axis

    PF_LRect requested = req.rect;
    requested.left   -= margin;
    requested.top    -= margin;
    requested.right  += margin;
    requested.bottom += margin;
    req.rect = requested;

    // Checkout input to compute (clipped) result rectangles
    ERR(extraP->cb->checkout_layer(
        in_data->effect_ref,
        LITEGLOW_INPUT,
        LITEGLOW_INPUT,
        &req,
        in_data->current_time,
        in_data->time_step,
        in_data->time_scale,
        &in_result));

    extraP->output->result_rect     = in_result.result_rect;
    extraP->output->max_result_rect = in_result.max_result_rect;

    return err;
}

// Smart Render dispatcher – chooses CPU or GPU path.
static PF_Err
SmartRender(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_SmartRenderExtra* extraP,
    bool isGPU)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

    PF_EffectWorld* input_worldP = nullptr;
    PF_EffectWorld* output_worldP = nullptr;

    LiteGlowRenderParams* infoP =
        reinterpret_cast<LiteGlowRenderParams*>(extraP->input->pre_render_data);
    const PF_LRect* areaP = &extraP->output->result_rect;

    if (!infoP) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    ERR(extraP->cb->checkout_layer_pixels(
        in_data->effect_ref, LITEGLOW_INPUT, &input_worldP));
    ERR(extraP->cb->checkout_output(
        in_data->effect_ref, &output_worldP));

    if (!err) {
        AEFX_SuiteScoper<PF_WorldSuite2> world_suite(
            in_data,
            kPFWorldSuite,
            kPFWorldSuiteVersion2,
            out_data);

        PF_PixelFormat pixel_format = PF_PixelFormat_INVALID;
        ERR(world_suite->PF_GetPixelFormat(input_worldP, &pixel_format));

        if (!err) {
            if (isGPU) {
                ERR(SmartRenderGPU(in_data, out_data, pixel_format, input_worldP, output_worldP, extraP, infoP, areaP));
            }
            else {
                ERR(SmartRenderCPU(in_data, out_data, pixel_format, input_worldP, output_worldP, extraP, infoP, areaP));
            }
        }
    }

    err2 = extraP->cb->checkin_layer_pixels(in_data->effect_ref, LITEGLOW_INPUT);
    return err;
}

static PF_Err
Render(
    PF_InData* in_data,
    PF_OutData* out_data,
    PF_ParamDef* params[],
    PF_LayerDef* output)
{
    LiteGlowRenderParams rp;

    // Get user parameters
    rp.strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
    rp.radius = params[LITEGLOW_RADIUS]->u.fs_d.value;
    rp.threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value / 255.0f;
    rp.quality = params[LITEGLOW_QUALITY]->u.pd.value;

    // Handle downsampling for preview
    float downscale_x = static_cast<float>(in_data->downsample_x.den) / static_cast<float>(in_data->downsample_x.num);
    float downscale_y = static_cast<float>(in_data->downsample_y.den) / static_cast<float>(in_data->downsample_y.num);
    rp.resolution_factor = MIN(downscale_x, downscale_y);

    PF_EffectWorld* inputW = reinterpret_cast<PF_EffectWorld*>(&params[LITEGLOW_INPUT]->u.ld);
    PF_EffectWorld* outputW = reinterpret_cast<PF_EffectWorld*>(output);

    return LiteGlowProcess(in_data, out_data, inputW, outputW, &rp, NULL);
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
        "361do LiteGlow",     // Match Name
        "361do_plugins",          // Category
        AE_RESERVED_INFO,    // Reserved Info
        "EffectMain",        // Entry point
        "https://github.com/rebuildup/Ae_LiteGlow"); // Support URL

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
            err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
            break;

        case PF_Cmd_SMART_RENDER:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, false);
            break;

        case PF_Cmd_SMART_RENDER_GPU:
            err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra, true);
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
#include <algorithm>
#include <cmath>
