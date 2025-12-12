/*
 * Minimal stub of KernelWrapper from Adobe GPUUtils.
 * We intentionally leave GF_KERNEL_FUNCTION undefined so that the macro
 * calls remain in the preprocessed output for ParseHLSL.py to detect.
 */
#pragma once

/* No definitions needed; presence of this file prevents falling back to
 * SDK's KernelWrapper.h that requires Boost. */

