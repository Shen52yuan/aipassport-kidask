// main/kid_opus.c —— 直接调用 78/esp-opus（libopus 移植）核心 API 做 Opus 编码/解码。
// 上行：麦克风 PCM16(16k/mono) → Opus；下行：Ogg Opus → PCM16 供 I2S 直放。
#include "kid_opus.h"
#include "opus.h"
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "kid_opus";

struct kid_encoder {
    OpusEncoder *enc;
};

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

kid_decoder_t kid_decoder_create(uint32_t rate, uint8_t channels, uint16_t preskip) {
    (void)preskip;
    int err = 0;
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK || !d) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d (rate=%u ch=%u)", err, rate, channels);
        return NULL;
    }
    kid_decoder_t k = (kid_decoder_t)malloc(sizeof(*k));
    if (!k) { opus_decoder_destroy(d); return NULL; }
    k->dec = d;
    return k;
}

int kid_decoder_decode(kid_decoder_t dec, const uint8_t *opus, int len, int16_t *out, int out_samples) {
    if (!dec || !dec->dec) return -1;
    return opus_decode(dec->dec, opus, len, out, out_samples, 0);
}

void kid_decoder_destroy(kid_decoder_t dec) {
    if (!dec) return;
    if (dec->dec) opus_decoder_destroy(dec->dec);
    free(dec);
}
