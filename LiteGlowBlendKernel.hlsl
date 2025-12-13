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

ByteAddressBuffer inSrc : register(t0);
ByteAddressBuffer inGlow : register(t1);
RWByteAddressBuffer outDst : register(u0);

static uint ByteOffset(uint pitch, uint x, uint y)
{
    return ((y * pitch) + x) * 16u;
}

static float4 LoadF4(ByteAddressBuffer b, uint byteOffset)
{
    uint4 u = b.Load4(byteOffset);
    return asfloat(u);
}

static void StoreF4(RWByteAddressBuffer b, uint byteOffset, float4 v)
{
    b.Store4(byteOffset, asuint(v));
}

[numthreads(16, 16, 1)]
[RootSignature(LITEGLOW_RS)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= mWidth || dtid.y >= mHeight) {
        return;
    }

    const uint x = dtid.x;
    const uint y = dtid.y;

    float4 original = LoadF4(inSrc, ByteOffset((uint)mSrcPitch, x, y));

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

    float4 g00 = LoadF4(inGlow, ByteOffset((uint)mGlowPitch, (uint)x0, (uint)y0));
    float4 g10 = LoadF4(inGlow, ByteOffset((uint)mGlowPitch, (uint)x1, (uint)y0));
    float4 g01 = LoadF4(inGlow, ByteOffset((uint)mGlowPitch, (uint)x0, (uint)y1));
    float4 g11 = LoadF4(inGlow, ByteOffset((uint)mGlowPitch, (uint)x1, (uint)y1));

    float4 glow = lerp(lerp(g00, g10, fx), lerp(g01, g11, fx), fy);

    // Stable HDR-to-display mapping:
    // g' = 1 - exp(-g*s)  (film-like, avoids hard clipping at high Strength)
    float3 g = max(0.0f, glow.xyz * mStrength);
    g = 1.0f - exp(-g);

    // Screen: 1 - (1-a)(1-b)
    float3 rgb = 1.0f - (1.0f - original.xyz) * (1.0f - g);

    // Hard clip to display white (user requirement).
    // Note: clamp also acts as a safety net against out-of-range values at high Strength.
    if (any(isnan(rgb)) || any(isinf(rgb))) {
        rgb = 0.0f;
    }
    rgb = saturate(rgb);

    StoreF4(outDst, ByteOffset((uint)mDstPitch, x, y), float4(rgb, original.w));
}
