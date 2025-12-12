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

    const uint srcX = dtid.x * (uint)mFactor;
    const uint srcY = dtid.y * (uint)mFactor;
    const uint srcIdx = srcY * (uint)mSrcPitch + srcX;

    float4 pixel = inSrc[srcIdx];

    // BGRA: x=B, y=G, z=R, w=A
    const float luma = pixel.z * 0.299f + pixel.y * 0.587f + pixel.x * 0.114f;

    float4 result = float4(0.0f, 0.0f, 0.0f, pixel.w);
    if (luma > mThreshold)
    {
        const float diff = luma - mThreshold;
        const float knee = diff / (1.0f + diff);
        const float scale = knee * mStrength;
        result.xyz = min(1.0f, pixel.xyz * scale);
    }

    const uint dstIdx = dtid.y * (uint)mDstPitch + dtid.x;
    outDst[dstIdx] = result;
}

