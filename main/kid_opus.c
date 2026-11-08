// main/kid_opus.c —— 直接调用 78/esp-opus（libopus 移植）核心 API 做 Opus 编码/解码。
// 上行：麦克风 PCM16(16k/mono) → Opus；下行：Ogg Opus → PCM16 供 I2S 直放。
#include "kid_opus.h"
#include "opus.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "kid_opus";

struct kid_encoder {
    OpusEncoder *enc;
};

// 编码器保持「每轮录音创建、录完销毁」：它与播放不同时存在，销毁后正好给下行音频
// 腾出堆空间。改常驻反而会让播放阶段多占 ~20KB，故不动。
kid_encoder_t kid_encoder_create(int bitrate) {
    int err = 0;
    OpusEncoder *e = opus_encoder_create(KID_OPUS_SR, KID_OPUS_CH, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e) {
        ESP_LOGE(TAG, "Opus 编码器创建失败: %d", err);
        return NULL;
    }
    opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(1));   // C3 算力有限，复杂度 1
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    kid_encoder_t k = (kid_encoder_t)malloc(sizeof(*k));
    if (!k) { opus_encoder_destroy(e); return NULL; }
    k->enc = e;
    return k;
}

int kid_encoder_encode(kid_encoder_t enc, const int16_t *pcm, int samples, uint8_t *out, int out_cap) {
    if (!enc || !enc->enc) return -1;
    return opus_encode(enc->enc, pcm, samples, out, out_cap);
}

void kid_encoder_destroy(kid_encoder_t enc) {
    if (!enc) return;
    if (enc->enc) opus_encoder_destroy(enc->enc);
    free(enc);
}

struct kid_decoder {
    OpusDecoder *dec;
};

// v0.1.11：解码器常驻复用。
// 原来每轮问答都 opus_decoder_create/destroy 一个约 20KB 的连续堆块；连续多轮后
// 堆碎片化，创建偶发失败 → play_on_head 拿不到 dec → kid_audio_play_ogg() 返回
// ESP_FAIL → 屏幕显示「声音没放出来」（2026-11-08 实机）。
// 现在只在首次（或采样率/声道变化时）申请一次，之后每轮用 OPUS_RESET_STATE 清状态，
// 既避免跨轮 PLC 状态污染，又彻底去掉播放阶段的大块分配。
static struct kid_decoder s_dec_inst;
static OpusDecoder       *s_dec;
static uint32_t           s_dec_rate;
static uint8_t            s_dec_ch;

kid_decoder_t kid_decoder_create(uint32_t rate, uint8_t channels, uint16_t preskip) {
    (void)preskip;
    if (s_dec && s_dec_rate == rate && s_dec_ch == channels) {
        opus_decoder_ctl(s_dec, OPUS_RESET_STATE);
        s_dec_inst.dec = s_dec;
        return &s_dec_inst;
    }
    if (s_dec) { opus_decoder_destroy(s_dec); s_dec = NULL; }

    int err = 0;
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK || !d) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d (rate=%u ch=%u) heap=%u largest=%u",
                 err, rate, channels,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return NULL;
    }
    s_dec = d;
    s_dec_rate = rate;
    s_dec_ch = channels;
    s_dec_inst.dec = d;
    ESP_LOGI(TAG, "Opus 解码器常驻就绪 (rate=%u ch=%u)", rate, channels);
    return &s_dec_inst;
}

int kid_decoder_decode(kid_decoder_t dec, const uint8_t *opus, int len, int16_t *out, int out_samples) {
    if (!dec || !dec->dec) return -1;
    return opus_decode(dec->dec, opus, len, out, out_samples, 0);
}

void kid_decoder_destroy(kid_decoder_t dec) {
    // 常驻句柄：只清引用，不真正释放；下一轮 create 会 RESET_STATE。
    if (dec) ((struct kid_decoder *)dec)->dec = NULL;
}
