 /**
 ******************************************************************************
 * @file    app.c
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#include "app.h"

#include <stdint.h>

#include "app_cam.h"
#include "app_config.h"
#include "isp_api.h"
#include "cmw_camera.h"
#include "stm32n6xx_hal.h"
#include "stm32n6xx_ll_venc.h"
#include "stm32n6570_discovery.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "app_enc.h"
#include "utils.h"
#include "uvcl.h"
#include "app_stream.h"
#include "app_uvc_format.h"

#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "dev"
#endif

#define FREERTOS_PRIORITY(p) ((UBaseType_t)((int)tskIDLE_PRIORITY + configMAX_PRIORITIES / 2 + (p)))

#define CACHE_OP(__op__) do { \
  if (is_cache_enable()) { \
    __op__; \
  } \
} while (0)

#define BQUEUE_MAX_BUFFERS 2
#define CPU_LOAD_HISTORY_DEPTH 8

#define CAPTURE_BUFFER_NB (CAPTURE_DELAY + 2)

/* venc conf */
#define VENC_MAX_WIDTH 1280
#define VENC_MAX_HEIGHT 720
#define VENC_OUT_BUFFER_SIZE (255 * 1024)

/* Globals */
#define APP_STREAM_PRESET_HD   0
#define APP_STREAM_PRESET_VGA  1

#define APP_STREAM_PRESET APP_STREAM_PRESET_HD

static int g_app_stream_preset = APP_STREAM_PRESET;
static volatile int app_stream_reconfig_in_progress;

/* capture buffers */
static uint8_t capture_buffer[CAPTURE_BUFFER_NB][VENC_MAX_WIDTH * VENC_MAX_HEIGHT * CAPTURE_BPP] ALIGN_32 IN_PSRAM;

static volatile int capture_buffer_disp_idx = 1;
static volatile int capture_buffer_capt_idx = 0;

/* venc */
static uint8_t venc_out_buffer[VENC_OUT_BUFFER_SIZE] ALIGN_32 UNCACHED;
/*
 * If UVCL/USB uses DMA on this buffer, placing it in a non-cacheable section or adding explicit
 * cache maintenance may be required on STM32N6.
 */
static uint8_t uvc_in_buffers[VENC_OUT_BUFFER_SIZE] ALIGN_32;

/* uvc */
static struct uvcl_callbacks uvcl_cbs;
static volatile int uvc_is_active;
static volatile int buffer_flying;
static volatile int force_intra;

 /* threads */
static StaticTask_t stream_thread;
static StackType_t stream_thread_stack[2 * configMINIMAL_STACK_SIZE];
static SemaphoreHandle_t stream_sem;
static StaticSemaphore_t stream_sem_buffer;

static StaticTask_t isp_thread;
static StackType_t isp_thread_stack[2 *configMINIMAL_STACK_SIZE];
static SemaphoreHandle_t isp_sem;
static StaticSemaphore_t isp_sem_buffer;

static void app_display_info_header(const APP_StreamConfig_t *p_stream_cfg);
static void app_fill_stream_preset(APP_StreamConfig_t *p_cfg, int preset_id);
static int app_apply_stream_runtime_config(const APP_StreamConfig_t *p_stream_cfg);
static int app_init_uvc(const APP_StreamConfig_t *p_stream_cfg);

static int is_cache_enable()
{
#if defined(USE_DCACHE)
  return 1;
#else
  return 0;
#endif
}

