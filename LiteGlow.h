#pragma once

#ifndef LITEGLOW_H
#define LITEGLOW_H

#define PF_DEEP_COLOR_AWARE 1

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

/* Define PF_TABLE_BITS before including AEFX_ChannelDepthTpl.h */
#define PF_TABLE_BITS	12
#define PF_TABLE_SZ_16	4096

#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"

#include "LiteGlow_Strings.h"

// Version constants
#define MAJOR_VERSION        1
#define MINOR_VERSION        0
#define BUG_VERSION          0
#define STAGE_VERSION        PF_Stage_DEVELOP
#define BUILD_VERSION        1
#define LITEGLOW_VERSION_VALUE 528385

#define STRENGTH_MIN       0
#define STRENGTH_MAX       2000
#define STRENGTH_DFLT      800

#define RADIUS_MIN         1
#define RADIUS_MAX         50
#define RADIUS_DFLT        10

#define THRESHOLD_MIN      0
#define THRESHOLD_MAX      255
#define THRESHOLD_DFLT     80

// Quality settings
#define QUALITY_LOW        1
#define QUALITY_MEDIUM     2
#define QUALITY_HIGH       3
#define QUALITY_NUM_CHOICES 3
#define QUALITY_DFLT       QUALITY_MEDIUM

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

// Structure definitions for internal processing
typedef struct {
    A_long sequence_id;
    int gaussKernelSize;
    int kernelRadius;
    int quality;
    float sigma;
    float gaussKernel[128];
} LiteGlowSequenceData;

typedef struct {
    float strength;
    float threshold;
    float resolution_factor;
    PF_EffectWorld* input;
} GlowData;

typedef GlowData* GlowDataP;

typedef struct {
    PF_EffectWorld* input;
    float* kernel;
    int radius;
} BlurData;

typedef BlurData* BlurDataP;

typedef struct {
    PF_EffectWorld* glow;
    int quality;
    float strength;
} BlendData;

typedef BlendData* BlendDataP;

extern "C" {
    DllExport
    PF_Err
    EffectMain(
        PF_Cmd          cmd,
        PF_InData       *in_data,
        PF_OutData      *out_data,
        PF_ParamDef     *params[],
        PF_LayerDef     *output,
        void            *extra);
}

#endif // LITEGLOW_H
