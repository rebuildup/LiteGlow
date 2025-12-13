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

ByteAddressBuffer inSrc : register(t0);
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

    const uint factor = (uint)max(1, mFactor);
    const uint srcW = max(1u, mWidth * factor);
    const uint srcH = max(1u, mHeight * factor);
    const uint srcX0 = dtid.x * factor;
    const uint srcY0 = dtid.y * factor;

    // Box downsample (factor x factor). This reduces aliasing artifacts when ds>1.
    float4 pixel = 0.0f;
    uint count = 0;
    [loop]
    for (uint j = 0; j < factor; ++j)
    {
        const uint sy = min(srcY0 + j, srcH - 1u);
        [loop]
        for (uint i = 0; i < factor; ++i)
        {
            const uint sx = min(srcX0 + i, srcW - 1u);
            pixel += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, sx, sy));
            count++;
        }
    }
    pixel *= (1.0f / (float)count);

    // BGRA: x=B, y=G, z=R, w=A
    const float luma = pixel.z * 0.299f + pixel.y * 0.587f + pixel.x * 0.114f;

    // Soft knee around threshold (avoids banding at the edge).
    const float knee = 0.1f;
    const float t0 = mThreshold - knee;
    const float t1 = mThreshold + knee;
    float contrib = 0.0f;
    if (luma > t0)
    {
        if (luma >= t1) {
            contrib = luma - mThreshold;
        } else {
            const float tt = (luma - t0) / (t1 - t0);
            contrib = (tt * tt * (3.0f - 2.0f * tt)) * max(0.0f, luma - mThreshold);
        }
    }

    float4 result = float4(0.0f, 0.0f, 0.0f, pixel.w);
    if (contrib > 0.0f)
    {
        result.xyz = pixel.xyz * (mStrength * contrib);
    }

    StoreF4(outDst, ByteOffset((uint)mDstPitch, dtid.x, dtid.y), result);
}
