 /**
 ******************************************************************************
 * @file    enc.c
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
#include "app_enc.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "h264encapi.h"
#include "jpegencapi.h"
#include "stm32n6xx_hal.h"
#include "utils.h"
#include "ewl.h"
#include "app_stream.h"
#include "stdio.h"

#define VENC_ALLOCATOR_SIZE (4 * 1024 * 1024)
#define RATE_CTRL_QP 25

enum {
  VENC_RATE_CTRL_QP_CONSTANT,
  VENC_RATE_CTRL_VBR,
};

typedef enum
{
  ENC_KIND_NONE = 0,
  ENC_KIND_H264,
  ENC_KIND_JPEG
} ENC_Kind_t;

static uint8_t venc_hw_allocator_buffer[VENC_ALLOCATOR_SIZE] ALIGN_32 IN_PSRAM;
static uint8_t *venc_hw_allocator_pos = venc_hw_allocator_buffer;

#if 0
static struct VENC_Context {
  H264EncInst hdl;
  int is_sps_pps_done;
  uint64_t pic_cnt;
  int gop_len;
} VENC_Instance;
#else
static struct VENC_Context {
  ENC_Kind_t kind;
  H264EncInst hdl;
  JpegEncInst jpeg_hdl;
  int is_sps_pps_done;
  uint64_t pic_cnt;
  int gop_len;
} VENC_Instance;
#endif

static void VENC_SetupConstantQp(H264EncRateCtrl *rate, int qp)
{
  rate->pictureRc = 0;
  rate->mbRc = 0;
  rate->pictureSkip = 0;
  rate->hrd = 0;
  rate->qpHdr = qp;
  rate->qpMin = qp;
  rate->qpMax = qp;
}

static void VENC_SetupVbr(H264EncRateCtrl *rate, int bitrate, int gopLen, int qp)
{
  rate->pictureRc = 1;
  rate->mbRc = 1;
  rate->pictureSkip = 0;
  rate->hrd = 0;
  rate->qpHdr = qp;
  rate->qpMin = 10;
  rate->qpMax = 51;
  rate->gopLen = gopLen;
  rate->bitPerSecond = bitrate;
  rate->intraQpDelta = 0;
}

static int VENC_AppendPadding(struct VENC_Context *p_ctx, uint8_t *p_out, size_t out_len, size_t *p_out_len)
{
  uint32_t out_addr = (uint32_t) p_out;
  int pad_size = 8 - (out_addr % 8);
  int pad_len = 0;

  *p_out_len = 0;

  /* No need of padding */
  if (out_addr % 8 == 0)
    return 0;

  /* adjust pad size */
  if (pad_size < 6)
    pad_size += 8;

  /* Do we add enought space for padding ? */
  if (pad_size > out_len)
    return -1;

  /* Ok now we add nal pad */
  p_out[pad_len++] = 0x00;
  p_out[pad_len++] = 0x00;
  p_out[pad_len++] = 0x00;
  p_out[pad_len++] = 0x01;
  p_out[pad_len++] = 0x2c; /* FIXME : adapt for nal_ref_idc ? */
  pad_size -= 5;
  while (pad_size--)
    p_out[pad_len++] = 0xff;

  *p_out_len = pad_len;

  return 0;
}

static int VENC_EncodeStart(struct VENC_Context *p_ctx, uint8_t *p_out, size_t out_len, size_t *p_out_len)
{
  H264EncOut enc_out;
  H264EncIn enc_in;
  size_t start_len;
  size_t pad_len;
  int ret;

  enc_in.pOutBuf = (u32 *) p_out;
  enc_in.busOutBuf = (ptr_t) p_out;
  enc_in.outBufSize = out_len;
  ret = H264EncStrmStart(p_ctx->hdl, &enc_in, &enc_out);
  if (ret)
    return ret;

  start_len = enc_out.streamSize;
  ret = VENC_AppendPadding(p_ctx, &p_out[start_len], out_len - start_len, &pad_len);
  if (ret)
    return ret;

  *p_out_len = start_len + pad_len;

  return 0;
}