static void app_main_pipe_frame_event()
{
  int next_disp_idx = (capture_buffer_disp_idx + 1) % CAPTURE_BUFFER_NB;
  int next_capt_idx = (capture_buffer_capt_idx + 1) % CAPTURE_BUFFER_NB;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  int ret;

  ret = HAL_DCMIPP_PIPE_SetMemoryAddress(CMW_CAMERA_GetDCMIPPHandle(), DCMIPP_PIPE1,
                                         DCMIPP_MEMORY_ADDRESS_0, (uint32_t) capture_buffer[next_capt_idx]);
  assert(ret == HAL_OK);

  capture_buffer_disp_idx = next_disp_idx;
  capture_buffer_capt_idx = next_capt_idx;

  ret = xSemaphoreGiveFromISR(stream_sem, &xHigherPriorityTaskWoken);
  if (ret == pdTRUE)
  {
	  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void app_main_pipe_vsync_event()
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  int ret;

  ret = xSemaphoreGiveFromISR(isp_sem, &xHigherPriorityTaskWoken);
  if (ret == pdTRUE)
	  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static int send_h264_frame(uint8_t *p_buffer, int is_intra_force)
{
  int len;
  int ret;

  len = ENC_EncodeFrame(p_buffer, venc_out_buffer, VENC_OUT_BUFFER_SIZE, is_intra_force);
  if (len <= 0)
  {
	//printf("ENC_EncodeFrame failed, len=%d\n", len);
    return -1;
  }

  if (buffer_flying)
  {
	//printf("Dropping frame: buffer still flying\n");
	force_intra = 1;
    return -1;
  }

  memcpy(uvc_in_buffers, venc_out_buffer, (size_t)len);

  buffer_flying = 1;
  ret = UVCL_ShowFrame(uvc_in_buffers, len);
  if (ret != 0)
  {
	//printf("UVCL_ShowFrame failed, ret=%d, len=%d\n", ret, len);
    buffer_flying = 0;
    return -1;
  }

  return 0;
}

static int app_switch_stream_preset(int preset_id)
{
  APP_StreamConfig_t new_stream_cfg;
  CAM_StreamConfig_t new_cam_cfg;
  const APP_StreamConfig_t *p_stream_cfg;
  int ret;

  if (app_stream_reconfig_in_progress)
  {
    printf("Preset switch ignored while reconfiguration is already in progress\n");
    return -1;
  }

  if (uvc_is_active)
  {
    printf("Preset switch ignored while UVC stream is active\n");
    return -1;
  }

  if (preset_id == g_app_stream_preset)
  {
    return 0;
  }

  app_stream_reconfig_in_progress = 1;

  app_fill_stream_preset(&new_stream_cfg, preset_id);

  new_cam_cfg.width = new_stream_cfg.width;
  new_cam_cfg.height = new_stream_cfg.height;
  new_cam_cfg.fps = new_stream_cfg.fps;

  printf("Switching preset %d -> %d\n", g_app_stream_preset, preset_id);

  ret = APP_Stream_Stop();
  if (ret != 0)
  {
    goto error;
  }

  printf("Reconfig: ENC_DeInit\n");
  ENC_DeInit();

  printf("Reconfig: CAM_DeInit\n");
  ret = CAM_DeInit();
  if (ret != 0)
  {
    goto error;
  }

  printf("Reconfig: UVCL_Deinit\n");
  ret = UVCL_Deinit();
  printf("Reconfig: UVCL_Deinit ret=%d\n", ret);
  if (ret != 0)
  {
    goto error;
  }

  ret = CAM_SetRequestedStreamConfig(&new_cam_cfg);
  if (ret != 0)
  {
    goto error;
  }

  printf("Reconfig: CAM_Init\n");
  CAM_Init();

  ret = APP_Stream_UpdateConfig(&new_stream_cfg);
  if (ret != 0)
  {
    goto error;
  }

  p_stream_cfg = APP_Stream_GetConfig();
  if (p_stream_cfg == NULL)
  {
    goto error;
  }

  printf("Reconfig: UVCL_Init\n");
  ret = app_init_uvc(p_stream_cfg);
  if (ret != 0)
  {
    goto error;
  }

  printf("Reconfig: ENC_Init\n");
  ret = app_apply_stream_runtime_config(p_stream_cfg);
  printf("Reconfig: UVCL_Init ret=%d\n", ret);
  if (ret != 0)
  {
    goto error;
  }

  capture_buffer_disp_idx = 1;
  capture_buffer_capt_idx = 0;
  force_intra = 1;
  buffer_flying = 0;
  printf("Reconfig: CAM_DisplayPipe_Start\n");
  CAM_DisplayPipe_Start(capture_buffer[0], CMW_MODE_CONTINUOUS);

  ret = APP_Stream_Start();
  if (ret != 0)
  {
    goto error;
  }

  g_app_stream_preset = preset_id;
  app_stream_reconfig_in_progress = 0;

  printf("Preset switch completed\n");
  app_display_info_header(p_stream_cfg);

  return 0;

error:
  app_stream_reconfig_in_progress = 0;
  printf("Preset switch failed\n");
  return -1;
}

static void app_process_user_button(void)
{
  static int button_press_latched = 0;
  int cur_button_state;
  int next_preset;

  cur_button_state = BSP_PB_GetState(BUTTON_USER1);

  if (cur_button_state == GPIO_PIN_SET)
  {
    if (!button_press_latched)
    {
      button_press_latched = 1;

      next_preset = (g_app_stream_preset == APP_STREAM_PRESET_HD) ?
                    APP_STREAM_PRESET_VGA : APP_STREAM_PRESET_HD;

      printf("\nPreset switch call\n");

      (void)app_switch_stream_preset(next_preset);
    }
  }
  else
  {
    button_press_latched = 0;
  }
}

static void stream_thread_fct(void *arg)
{
  int ret;
  int uvc_is_active_prev = 0;

  (void)arg;

  while (1)
  {
    ret = xSemaphoreTake(stream_sem, portMAX_DELAY);
    assert(ret == pdTRUE);

    app_process_user_button();

    if (app_stream_reconfig_in_progress || !APP_Stream_IsStarted())
    {
      uvc_is_active_prev = 0;
      continue;
    }

    if (!uvc_is_active)
    {
      uvc_is_active_prev = 0;
      continue;
    }

    if (send_h264_frame(capture_buffer[capture_buffer_disp_idx],
                        (!uvc_is_active_prev || force_intra)) == 0)
    {
      force_intra = 0;
      uvc_is_active_prev = 1;
    }
  }
}

static void isp_thread_fct(void *arg)
{
  int ret;

  while (1) {
    ret = xSemaphoreTake(isp_sem, portMAX_DELAY);
    assert(ret == pdTRUE);

    if (!app_stream_reconfig_in_progress)
    {
      CAM_IspUpdate();
    }
  }
}

static void app_uvc_streaming_active(struct uvcl_callbacks *cbs, UVCL_StreamConf_t stream)
{
  (void)cbs;
  (void)stream;
  //force_intra = 1;
  //uvc_is_active = 1;
  //printf("\r\n Active\n");
  printf("\r\nUVC streaming active callback\r\n");

  /*
   * Ensure stream submission state is reset when a new host-side
   * streaming session starts.
   */
  buffer_flying = 0;
  force_intra = 1;
  uvc_is_active = 1;

  //printf("\r\n Active\n");
  BSP_LED_On(LED_RED);
}

static void app_uvc_streaming_inactive(struct uvcl_callbacks *cbs)
{
  (void)cbs;
  //uvc_is_active = 0;
  //printf("\r\n Inactive \n");

  printf("\r\nUVC streaming inactive callback\r\n");

  /*
   * Clear submission state on host-side stream stop.
   */
  uvc_is_active = 0;
  buffer_flying = 0;

  BSP_LED_Off(LED_RED);
}

static void app_uvc_frame_release(struct uvcl_callbacks *cbs, void *frame)
{

  (void)cbs;
  (void)frame;
  //assert(buffer_flying);

  /*
   * During stop/restart transitions, a delayed release callback may arrive
   * after the local submission state has already been reset.
   */
  if (!buffer_flying)
  {
	  printf("Frame release received while no buffer was marked flying\r\n");
	  return;
  }

  buffer_flying = 0;
}

static const char *app_stream_format_to_string(APP_StreamFormat_t format)
{
  switch (format)
  {
    case APP_STREAM_FMT_H264:
      return "H264";
    case APP_STREAM_FMT_JPEG:
      return "JPEG";
    case APP_STREAM_FMT_RGB565:
      return "RGB565";
    case APP_STREAM_FMT_RGB888:
      return "RGB888";
    case APP_STREAM_FMT_YUV422:
      return "YUV422";
    case APP_STREAM_FMT_YUV420:
      return "YUV420";
    case APP_STREAM_FMT_GRAY8:
      return "GRAY8";
    default:
      return "UNKNOWN";
  }
}

static void app_display_info_header(const APP_StreamConfig_t *p_stream_cfg)
{
  printf("\r\n========================================\r\n");
  printf("stm32n6 universal uvc camera (%s)\r\n", APP_VERSION_STRING);
  printf("Build date & time: %s %s\r\n", __DATE__, __TIME__);
#if defined(__GNUC__)
  printf("Compiler: GCC %d.%d.%d\r\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__ICCARM__)
  printf("Compiler: IAR EWARM %d.%d.%d\n", __VER__ / 1000000, (__VER__ / 1000) % 1000 ,__VER__ % 1000);
#else
  printf("Compiler: Unknown\n");
#endif
  printf("HAL: %lu.%lu.%lu\r\n", __STM32N6xx_HAL_VERSION_MAIN, __STM32N6xx_HAL_VERSION_SUB1, __STM32N6xx_HAL_VERSION_SUB2);

  if (p_stream_cfg != NULL)
  {
    printf("Streaming mode: %s over UVC\r\n", app_stream_format_to_string(p_stream_cfg->format));
    printf("Stream config : %ux%u @ %lu fps\r\n",
           p_stream_cfg->width,
           p_stream_cfg->height,
           p_stream_cfg->fps);
  }

  printf("========================================\r\n");
}

static void app_fill_stream_preset(APP_StreamConfig_t *p_cfg, int preset_id)
{
  assert(p_cfg != NULL);

  switch (preset_id)
  {
    case 0:
      p_cfg->width = 1280;
      p_cfg->height = 720;
      p_cfg->fps = CAMERA_FPS;
      p_cfg->format = APP_STREAM_FMT_H264;
      break;

    case 1:
      p_cfg->width = 640;
      p_cfg->height = 480;
      p_cfg->fps = CAMERA_FPS;
      p_cfg->format = APP_STREAM_FMT_H264;
      break;

    default:
      assert(0);
  }
}


static int app_init_uvc(const APP_StreamConfig_t *p_stream_cfg)
{
  UVCL_Conf_t uvcl_conf = { 0 };
  int ret;

  if (p_stream_cfg == NULL)
  {
    return -1;
  }

  uvcl_conf.streams[0].width = p_stream_cfg->width;
  uvcl_conf.streams[0].height = p_stream_cfg->height;
  uvcl_conf.streams[0].fps = p_stream_cfg->fps;

  ret = APP_UVC_FormatToPayload(p_stream_cfg->format, &uvcl_conf.streams[0].payload_type);
  if (ret != 0)
  {
    return -1;
  }

  uvcl_conf.streams_nb = 1;
  uvcl_conf.is_immediate_mode = 1;

  uvcl_cbs.streaming_active = app_uvc_streaming_active;
  uvcl_cbs.streaming_inactive = app_uvc_streaming_inactive;
  uvcl_cbs.frame_release = app_uvc_frame_release;

  ret = UVCL_Init(USB1_OTG_HS, &uvcl_conf, &uvcl_cbs);
  if (ret != 0)
  {
    return -1;
  }

  return 0;
}

static int app_apply_stream_runtime_config(const APP_StreamConfig_t *p_stream_cfg)
{
  ENC_Conf_t enc_conf = { 0 };

  if (p_stream_cfg == NULL)
  {
    return -1;
  }

  if (!APP_UVC_IsCompressedFormat(p_stream_cfg->format))
  {
    return -1;
  }

  if (p_stream_cfg->format != APP_STREAM_FMT_H264)
  {
    /* Compressed format detected, but not supported yet */
    return -1;
  }

  enc_conf.width = p_stream_cfg->width;
  enc_conf.height = p_stream_cfg->height;
  enc_conf.fps = p_stream_cfg->fps;
  ENC_Init(&enc_conf);

  return 0;
}

void app_run()
{
  UBaseType_t isp_priority = FREERTOS_PRIORITY(2);
  UBaseType_t stream_priority = FREERTOS_PRIORITY(1);
  APP_StreamConfig_t stream_cfg;
  const APP_StreamConfig_t *p_stream_cfg;
  CAM_StreamConfig_t cam_stream_cfg;
  TaskHandle_t hdl;
  int ret;

  /* Enable DWT so DWT_CYCCNT works when debugger not attached */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  ret = BSP_PB_Init(BUTTON_USER1, BUTTON_MODE_GPIO);
  assert(ret == BSP_ERROR_NONE);

  //cpuload_init(&cpu_load);

  /* Enable venc */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
   LL_VENC_Init();

  /*** Camera Init ************************************************************/

  g_app_stream_preset = APP_STREAM_PRESET;
  app_fill_stream_preset(&stream_cfg, g_app_stream_preset);

  cam_stream_cfg.width = stream_cfg.width;
  cam_stream_cfg.height = stream_cfg.height;
  cam_stream_cfg.fps = stream_cfg.fps;

  ret = CAM_SetRequestedStreamConfig(&cam_stream_cfg);
  assert(ret == 0);

  CAM_Init();

  ret = APP_Stream_Init(&stream_cfg);
  assert(ret == 0);

  ret = APP_Stream_Start();
  assert(ret == 0);

  p_stream_cfg = APP_Stream_GetConfig();
  assert(p_stream_cfg != NULL);

  app_display_info_header(p_stream_cfg);

  ret = app_init_uvc(p_stream_cfg);
  assert(ret == 0);

  ret = app_apply_stream_runtime_config(p_stream_cfg);
  assert(ret == 0);

  /* sems + mutex init */
  isp_sem = xSemaphoreCreateCountingStatic(1, 0, &isp_sem_buffer);
  assert(isp_sem);
  stream_sem = xSemaphoreCreateCountingStatic(1, 0, &stream_sem_buffer);
  assert(stream_sem);

  /* Start LCD Display camera pipe stream */
  CAM_DisplayPipe_Start(capture_buffer[0], CMW_MODE_CONTINUOUS);

  /* threads init */

  hdl = xTaskCreateStatic(stream_thread_fct, "stream", configMINIMAL_STACK_SIZE * 2, NULL,
                          stream_priority, stream_thread_stack, &stream_thread);
  assert(hdl != NULL);

  hdl = xTaskCreateStatic(isp_thread_fct, "isp", configMINIMAL_STACK_SIZE * 2, NULL, isp_priority, isp_thread_stack,
                          &isp_thread);
  assert(hdl != NULL);

  BSP_LED_On(LED_GREEN);
}

int CMW_CAMERA_PIPE_FrameEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_frame_event();

  return HAL_OK;
}

int CMW_CAMERA_PIPE_VsyncEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_vsync_event();

  return HAL_OK;
}
