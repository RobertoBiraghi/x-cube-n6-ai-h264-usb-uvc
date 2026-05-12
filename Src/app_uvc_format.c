 /**
 ******************************************************************************
 * @file    app_uvc_format.c
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


#include "app_uvc_format.h"

#include "uvcl.h"

int APP_UVC_FormatToPayload(APP_StreamFormat_t format, int *p_payload)
{
  if (p_payload == NULL)
  {
    return -1;
  }

  *p_payload = -1;

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

    case APP_STREAM_FMT_BGR3:
      *p_payload = UVCL_PAYLOAD_FB_BGR3;
      return 0;

    case APP_STREAM_FMT_RGB888:
    case APP_STREAM_FMT_YUV420:
    default:
      return -1;
  }
}

size_t APP_UVC_GetFrameSize(const APP_StreamConfig_t *p_cfg)
{
  if (p_cfg == NULL)
  {
    return 0;
  }

  switch (p_cfg->format)
  {
    case APP_STREAM_FMT_RGB565:
    case APP_STREAM_FMT_YUV422:
      return (size_t)p_cfg->width * p_cfg->height * 2U;

    case APP_STREAM_FMT_GRAY8:
      return (size_t)p_cfg->width * p_cfg->height;

    case APP_STREAM_FMT_H264:
    case APP_STREAM_FMT_JPEG:
      return 0;

    case APP_STREAM_FMT_RGB888:
      return (size_t)p_cfg->width * p_cfg->height * 3U;

    case APP_STREAM_FMT_YUV420:
      return ((size_t)p_cfg->width * p_cfg->height * 3U) / 2U;

    case APP_STREAM_FMT_BGR3:
    	return (size_t)p_cfg->width * p_cfg->height * 3U;

    default:
      return 0;
  }
}

int APP_UVC_IsCompressedFormat(APP_StreamFormat_t format)
{
  switch (format)
  {
    case APP_STREAM_FMT_H264:
    case APP_STREAM_FMT_JPEG:
      return 1;

    case APP_STREAM_FMT_RGB565:
    case APP_STREAM_FMT_RGB888:
    case APP_STREAM_FMT_YUV422:
    case APP_STREAM_FMT_YUV420:
    case APP_STREAM_FMT_GRAY8:
    case APP_STREAM_FMT_BGR3:
    default:
      return 0;
  }
}
