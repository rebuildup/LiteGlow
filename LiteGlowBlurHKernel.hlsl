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

StructuredBuffer<float4> inSrc : register(t0);
RWStructuredBuffer<float4> outDst : register(u0);

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

    // Fast 5-tap separable blur with adjustable step to approximate a wider radius.
    // Weights are binomial (1 4 6 4 1)/16.
    const int step = max(1, r / 2);
    const float w0 = 0.0625f;
    const float w1 = 0.25f;
    const float w2 = 0.375f;

    const int x0 = clamp(x - 2 * step, 0, (int)mWidth - 1);
    const int x1 = clamp(x - step,     0, (int)mWidth - 1);
    const int x2 = x;
    const int x3 = clamp(x + step,     0, (int)mWidth - 1);
    const int x4 = clamp(x + 2 * step, 0, (int)mWidth - 1);

    const uint rowBase = (uint)y * (uint)mSrcPitch;
    float4 sum =
        inSrc[rowBase + (uint)x0] * w0 +
        inSrc[rowBase + (uint)x1] * w1 +
        inSrc[rowBase + (uint)x2] * w2 +
        inSrc[rowBase + (uint)x3] * w1 +
        inSrc[rowBase + (uint)x4] * w0;

    outDst[(uint)y * (uint)mDstPitch + (uint)x] = sum;
}
