#include "PrGPU/KernelSupport/KernelCore.h"
#include "PrGPU/KernelSupport/KernelMemory.h"

#if GF_DEVICE_TARGET_DEVICE

// Simple glow kernel used for the GPU path of LiteGlow.
// It thresholds bright areas and applies a small gaussian-like blur,
// then screens the result over the original image.
GF_KERNEL_FUNCTION(LiteGlowKernel,
	((GF_PTR_READ_ONLY(float4))(inSrc))
	((GF_PTR(float4))(outDst)),
	((int)(inSrcPitch))
	((int)(inDstPitch))
	((int)(in16f))
	((unsigned int)(inWidth))
	((unsigned int)(inHeight))
	((float)(inStrength))
	((float)(inThreshold))
	((float)(inRadius))
	((int)(inQuality)),
	((uint2)(inXY)(KERNEL_XY)))
{
	if (inXY.x >= inWidth || inXY.y >= inHeight) {
		return;
	}

	const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);

	const float strength = max(inStrength, 0.0f);
	const float threshold = clamp(inThreshold, 0.0f, 1.0f);
	const float radius = clamp(inRadius, 1.0f, 32.0f);

	if (strength <= 0.0f) {
		float4 srcNoGlow = ReadFloat4(inSrc, inXY.y * inSrcPitch + inXY.x, !!in16f);
		WriteFloat4(srcNoGlow, outDst, inXY.y * inDstPitch + inXY.x, !!in16f);
		return;
	}

	// Base pixel
	float4 src = ReadFloat4(inSrc, inXY.y * inSrcPitch + inXY.x, !!in16f);
	float baseLuma = dot(src.xyz, lumaWeights);

	// Mask based on local brightness vs. threshold
	float mask = saturate((baseLuma - threshold) * 4.0f);
	if (mask <= 0.0f) {
		WriteFloat4(src, outDst, inXY.y * inDstPitch + inXY.x, !!in16f);
		return;
	}

	const int r = (int)radius;

	// Quality affects sampling density.
	int step = 1;
	if (inQuality <= 1) {
		step = 3;
	} else if (inQuality == 2) {
		step = 2;
	}

	const float sigma = radius * 0.75f;
	const float twoSigma2 = 2.0f * sigma * sigma;

	float3 glowAccum = float3(0.0f, 0.0f, 0.0f);
	float weightAccum = 0.0f;

	for (int dy = -r; dy <= r; dy += step) {
		int iy = (int)inXY.y + dy;
		if (iy < 0) {
			iy = 0;
		}
		else if ((unsigned int)iy >= inHeight) {
			iy = (int)inHeight - 1;
		}

		for (int dx = -r; dx <= r; dx += step) {
			int ix = (int)inXY.x + dx;
			if (ix < 0) {
				ix = 0;
			}
			else if ((unsigned int)ix >= inWidth) {
				ix = (int)inWidth - 1;
			}

			const float dist2 = (float)(dx * dx + dy * dy);
			const float w = exp(-dist2 / twoSigma2);

			float4 p = ReadFloat4(inSrc, iy * inSrcPitch + ix, !!in16f);
			float l = dot(p.xyz, lumaWeights);

			float localMask = l > threshold ? 1.0f : 0.0f;

			glowAccum += p.xyz * (w * localMask);
			weightAccum += w * localMask;
		}
	}

	float3 glow = (weightAccum > 0.0f) ? glowAccum / weightAccum : float3(0.0f, 0.0f, 0.0f);

	// Scale glow by strength and clamp
	glow *= strength;
	glow = saturate(glow);

	// Screen blend
	float3 screenColor = 1.0f - (1.0f - src.xyz) * (1.0f - glow);

	float4 outPixel;
	outPixel.xyz = lerp(src.xyz, screenColor, mask);
	outPixel.w = src.w;

	WriteFloat4(outPixel, outDst, inXY.y * inDstPitch + inXY.x, !!in16f);
}

#endif // GF_DEVICE_TARGET_DEVICE

