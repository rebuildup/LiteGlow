// LiteGlowBrightPassKernel.hlsl

#define LITEGLOW_RS "CBV(b0), DescriptorTable(UAV(u0)), DescriptorTable(SRV(t0))"

cbuffer BrightPassParams : register(b0)
{
    int mSrcPitch;
    int mDstPitch;
    int m16f;
    uint mWidth;
    uint mHeight;
    float mThreshold;
    float mStrength;
    int mFactor;
};

StructuredBuffer<float4> inSrc : register(t0);
RWStructuredBuffer<float4> outDst : register(u0);

[numthreads(16, 16, 1)]
[RootSignature(LITEGLOW_RS)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= mWidth || dtid.y >= mHeight) {
        return;
    }

    const uint factor = (uint)max(1, mFactor);
    const uint srcX0 = dtid.x * factor;
    const uint srcY0 = dtid.y * factor;

    // Box downsample (factor x factor). This reduces aliasing artifacts when ds>1.
    float4 pixel = 0.0f;
    uint count = 0;
    [loop]
    for (uint j = 0; j < factor; ++j)
    {
        const uint sy = srcY0 + j;
        [loop]
        for (uint i = 0; i < factor; ++i)
        {
            const uint sx = srcX0 + i;
            const uint idx = sy * (uint)mSrcPitch + sx;
            pixel += inSrc[idx];
            count++;
        }
    }
    pixel *= (1.0f / (float)count);

    // BGRA: x=B, y=G, z=R, w=A
    const float luma = pixel.z * 0.299f + pixel.y * 0.587f + pixel.x * 0.114f;

    float4 result = float4(0.0f, 0.0f, 0.0f, pixel.w);
    if (luma > mThreshold)
    {
        const float diff = luma - mThreshold;
        const float knee = diff / (1.0f + diff);
        const float scale = knee * mStrength;
        // Allow HDR glow (do not clamp here); the blend stage will apply tone mapping.
        result.xyz = pixel.xyz * scale;
    }

    const uint dstIdx = dtid.y * (uint)mDstPitch + dtid.x;
    outDst[dstIdx] = result;
}