static int VENC_EncodeFrame(struct VENC_Context *p_ctx, uint8_t *p_in, uint8_t *p_out, size_t out_len,
                            size_t *p_out_len, int is_intra_force)
{
  H264EncOut enc_out;
  H264EncIn enc_in;
  int ret;

  /* In both N6_VENC_INPUT_YUV2 and N6_VENC_INPUT_RGB565 only busLuma is used */
  enc_in.busLuma = (ptr_t) p_in;
  enc_in.busChromaU = 0;
  enc_in.busChromaV = 0;
  enc_in.pOutBuf = (u32 *) p_out;
  enc_in.busOutBuf = (ptr_t) p_out;
  enc_in.outBufSize = out_len;
  enc_in.codingType = (p_ctx->pic_cnt % (p_ctx->gop_len + 1) == 0) ? H264ENC_INTRA_FRAME : H264ENC_PREDICTED_FRAME;
  enc_in.codingType = is_intra_force ? H264ENC_INTRA_FRAME : enc_in.codingType;
  enc_in.timeIncrement = 1;
  enc_in.ipf = H264ENC_REFERENCE_AND_REFRESH; /* FIXME : can be H264ENC_NO_REFERENCE_NO_REFRESH in I only mode */
  enc_in.ltrf = H264ENC_NO_REFERENCE_NO_REFRESH;
  enc_in.lineBufWrCnt = 0;
  enc_in.sendAUD = 0;

  ret = H264EncStrmEncode(p_ctx->hdl, &enc_in, &enc_out, NULL, NULL, NULL);
  if (ret != H264ENC_FRAME_READY)
    return -1;

  p_ctx->pic_cnt++;
  *p_out_len = enc_out.streamSize;

  return 0;
}

static int VENC_Encode(uint8_t *p_in, uint8_t *p_out, size_t out_len, size_t *p_out_len, int is_intra_force)
{
  struct VENC_Context *p_ctx = &VENC_Instance;
  size_t start_len = 0;
  size_t frame_len;
  int ret;

  if (!p_ctx->is_sps_pps_done)
  {
    ret = VENC_EncodeStart(p_ctx, p_out, out_len, &start_len);
    if (ret)
      return ret;
    p_ctx->is_sps_pps_done = 1;
  }

  ret = VENC_EncodeFrame(p_ctx, p_in, &p_out[start_len], out_len - start_len, &frame_len, is_intra_force);
  if (ret)
    return ret;

  *p_out_len = start_len + frame_len;

  return 0;
}

void VENC_H264_Init(ENC_Conf_t *p_conf)
{
  const int rate_ctrl_mode = VENC_RATE_CTRL_VBR;
  struct VENC_Context *p_ctx = &VENC_Instance;
  H264EncPreProcessingCfg cfg;
  H264EncCodingCtrl ctrl;
  H264EncConfig config;
  H264EncRateCtrl rate;
  int target_bitrate;
  int ret;

  venc_hw_allocator_pos = venc_hw_allocator_buffer;
  memset(p_ctx, 0, sizeof(*p_ctx));
  memset(&config, 0, sizeof(config));
  /*Set H264*/
  p_ctx->kind = ENC_KIND_H264;
  p_ctx->gop_len = p_conf->fps - 1;
  /* init encoder */
  config.streamType = H264ENC_BYTE_STREAM;
  config.viewMode = H264ENC_BASE_VIEW_SINGLE_BUFFER;
  config.level = H264ENC_LEVEL_5_1;
  config.width = p_conf->width;
  config.height = p_conf->height;
  config.frameRateNum = p_conf->fps;
  config.frameRateDenom = 1;
  config.refFrameAmount = 1;
  ret = H264EncInit(&config, &p_ctx->hdl);
  assert(ret == H264ENC_OK);

  /* setup source format */
  ret = H264EncGetPreProcessing(p_ctx->hdl, &cfg);
  assert(ret == H264ENC_OK);
  cfg.inputType = H264ENC_RGB888;
  ret = H264EncSetPreProcessing(p_ctx->hdl, &cfg);
  assert(ret == H264ENC_OK);

  /* setup coding ctrl */
  ret = H264EncGetCodingCtrl(p_ctx->hdl, &ctrl);
  assert(ret == H264ENC_OK);
  ctrl.idrHeader = 1;
  ret = H264EncSetCodingCtrl(p_ctx->hdl, &ctrl);
  assert(ret == H264ENC_OK);

  /* setup rate ctrl */
  ret = H264EncGetRateCtrl(p_ctx->hdl, &rate);
  assert(ret == H264ENC_OK);
  target_bitrate = ((p_conf->width * p_conf->height * 12) * p_conf->fps) / 30;
  if (rate_ctrl_mode == VENC_RATE_CTRL_QP_CONSTANT)
  {
    VENC_SetupConstantQp(&rate, RATE_CTRL_QP);
  } else if (rate_ctrl_mode == VENC_RATE_CTRL_VBR)
  {
    VENC_SetupVbr(&rate, target_bitrate, p_conf->fps, RATE_CTRL_QP);
  } else
  {
    assert(0);
  }
  ret = H264EncSetRateCtrl(p_ctx->hdl, &rate);
  assert(ret == H264ENC_OK);
}

