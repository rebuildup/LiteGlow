#pragma once

#ifndef LITEGLOW_H
#define LITEGLOW_H

typedef unsigned char        u_char;
typedef unsigned short       u_short;
typedef unsigned short       u_int16;
typedef unsigned long        u_long;
typedef short int            int16;
#define PF_TABLE_BITS    12
#define PF_TABLE_SZ_16   4096

#define PF_DEEP_COLOR_AWARE 1   // make sure we get 16bpc pixels; 
// AE_Effect.h checks for this.

#include "AEConfig.h"

#ifdef AE_OS_WIN
typedef unsigned short PixelType;
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"

#include "LiteGlow_Strings.h"

/* Versioning information */
#define    MAJOR_VERSION    1
#define    MINOR_VERSION    2
#define    BUG_VERSION      0
#define    STAGE_VERSION    PF_Stage_DEVELOP
#define    BUILD_VERSION    1

/* Parameter defaults and limits */
#define    STRENGTH_MIN       0
#define    STRENGTH_MAX       3000   // Increased to 3000 as requested
#define    STRENGTH_DFLT      800    // Default value

#define    RADIUS_MIN         1
#define    RADIUS_MAX         50
#define    RADIUS_DFLT        10

#define    THRESHOLD_MIN      0
#define    THRESHOLD_MAX      255
#define    THRESHOLD_DFLT     80

/* Quality settings */
#define    QUALITY_LOW        1
#define    QUALITY_MEDIUM     2
#define    QUALITY_HIGH       3
#define    QUALITY_NUM_CHOICES 3
#define    QUALITY_DFLT       QUALITY_MEDIUM

/* Maximum kernel size for Gaussian blur */
#define    KERNEL_SIZE_MAX    64

enum {
    LITEGLOW_INPUT = 0,
    LITEGLOW_STRENGTH,
    LITEGLOW_RADIUS,
    LITEGLOW_THRESHOLD,
    LITEGLOW_QUALITY,
    LITEGLOW_NUM_PARAMS
};

enum {
    STRENGTH_DISK_ID = 1,
    RADIUS_DISK_ID,
    THRESHOLD_DISK_ID,
    QUALITY_DISK_ID
};

// Sequence data for caching information between renders
typedef struct {
    A_long sequence_id;      // Unique ID for this sequence
    float gaussKernel[KERNEL_SIZE_MAX * 2 + 1]; // Cached Gaussian kernel
    int gaussKernelSize;     // Size of kernel
    int kernelRadius;        // Radius of kernel
    float sigma;             // Sigma value used for kernel
    int quality;             // Quality setting
} LiteGlowSequenceData;

// Structure for glow parameters
typedef struct {
    float strength;          // Glow strength
    float threshold;         // Brightness threshold
    PF_EffectWorldPtr input; // Input image for reference
    float resolution_factor; // Downsampling factor
} GlowData, * GlowDataP;

// Structure for blur parameters
typedef struct {
    PF_EffectWorldPtr input;    // Source image
    int radius;                 // Blur radius
    float* kernel;              // Gaussian kernel
} BlurData, * BlurDataP;

// Structure for blend parameters
typedef struct {
    PF_EffectWorldPtr glow;     // Blurred glow image
    int quality;                // Quality setting
} BlendData, * BlendDataP;

extern "C" {

    DllExport
        PF_Err
        EffectMain(
            PF_Cmd          cmd,
            PF_InData* in_data,
            PF_OutData* out_data,
            PF_ParamDef* params[],
            PF_LayerDef* output,
            void* extra);

}

#endif // LITEGLOW_H