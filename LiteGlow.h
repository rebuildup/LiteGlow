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
#define QUALITY_NUM_CHOICES 3
#define QUALITY_DFLT       QUALITY_MEDIUM

// Maximum kernel size for Gaussian blur
#define KERNEL_SIZE_MAX    64

// Define Smart Render structs locally to avoid missing header issues
typedef struct PF_SmartRenderCallbacks_Local {
    PF_Err (*checkout_layer_pixels)(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_EffectWorld **pixels);
    PF_Err (*checkout_output)(PF_ProgPtr effect_ref, PF_EffectWorld **output);
    PF_Err (*checkin_layer_pixels)(PF_ProgPtr effect_ref, PF_ParamIndex index);
    PF_Err (*is_layer_pixel_data_valid)(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_Boolean *valid);
} PF_SmartRenderCallbacks_Local;

typedef struct PF_SmartRenderExtra_Local {
    PF_SmartRenderCallbacks_Local *cb;
    void *unused;
} PF_SmartRenderExtra_Local;

typedef struct PF_PreRenderInput_Local {
    PF_RenderRequest output_request;
    short bit_depth;
    void *unused;
} PF_PreRenderInput_Local;

typedef struct PF_PreRenderOutput_Local {
    PF_LRect result_rect;
    PF_LRect max_result_rect;
    void *unused;
} PF_PreRenderOutput_Local;

typedef struct PF_PreRenderCallbacks_Local {
    PF_Err (*checkout_layer)(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamIndex req_index, const PF_RenderRequest *req, A_long current_time, A_long time_step, A_u_long time_scale, PF_CheckoutResult *result);
    PF_Err (*checkout_layer_pixels)(PF_ProgPtr effect_ref, PF_ParamIndex index, PF_ParamIndex req_index, const PF_RenderRequest *req, A_long current_time, A_long time_step, A_u_long time_scale, PF_EffectWorld **pixels);
} PF_PreRenderCallbacks_Local;

typedef struct PF_PreRenderExtra_Local {
    PF_PreRenderCallbacks_Local *cb;
    PF_PreRenderInput_Local *input;
    PF_PreRenderOutput_Local *output;
    void *unused;
} PF_PreRenderExtra_Local;

enum {
    LITEGLOW_INPUT = 0,
    LITEGLOW_STRENGTH,
    LITEGLOW_RADIUS,
    LITEGLOW_THRESHOLD,
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