 /**
 ******************************************************************************
 * @file    app_stream.c
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
 ******************************************************************************/

#include "app_stream.h"
#include "uvcl.h"

#include <assert.h>

static APP_StreamConfig_t g_app_stream_cfg = {0};
static int g_app_stream_started = 0;
static int g_app_stream_initialized = 0;


static int APP_Stream_IsFormatSupported(APP_StreamFormat_t format);
static int APP_Stream_IsConfigValid(const APP_StreamConfig_t *p_cfg);

int APP_Stream_Init(const APP_StreamConfig_t *p_cfg)
{
  if (!APP_Stream_IsConfigValid(p_cfg))
  {
    return -1;
  }

  g_app_stream_cfg = *p_cfg;
  g_app_stream_initialized = 1;
  g_app_stream_started = 0;

  return 0;
}

const APP_StreamConfig_t *APP_Stream_GetConfig(void)
{

  if (!g_app_stream_initialized)
  {
	return NULL;
  }
  return &g_app_stream_cfg;
}

int APP_Stream_FormatToUvclPayload(APP_StreamFormat_t format, int *p_payload)
{
  if (p_payload == NULL)
    return -1;

  *p_payload = -1;

  /*
   * Map application stream formats to UVCL payload identifiers.
   * Note: this mapping does not imply full end-to-end runtime support yet.
   * Camera pipe configuration, descriptors, routing and reconfiguration logic
   * are introduced incrementally in follow-up steps.
   */

  switch (format)
  {
    case APP_STREAM_FMT_H264:
      *p_payload = UVCL_PAYLOAD_FB_H264;
      return 0;

    case APP_STREAM_FMT_JPEG:
      *p_payload = UVCL_PAYLOAD_FB_JPEG;
      return 0;

    case APP_STREAM_FMT_RGB565:
      *p_payload = UVCL_PAYLOAD_FB_RGB565;
      return 0;

    case APP_STREAM_FMT_YUV422:
      *p_payload = UVCL_PAYLOAD_UNCOMPRESSED_YUY2;
      return 0;

    case APP_STREAM_FMT_GRAY8:
      *p_payload = UVCL_PAYLOAD_FB_GREY;
      return 0;

 /* UVCL payload not defined yet in current middleware for:
  * - APP_STREAM_FMT_RGB888
  * - APP_STREAM_FMT_YUV420
  */
    case APP_STREAM_FMT_RGB888: //UVCL_PAYLOAD_FB_BGR3
    case APP_STREAM_FMT_YUV420:
    default:
      return -1;
  }
}

static int APP_Stream_IsFormatSupported(APP_StreamFormat_t format)
{
  int payload;

  return (APP_Stream_FormatToUvclPayload(format, &payload) == 0) ? 1 : 0;
}

int APP_Stream_Start(void)
{
  if (!g_app_stream_initialized)
  {
    return -1;
  }

  if (g_app_stream_started)
  {
    return 0;
  }

  g_app_stream_started = 1;
  return 0;
}

int APP_Stream_Stop(void)
{
  if (!g_app_stream_initialized)
  {
    return -1;
  }

  g_app_stream_started = 0;
  return 0;
}

int APP_Stream_IsStarted(void)
{
  return g_app_stream_started;
}

int APP_Stream_IsInitialized(void)
{
  return g_app_stream_initialized;
}

int APP_Stream_UpdateConfig(const APP_StreamConfig_t *p_cfg)
{

  if (!g_app_stream_initialized)
  {
	return -1;
  }

  if (p_cfg == NULL)
  {
    return -1;
  }

  if (g_app_stream_started)
  {
    return -1;
  }

  if (!APP_Stream_IsConfigValid(p_cfg))
  {
    return -1;
  }

  g_app_stream_cfg = *p_cfg;
  return 0;
}

static int APP_Stream_IsConfigValid(const APP_StreamConfig_t *p_cfg)
{
  if (p_cfg == NULL)
  {
    return 0;
  }

  if ((p_cfg->width == 0U) || (p_cfg->height == 0U) || (p_cfg->fps == 0U))
  {
    return 0;
  }

  if (!APP_Stream_IsFormatSupported(p_cfg->format))
  {
    return 0;
  }

  return 1;
}

int APP_Stream_IsSameConfig(const APP_StreamConfig_t *p_cfg_a, const APP_StreamConfig_t *p_cfg_b)
{
  if ((p_cfg_a == NULL) || (p_cfg_b == NULL))
  {
    return 0;
  }

  if (p_cfg_a->width != p_cfg_b->width)
  {
    return 0;
  }

  if (p_cfg_a->height != p_cfg_b->height)
  {
    return 0;
  }

  if (p_cfg_a->fps != p_cfg_b->fps)
  {
    return 0;
  }

  if (p_cfg_a->format != p_cfg_b->format)
  {
    return 0;
  }

  return 1;
  // return memcmp(p_cfg_a, p_cfg_b, sizeof(APP_StreamConfig_t)) == 0; /* prefer explicit way instead of this*/
}
