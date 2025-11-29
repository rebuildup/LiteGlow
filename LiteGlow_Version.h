#pragma once

#include "AEConfig.h"
#include "AE_Effect.h"

// Version constants shared between runtime code and the PiPL.
#define MAJOR_VERSION        1
#define MINOR_VERSION        0
#define BUG_VERSION          0
#define STAGE_VERSION        PF_Stage_DEVELOP
#define BUILD_VERSION        1

#define LITEGLOW_VERSION PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION)
