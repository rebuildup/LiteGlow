// LiteGlowBlurVKernel.hlsl

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

    const int step = max(1, r / 2);
    const float w0 = 0.0625f;
    const float w1 = 0.25f;
    const float w2 = 0.375f;

    const int y0 = clamp(y - 2 * step, 0, (int)mHeight - 1);
    const int y1 = clamp(y - step,     0, (int)mHeight - 1);
    const int y2 = y;
    const int y3 = clamp(y + step,     0, (int)mHeight - 1);
    const int y4 = clamp(y + 2 * step, 0, (int)mHeight - 1);

    const uint col = (uint)x;
    float4 sum =
        inSrc[(uint)y0 * (uint)mSrcPitch + col] * w0 +
        inSrc[(uint)y1 * (uint)mSrcPitch + col] * w1 +
        inSrc[(uint)y2 * (uint)mSrcPitch + col] * w2 +
        inSrc[(uint)y3 * (uint)mSrcPitch + col] * w1 +
        inSrc[(uint)y4 * (uint)mSrcPitch + col] * w0;

    outDst[(uint)y * (uint)mDstPitch + col] = sum;
}
