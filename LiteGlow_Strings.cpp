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
    StrID_Quality_Param_Choices,     "Low|Medium|High"
};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}