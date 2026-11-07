#pragma once
#include <stdint.h>

#define KID_OPUS_SR        16000      /* 上行采样率（麦克风采集 / 编码） */
#define KID_OPUS_CH        1
#define KID_OPUS_FRAME_MS  20
#define KID_OPUS_FRAME_SAMPLES (KID_OPUS_SR * KID_OPUS_FRAME_MS / 1000)  /* 320 samples/帧 */
#define KID_OPUS_BITRATE   24000      /* 与下行 24kbps 同档；上行亦用此档 */

/* 上行编码：麦克风 PCM16(16k/mono) → Opus 包（调用方按 20ms 帧循环）。 */
typedef struct kid_encoder *kid_encoder_t;
kid_encoder_t kid_encoder_create(int bitrate);
/* 返回写入 out 的字节数；<0 表示错误（OPUS_ 错误码取负）。 */
int  kid_encoder_encode(kid_encoder_t enc, const int16_t *pcm, int samples, uint8_t *out, int out_cap);
void kid_encoder_destroy(kid_encoder_t enc);

/* 下行解码：Ogg Opus → PCM16。rate/channels/preskip 取自 Ogg OpusHead。 */
typedef struct kid_decoder *kid_decoder_t;
kid_decoder_t kid_decoder_create(uint32_t rate, uint8_t channels, uint16_t preskip);
/* 返回每通道采样数；<0 表示错误。out 需容纳 out_samples 每通道。 */
int  kid_decoder_decode(kid_decoder_t dec, const uint8_t *opus, int len, int16_t *out, int out_samples);
void kid_decoder_destroy(kid_decoder_t dec);
