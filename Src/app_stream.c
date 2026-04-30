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

#include <string.h>
#include <assert.h>

static APP_StreamConfig_t g_app_stream_cfg;

int APP_Stream_Init(const APP_StreamConfig_t *p_cfg)
{
  assert(p_cfg != NULL);

  g_app_stream_cfg = *p_cfg;
  return 0;
}

const APP_StreamConfig_t *APP_Stream_GetConfig(void)
{
  return &g_app_stream_cfg;
}
