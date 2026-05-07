/**
 ******************************************************************************
 * @file    app_uvc_format.h
 * @author  SRA Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */


#ifndef APP_UVC_FORMAT_H
#define APP_UVC_FORMAT_H

#include <stddef.h>

#include "app_stream.h"

int APP_UVC_FormatToPayload(APP_StreamFormat_t format, int *p_payload);
size_t APP_UVC_GetFrameSize(const APP_StreamConfig_t *p_cfg);
int APP_UVC_IsCompressedFormat(APP_StreamFormat_t format);

#endif /* APP_UVC_FORMAT_H */
