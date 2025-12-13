// LiteGlowBlurHKernel.hlsl

#define LITEGLOW_RS "CBV(b0), DescriptorTable(UAV(u0)), DescriptorTable(SRV(t0))"

cbuffer BlurParams : register(b0)
{
    int mSrcPitch;
    int mDstPitch;
    int m16f;
    uint mWidth;
    uint mHeight;
    int mRadius;
    int mPadding0;
    int mPadding1;
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
    float4 v = asfloat(u);
    if (any(isnan(v)) || any(isinf(v))) {
        v = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return v;
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

    const int x = (int)dtid.x;
    const int y = (int)dtid.y;
    const int r = max(1, mRadius);

    // 9-tap Gaussian-ish blur with variable step (more stable on text edges).
    const int step = max(1, r / 3);
    const float w0 = 0.050f;
    const float w1 = 0.090f;
    const float w2 = 0.120f;
    const float w3 = 0.150f;
    const float w4 = 0.180f;

    float4 sum = 0.0f;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x - 4 * step, 0, (int)mWidth - 1), (uint)y)) * w0;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x - 3 * step, 0, (int)mWidth - 1), (uint)y)) * w1;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x - 2 * step, 0, (int)mWidth - 1), (uint)y)) * w2;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x - 1 * step, 0, (int)mWidth - 1), (uint)y)) * w3;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)x, (uint)y)) * w4;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x + 1 * step, 0, (int)mWidth - 1), (uint)y)) * w3;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x + 2 * step, 0, (int)mWidth - 1), (uint)y)) * w2;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x + 3 * step, 0, (int)mWidth - 1), (uint)y)) * w1;
    sum += LoadF4(inSrc, ByteOffset((uint)mSrcPitch, (uint)clamp(x + 4 * step, 0, (int)mWidth - 1), (uint)y)) * w0;

    StoreF4(outDst, ByteOffset((uint)mDstPitch, (uint)x, (uint)y), sum);
}
