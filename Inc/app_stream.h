/**
 ******************************************************************************
 * @file    app_stream.h
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

#ifndef APP_STREAM_H
#define APP_STREAM_H

#include <stdint.h>

typedef enum
{
  APP_STREAM_FMT_H264 = 0,
  APP_STREAM_FMT_JPEG,
  APP_STREAM_FMT_RGB565,
  APP_STREAM_FMT_RGB888,
  APP_STREAM_FMT_YUV422,
  APP_STREAM_FMT_YUV420,
  APP_STREAM_FMT_GRAY8,
  APP_STREAM_FMT_BGR3
} APP_StreamFormat_t;

typedef struct
{
  uint16_t width;
  uint16_t height;
  uint32_t fps;
  APP_StreamFormat_t format;
} APP_StreamConfig_t;


int APP_Stream_Init(const APP_StreamConfig_t *p_cfg);
int APP_Stream_UpdateConfig(const APP_StreamConfig_t *p_cfg);
const APP_StreamConfig_t *APP_Stream_GetConfig(void);
int APP_Stream_IsSameConfig(const APP_StreamConfig_t *p_cfg_a, const APP_StreamConfig_t *p_cfg_b);

int APP_Stream_Start(void);
int APP_Stream_Stop(void);
int APP_Stream_IsStarted(void);
int APP_Stream_IsInitialized(void);


#endif /* APP_STREAM_H */


