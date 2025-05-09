/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2025 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

#pragma once

typedef enum {
	StrID_NONE,
	StrID_Name,
	StrID_Description,
	StrID_Strength_Param_Name,
	StrID_Radius_Param_Name,
	StrID_Threshold_Param_Name,
	StrID_Quality_Param_Name,
	StrID_Blend_Param_Name,
	StrID_Performance_Param_Name,        // NEW: Performance mode parameter name
	StrID_Performance_Param_Description, // NEW: Performance mode parameter description
	StrID_Quality_Param_Choices,
	StrID_NUMTYPES
} StrIDType;