void VENC_H264_DeInit()
{
  struct VENC_Context *p_ctx = &VENC_Instance;
  int ret = H264ENC_OK;

  if (p_ctx->hdl != NULL)
  {
    ret = H264EncRelease(p_ctx->hdl);
    assert(ret == H264ENC_OK);
  }

  memset(p_ctx, 0, sizeof(*p_ctx));
  venc_hw_allocator_pos = venc_hw_allocator_buffer;
}

int VENC_H264_EncodeFrame(uint8_t *p_in, uint8_t *p_out, size_t out_len, int is_intra_force)
{
  size_t out_compressed_frame_len;
  int ret;

  ret = VENC_Encode(p_in, p_out, out_len, &out_compressed_frame_len, is_intra_force);

  return ret ? -1 : out_compressed_frame_len;
}


void VENC_JPEG_Init(ENC_Conf_t *p_conf){
  struct VENC_Context *p_ctx = &VENC_Instance;
  JpegEncCfg cfg;
  JpegEncRet ret;
#if 0
  if (p_conf == NULL)
  {
	return -1;
  }
#endif

  venc_hw_allocator_pos = venc_hw_allocator_buffer;
  memset(p_ctx, 0, sizeof(*p_ctx));
  memset(&cfg, 0, sizeof(cfg));

  printf("JPEG Init: %dx%d\n", p_conf->width, p_conf->height);
  printf("JPEG Init: calling JpegEncInit\n");

  p_ctx->kind = ENC_KIND_JPEG;
  //p_ctx->width = p_conf->width;
  //p_ctx->height = p_conf->height;
  //p_ctx->fps = p_conf->fps;
  //p_ctx->format = p_conf->format;

  cfg.qLevel = 5; /* qualità media: adattabile */
  cfg.frameType = JPEGENC_YUV422_INTERLEAVED_YUYV;
  cfg.codingType = JPEGENC_WHOLE_FRAME;
  cfg.markerType = JPEGENC_SINGLE_MARKER;

  cfg.unitsType = JPEGENC_NO_UNITS;
  cfg.xDensity = 1;
  cfg.yDensity = 1;

  cfg.inputWidth = p_conf->width;
  cfg.inputHeight = p_conf->height;
  cfg.codingWidth = p_conf->width;
  cfg.codingHeight = p_conf->height;
  cfg.xOffset = 0;
  cfg.yOffset = 0;
  cfg.rotation = JPEGENC_ROTATE_0;
  cfg.restartInterval = 0;
  cfg.codingMode = JPEGENC_422_MODE;

  cfg.inputLineBufEn = 0;
  cfg.inputLineBufLoopBackEn = 0;
  cfg.inputLineBufDepth = 0;
  cfg.inputLineBufHwModeEn = 0;

  ret = JpegEncInit(&cfg, &p_ctx->jpeg_hdl);
  printf("JPEG Init: JpegEncInit ret=%d\n", ret);
  if (ret != JPEGENC_OK)
  {
	return;
	  //return -1;
  }

  ret = JpegEncSetPictureSize(p_ctx->jpeg_hdl, &cfg);
  printf("JPEG Init: JpegEncSetPictureSize ret=%d\n", ret);
  if (ret != JPEGENC_OK)
  {
	JpegEncRelease(p_ctx->jpeg_hdl);
	p_ctx->jpeg_hdl = NULL;
	return;
	//return -1;
  }

  return;
  //return 0;
}

