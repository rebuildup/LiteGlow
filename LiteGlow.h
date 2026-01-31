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

// Blend mode settings
#define BLEND_MODE_SCREEN    1
#define BLEND_MODE_ADD       2
#define BLEND_MODE_NORMAL    3
#define BLEND_MODE_NUM_CHOICES 3
#define BLEND_MODE_DFLT      BLEND_MODE_SCREEN

// Bloom intensity settings
#define BLOOM_INTENSITY_MIN   0
#define BLOOM_INTENSITY_MAX   400
#define BLOOM_INTENSITY_DFLT  150  // Represents 1.5x when divided by 100

// Threshold softness (knee) settings
#define KNEE_MIN   0
#define KNEE_MAX   100
#define KNEE_DFLT  10  // Represents 0.1 when divided by 100

enum {
    LITEGLOW_INPUT = 0,
    LITEGLOW_STRENGTH,
    LITEGLOW_RADIUS,
    LITEGLOW_THRESHOLD,
    LITEGLOW_QUALITY,
    LITEGLOW_BLOOM_INTENSITY,
    LITEGLOW_KNEE,
    LITEGLOW_BLEND_MODE,
    LITEGLOW_TINT_COLOR,
    LITEGLOW_NUM_PARAMS
};

enum {
    STRENGTH_DISK_ID = 1,
    RADIUS_DISK_ID,
    THRESHOLD_DISK_ID,
    QUALITY_DISK_ID,
    BLOOM_INTENSITY_DISK_ID,
    KNEE_DISK_ID,
    BLEND_MODE_DISK_ID,
    TINT_COLOR_DISK_ID
};

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

    DllExport PF_Err PluginDataEntryFunction2(
        PF_PluginDataPtr inPtr,
        PF_PluginDataCB2 inPluginDataCallBackPtr,
        SPBasicSuite *inSPBasicSuitePtr,
        const char *inHostName,
        const char *inHostVersion);
}

#endif // LITEGLOW_H
