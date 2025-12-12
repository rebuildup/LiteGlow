/*
 * Minimal PrGPU KernelMemory stubs for HLSL preprocessing.
 *
 * These provide no real GPU memory semantics; ParseHLSL.py rewrites memory
 * access for the generated per-kernel .hlsl files. The definitions here are
 * just to keep preprocessing resilient when SDK headers are unavailable.
 */

#pragma once

/* Read/Write helpers used in kernels. Provide simple HLSL-friendly forms. */
#ifndef ReadFloat4
#define ReadFloat4(buffer, index, is16f) (buffer[index])
#endif

#ifndef WriteFloat4
#define WriteFloat4(value, buffer, index, is16f) do { buffer[index] = value; } while(0)
#endif

