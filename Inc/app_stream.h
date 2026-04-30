/*
 * app_stream.c
 *
 *  Created on: Apr 29, 2026
 *      Author: biraghir
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
  APP_STREAM_FMT_GRAY8
} APP_StreamFormat_t;

typedef struct
{
  uint16_t width;
  uint16_t height;
  uint32_t fps;
  APP_StreamFormat_t format;
} APP_StreamConfig_t;

int APP_Stream_Init(const APP_StreamConfig_t *p_cfg);
const APP_StreamConfig_t *APP_Stream_GetConfig(void);

#endif /* APP_STREAM_H */


