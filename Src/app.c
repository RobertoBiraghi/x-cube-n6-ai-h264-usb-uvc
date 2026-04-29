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
    return -1;
  }

  if (buffer_flying)
  {
    force_intra = 1;
    return -1;
  }

  memcpy(uvc_in_buffers, venc_out_buffer, (size_t)len);

  buffer_flying = 1;
  ret = UVCL_ShowFrame(uvc_in_buffers, len);
  if (ret != 0)
  {
    buffer_flying = 0;
    return -1;
  }

  return 0;
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

    CAM_IspUpdate();
  }
}

static void app_uvc_streaming_active(struct uvcl_callbacks *cbs, UVCL_StreamConf_t stream)
{
  (void)cbs;
  (void)stream;
  force_intra = 1;
  uvc_is_active = 1;
  BSP_LED_On(LED_RED);
}

static void app_uvc_streaming_inactive(struct uvcl_callbacks *cbs)
{
  (void)cbs;
  uvc_is_active = 0;
  BSP_LED_Off(LED_RED);
}

static void app_uvc_frame_release(struct uvcl_callbacks *cbs, void *frame)
{

  (void)cbs;
  (void)frame;
  assert(buffer_flying);

  buffer_flying = 0;
}

static void app_display_info_header()
{
  printf("========================================\n");
  printf("stm32n6 universal uvc camera (%s)\n", APP_VERSION_STRING);
  printf("Build date & time: %s %s\n", __DATE__, __TIME__);
#if defined(__GNUC__)
  printf("Compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__ICCARM__)
  printf("Compiler: IAR EWARM %d.%d.%d\n", __VER__ / 1000000, (__VER__ / 1000) % 1000 ,__VER__ % 1000);
#else
  printf("Compiler: Unknown\n");
#endif
  printf("HAL: %lu.%lu.%lu\n", __STM32N6xx_HAL_VERSION_MAIN, __STM32N6xx_HAL_VERSION_SUB1, __STM32N6xx_HAL_VERSION_SUB2);
  printf("Streaming mode: H264 over UVC\n");
  printf("========================================\n");
}

void app_run()
{
  UBaseType_t isp_priority = FREERTOS_PRIORITY(2);
  UBaseType_t stream_priority = FREERTOS_PRIORITY(1);
  UVCL_Conf_t uvcl_conf = { 0 };
  ENC_Conf_t enc_conf = { 0 };
  TaskHandle_t hdl;
  int ret;

  app_display_info_header();
  /* Enable DWT so DWT_CYCCNT works when debugger not attached */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  ret = BSP_PB_Init(BUTTON_USER1, BUTTON_MODE_GPIO);
  assert(ret == BSP_ERROR_NONE);

  //cpuload_init(&cpu_load);

  /* create buffer queues */

  /* setup fonts */

  /* Enable venc */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
   LL_VENC_Init();

  /*** Camera Init ************************************************************/  
  CAM_Init();

  /* Encoder init */
  enc_conf.width = VENC_WIDTH;
  enc_conf.height = VENC_HEIGHT;
  enc_conf.fps = CAMERA_FPS;
  ENC_Init(&enc_conf);

  /* Uvc init */
  uvcl_conf.streams[0].width = VENC_WIDTH;
  uvcl_conf.streams[0].height = VENC_HEIGHT;
  uvcl_conf.streams[0].fps = CAMERA_FPS;
  uvcl_conf.streams[0].payload_type = UVCL_PAYLOAD_FB_H264;
  uvcl_conf.streams_nb = 1;
  uvcl_conf.is_immediate_mode = 1;
  uvcl_cbs.streaming_active = app_uvc_streaming_active;
  uvcl_cbs.streaming_inactive = app_uvc_streaming_inactive;
  uvcl_cbs.frame_release = app_uvc_frame_release;
  ret = UVCL_Init(USB1_OTG_HS, &uvcl_conf, &uvcl_cbs);

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
