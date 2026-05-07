 /**
 ******************************************************************************
 * @file    app_cam.h
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
#ifndef APP_CAM
#define APP_CAM

#include <stdint.h>

#include "app_stream.h"

#define CAMERA_FPS 30
#define CAM_DEFAULT_STREAM_WIDTH   1280U
#define CAM_DEFAULT_STREAM_HEIGHT  720U
#define CAM_DEFAULT_STREAM_FORMAT  (APP_STREAM_FMT_H264)

typedef struct
{
  uint16_t width;
  uint16_t height;
  uint32_t fps;
  APP_StreamFormat_t format;
} CAM_StreamConfig_t;

void CAM_Init(void);
int CAM_DeInit(void);
void CAM_DisplayPipe_Start(uint8_t *display_pipe_dst, uint32_t cam_mode);
#if 0
void CAM_DisplayPipe_Stop(void);
#endif
void CAM_NNPipe_Start(uint8_t *nn_pipe_dst, uint32_t cam_mode);
void CAM_IspUpdate(void);
int CAM_GetVencWidth(void);
int CAM_GetVencHeight(void);

int CAM_SetRequestedStreamConfig(const CAM_StreamConfig_t *p_cfg);

#endif
