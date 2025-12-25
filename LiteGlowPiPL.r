#include "AEConfig.h"
#include "AE_EffectVers.h"

/* Include AE_General.r for resource definitions on Mac */
#ifdef AE_OS_MAC
	#include <AE_General.r>
#endif

#if defined(__MACH__) && !defined(AE_OS_MAC)
	#define AE_OS_MAC 1
	#include <AE_General.r>
#endif
    
resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "LiteGlow"
        },
        Category {
            "361do_plugins"
        },
#ifdef AE_OS_WIN
    #ifdef AE_PROC_INTELx64
        CodeWin64X86 {"EffectMain"},
    #endif
#else
    #ifdef AE_OS_MAC
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64 {"EffectMain"},
    #endif
#endif
        AE_PiPL_Version {
            2,
            0
        },
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },
        #define LITEGLOW_VERSION_VALUE 528385
        AE_Effect_Version {
            LITEGLOW_VERSION_VALUE
        },
        AE_Effect_Info_Flags {
            0
        },
        AE_Effect_Global_OutFlags {
            0x02000400  // PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_PIX_INDEPENDENT
        },
        AE_Effect_Global_OutFlags_2 {
            0x2A001400  // DIRECTX (0x20000000) | THREADED (0x08000000) | GPU_F32 (0x02000000) | FLOAT (0x00001000) | SMART (0x00000400)
        },
        AE_Effect_Match_Name {
            "361do LiteGlow"
        },
        AE_Reserved_Info {
            0
        },
        AE_Effect_Support_URL {
            "https://github.com/rebuildup/Ae_LiteGlow"
        }
    }
};
