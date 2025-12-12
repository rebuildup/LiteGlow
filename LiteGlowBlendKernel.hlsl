// LiteGlowBlendKernel.hlsl

#define LITEGLOW_RS "CBV(b0), DescriptorTable(UAV(u0)), DescriptorTable(SRV(t0)), DescriptorTable(SRV(t1))"

cbuffer BlendParams : register(b0)
{
    int mSrcPitch;
    int mGlowPitch;
    int mDstPitch;
    int m16f;
    uint mWidth;
    uint mHeight;
    float mStrength;
    int mFactor;
};

StructuredBuffer<float4> inSrc : register(t0);
StructuredBuffer<float4> inGlow : register(t1);
RWStructuredBuffer<float4> outDst : register(u0);

[numthreads(16, 16, 1)]
[RootSignature(LITEGLOW_RS)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= mWidth || dtid.y >= mHeight) {
        return;
    }

    const uint x = dtid.x;
    const uint y = dtid.y;

    const uint srcIdx = y * (uint)mSrcPitch + x;
    float4 original = inSrc[srcIdx];

    const uint gx = x / (uint)mFactor;
    const uint gy = y / (uint)mFactor;
    const uint glowIdx = gy * (uint)mGlowPitch + gx;
    float4 glow = inGlow[glowIdx] * mStrength;

    // Screen: 1 - (1-a)(1-b)
    float3 rgb = 1.0f - (1.0f - original.xyz) * (1.0f - glow.xyz);
    outDst[y * (uint)mDstPitch + x] = float4(rgb, original.w);
}

