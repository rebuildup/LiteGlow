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

// Maximum kernel size for Gaussian blur
#define KERNEL_SIZE_MAX    64

enum {
    LITEGLOW_INPUT = 0,
    LITEGLOW_STRENGTH,
    LITEGLOW_RADIUS,
    LITEGLOW_THRESHOLD,
    LITEGLOW_QUALITY,
    LITEGLOW_NUM_PARAMS
};

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
    LITEGLOW_QUALITY,
    LITEGLOW_NUM_PARAMS
};

enum {
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