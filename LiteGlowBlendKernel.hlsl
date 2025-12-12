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

    const uint factor = (uint)max(1, mFactor);
    const uint glowW = max(1u, mWidth / factor);
    const uint glowH = max(1u, mHeight / factor);

    // Bilinear upsample from glow buffer.
    const float gx = ((float)x + 0.5f) / (float)factor - 0.5f;
    const float gy = ((float)y + 0.5f) / (float)factor - 0.5f;
    const int x0 = clamp((int)floor(gx), 0, (int)glowW - 1);
    const int y0 = clamp((int)floor(gy), 0, (int)glowH - 1);
    const int x1 = min(x0 + 1, (int)glowW - 1);
    const int y1 = min(y0 + 1, (int)glowH - 1);
    const float fx = saturate(gx - (float)x0);
    const float fy = saturate(gy - (float)y0);

    const uint row0 = (uint)y0 * (uint)mGlowPitch;
    const uint row1 = (uint)y1 * (uint)mGlowPitch;
    float4 g00 = inGlow[row0 + (uint)x0];
    float4 g10 = inGlow[row0 + (uint)x1];
    float4 g01 = inGlow[row1 + (uint)x0];
    float4 g11 = inGlow[row1 + (uint)x1];

    float4 glow = lerp(lerp(g00, g10, fx), lerp(g01, g11, fx), fy);

    // Stable strength mapping (prevents breakage at high Strength):
    // g' = (g*s) / (1 + g*s)   in [0,1)
    float3 g = glow.xyz * mStrength;
    g = g / (1.0f + g);

    // Screen: 1 - (1-a)(1-b)
    float3 rgb = 1.0f - (1.0f - original.xyz) * (1.0f - g);
    outDst[y * (uint)mDstPitch + x] = float4(rgb, original.w);
}
