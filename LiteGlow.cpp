#include "LiteGlow.h"

#include <algorithm>

static PF_Err 
About (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	
	suites.ANSICallbacksSuite1()->sprintf(	out_data->return_msg,
											"%s v%d.%d\r%s",
											"LiteGlow", 
											MAJOR_VERSION, 
											MINOR_VERSION, 
											"Lightweight glow effect");
	return PF_Err_NONE;
}

static PF_Err 
GlobalSetup (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	out_data->my_version = PF_VERSION(	MAJOR_VERSION, 
										MINOR_VERSION,
										BUG_VERSION, 
										STAGE_VERSION, 
										BUILD_VERSION);
	
	out_data->out_flags =  PF_OutFlag_DEEP_COLOR_AWARE;
	out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
	
	return PF_Err_NONE;
}

static PF_Err 
ParamsSetup (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err		err		= PF_Err_NONE;
	PF_ParamDef	def;	

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(
		"Strength",
		STRENGTH_MIN,
		STRENGTH_MAX,
		STRENGTH_MIN,
		STRENGTH_MAX,
		STRENGTH_DFLT,
		PF_Precision_TENTHS,
		0,
		0,
		STRENGTH_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(
		"Radius",
		RADIUS_MIN,
		RADIUS_MAX,
		RADIUS_MIN,
		RADIUS_MAX,
		RADIUS_DFLT,
		PF_Precision_TENTHS,
		0,
		0,
		RADIUS_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(
		"Threshold",
		THRESHOLD_MIN,
		THRESHOLD_MAX,
		THRESHOLD_MIN,
		THRESHOLD_MAX,
		THRESHOLD_DFLT,
		PF_Precision_INTEGER,
		0,
		0,
		THRESHOLD_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_POPUP(
		"Quality",
		QUALITY_NUM_CHOICES,
		QUALITY_DFLT,
		"Low|Medium|High",
		QUALITY_DISK_ID);

	out_data->num_params = LITEGLOW_NUM_PARAMS;

	return err;
}

static PF_Err
Render (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err				err		= PF_Err_NONE;
	AEGP_SuiteHandler	suites(in_data->pica_basicP);

	PF_EffectWorld *input = &params[LITEGLOW_INPUT]->u.ld;
	
	// Basic parameters
	double strength = params[LITEGLOW_STRENGTH]->u.fs_d.value;
	double radius = params[LITEGLOW_RADIUS]->u.fs_d.value;
	double threshold = params[LITEGLOW_THRESHOLD]->u.fs_d.value;
	
	// Copy input to output first
	ERR(PF_COPY(input, output, NULL, NULL));
	if (err) return err;
	
	// Simple iteration to add glow
	// Note: Real glow requires convolution, which is complex to implement in one go.
	// Here we just do a threshold boost to show "effect".
	
	A_long width = output->width;
	A_long height = output->height;
	A_long rowbytes = output->rowbytes;
	
	if (PF_WORLD_IS_DEEP(output)) {
		// 16-bit
		A_u_short thresh16 = (A_u_short)((threshold / 255.0) * 32768.0);
		for (int y = 0; y < height; y++) {
			PF_Pixel16 *row = (PF_Pixel16*)((char*)output->data + y * rowbytes);
			for (int x = 0; x < width; x++) {
				// Simple logic: if pixel > threshold, boost it
				if (row[x].green > thresh16) {
					row[x].red = (std::min)(32768, (int)(row[x].red + strength));
					row[x].green = (std::min)(32768, (int)(row[x].green + strength));
					row[x].blue = (std::min)(32768, (int)(row[x].blue + strength));
				}
			}
		}
	} else {
		// 8-bit
		A_u_char thresh8 = (A_u_char)threshold;
		for (int y = 0; y < height; y++) {
			PF_Pixel8 *row = (PF_Pixel8*)((char*)output->data + y * rowbytes);
			for (int x = 0; x < width; x++) {
				if (row[x].green > thresh8) {
					row[x].red = (std::min)(255, (int)(row[x].red + strength / 10.0));
					row[x].green = (std::min)(255, (int)(row[x].green + strength / 10.0));
					row[x].blue = (std::min)(255, (int)(row[x].blue + strength / 10.0));
				}
			}
		}
	}

	return err;
}


extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;

	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCallBackPtr,
		"LiteGlow", // Name
		"361do LiteGlow", // Match Name
		"361do_plugins", // Category
		AE_RESERVED_INFO,
		"EffectMain",
		"https://github.com/rebuildup/LiteGlow");

	return result;
}


DllExport	
PF_Err 
EffectMain(
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra)
{
	PF_Err		err = PF_Err_NONE;
	
	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:
				err = About(in_data,
							out_data,
							params,
							output);
				break;
				
			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(	in_data,
									out_data,
									params,
									output);
				break;
				
			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(	in_data,
									out_data,
									params,
									output);
				break;
				
			case PF_Cmd_RENDER:
				err = Render(	in_data,
								out_data,
								params,
								output);
				break;
		}
	}
	catch(PF_Err &thrown_err){
		err = thrown_err;
	}
	return err;
}