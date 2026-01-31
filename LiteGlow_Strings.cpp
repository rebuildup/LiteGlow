// =============================================================================
// Naming Convention
// =============================================================================
// - PascalCase for functions and types
// - camelCase for local variables
// - UPPER_SNAKE_CASE for constants and macros
// - Prefix notation: g for global variables (e.g., g_strs)
// - String table uses A_ prefix for Adobe SDK types (A_u_long, A_char)

#include "LiteGlow.h"

typedef struct {
    A_u_long index;
    A_char   str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,                      "",
    StrID_Name,                      "LiteGlow",
    StrID_Description,               "An enhanced glow effect with true Gaussian blur.\rCopyright 2007-2025.",
    StrID_Strength_Param_Name,       "Strength",
    StrID_Radius_Param_Name,         "Radius",
    StrID_Threshold_Param_Name,      "Threshold",
    StrID_Quality_Param_Name,        "Quality",
    StrID_Quality_Param_Choices,     "Low|Medium|High",
    StrID_Bloom_Intensity_Param_Name, "Bloom Intensity",
    StrID_Knee_Param_Name,           "Threshold Softness",
    StrID_Blend_Mode_Param_Name,     "Blend Mode",
    StrID_Blend_Mode_Param_Choices,  "Screen|Add|Normal",
    StrID_Tint_Color_Param_Name,     "Tint Color"
};

char* GetStringPtr(int strNum)
{
    if (strNum < 0 || strNum >= StrID_NUMTYPES) {
        return g_strs[StrID_NONE].str;
    }
    return g_strs[strNum].str;
}
