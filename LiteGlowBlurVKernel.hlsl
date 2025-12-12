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
    const int r = mRadius;

    float4 sum = 0.0f;
    int count = 0;

    [loop]
    for (int j = -r; j <= r; ++j)
    {
        const int sy = clamp(y + j, 0, (int)mHeight - 1);
        const uint idx = (uint)sy * (uint)mSrcPitch + (uint)x;
        sum += inSrc[idx];
        count++;
    }

    const uint outIdx = (uint)y * (uint)mDstPitch + (uint)x;
    outDst[outIdx] = sum / (float)count;
}