static int VENC_JPEG_EncodeFrame(uint8_t *p_in, uint8_t *p_out, size_t out_len)
{
  struct VENC_Context *p_ctx = &VENC_Instance;
  JpegEncIn enc_in;
  JpegEncOut enc_out;
  JpegEncRet ret;

  if ((p_ctx->jpeg_hdl == NULL) || (p_in == NULL) || (p_out == NULL))
  {
    return -1;
  }

  memset(&enc_in, 0, sizeof(enc_in));
  memset(&enc_out, 0, sizeof(enc_out));

  enc_in.pOutBuf = p_out;
  enc_in.busOutBuf = (ptr_t)p_out;
  enc_in.outBufSize = (u32)out_len;

  enc_in.busLum = (ptr_t)p_in;
  enc_in.busCb = 0;
  enc_in.busCr = 0;

  enc_in.frameHeader = 1;
  enc_in.lineBufWrCnt = 0;
  printf("JPEG Encode: start\n");

  ret = JpegEncEncode(p_ctx->jpeg_hdl, &enc_in, &enc_out, NULL, NULL);
  printf("JPEG Encode: ret=%d size=%lu\n", ret, (unsigned long)enc_out.jfifSize);
  if (ret != JPEGENC_FRAME_READY)
  {
    return -1;
  }

  return enc_out.jfifSize;
}

void VENC_JPEG_DeInit(void){
  struct VENC_Context *p_ctx = &VENC_Instance;

  if (p_ctx->jpeg_hdl != NULL)
  {
	(void)JpegEncRelease(p_ctx->jpeg_hdl);
  }

  memset(p_ctx, 0, sizeof(*p_ctx));
  venc_hw_allocator_pos = venc_hw_allocator_buffer;
}


void ENC_Init(ENC_Conf_t *p_conf)
{
  /* trasform to have return int */
#if 0
  if (p_conf == NULL)
  {
    return -1;
  }
#endif

  switch (p_conf->format)
  {
    case APP_STREAM_FMT_H264:
      VENC_H264_Init(p_conf);
      break;

    case APP_STREAM_FMT_JPEG:
      VENC_JPEG_Init(p_conf);
      break;

    default:
  }
}

void ENC_DeInit()
{

  struct VENC_Context *p_ctx = &VENC_Instance;

  switch (p_ctx->kind)
  {
    case ENC_KIND_H264:
      VENC_H264_DeInit();
      break;

    case ENC_KIND_JPEG:
      VENC_JPEG_DeInit();
      break;

    default:
  }
}

int ENC_EncodeFrame(uint8_t *p_in, uint8_t *p_out, size_t out_len, int is_intra_force){
	  struct VENC_Context *p_ctx = &VENC_Instance;

	  switch (p_ctx->kind)
	  {
	    case ENC_KIND_H264:
	      return VENC_H264_EncodeFrame(p_in, p_out, out_len, is_intra_force);

	    case ENC_KIND_JPEG:
	      return VENC_JPEG_EncodeFrame(p_in, p_out, out_len);

	    default:
	      return -1;
	  }
}

void *EWLmalloc(u32 n)
{
  void *res = malloc(n);

  assert(res);

  return res;
}

void EWLfree(void *p)
{
  free(p);
}

void *EWLcalloc(u32 n, u32 s)
{
  void *res = calloc(n, s);

  assert(res);

  return res;
}

/* Implement simple EWLMallocLinear. No dealloc supported */
i32 EWLMallocLinear(const void *instance, u32 size, EWLLinearMem_t *info)
{
  if (venc_hw_allocator_pos + size > venc_hw_allocator_buffer + VENC_ALLOCATOR_SIZE)
    return -1;

  info->size = size;
  info->virtualAddress = (u32 *) venc_hw_allocator_pos;
  info->busAddress = (ptr_t)info->virtualAddress;

  venc_hw_allocator_pos += size;

  return 0;
}
