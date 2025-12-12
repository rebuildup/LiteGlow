/*
 * Minimal PrGPU KernelSupport stubs.
 *
 * The official After Effects SDK ships these headers under GPUUtils/PrGPU.
 * GitHub Actions environments sometimes omit that folder, which breaks the
 * HLSL preprocessing step (cl.exe /P) used by ParseHLSL.py.
 *
 * These stubs are only meant to allow preprocessing and kernel extraction.
 * They intentionally avoid defining GF_KERNEL_FUNCTION so ParseHLSL can
 * detect kernel entrypoints in the preprocessed output.
 */

#pragma once

/* Target defines used by SDK kernels */
#ifndef GF_DEVICE_TARGET_HLSL
#define GF_DEVICE_TARGET_HLSL 0
#endif

#ifndef GF_DEVICE_TARGET_DEVICE
#if GF_DEVICE_TARGET_HLSL
#define GF_DEVICE_TARGET_DEVICE 1
#else
#define GF_DEVICE_TARGET_DEVICE 0
#endif
#endif

/* Pointer qualifier macros used in kernel argument lists */
#ifndef GF_PTR_READ_ONLY
#define GF_PTR_READ_ONLY(T) T
#endif

#ifndef GF_PTR
#define GF_PTR(T) T
#endif

/* Thread ID annotation placeholder */
#ifndef KERNEL_XY
#define KERNEL_XY
#endif

/* Small helpers referenced by kernels; keep simple for preprocessing */
#ifndef make_float4
#define make_float4(x,y,z,w) float4(x,y,z,w)
#endif

