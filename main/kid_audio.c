// main/kid_audio.c —— 见 kid_audio.h。
//
// 设备端没有 PSRAM，故录音缓冲走堆（≤10s@24kbps Opus ≈ 30KB Ogg），播放前整段
// Ogg 已落内存（服务端≤70KB），逐包解码直放。Opus 编解码直接调用 78/esp-opus 核心库。
#include "kid_audio.h"

#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "kid_opus.h"
#include "ogg_mux.h"
#include "ogg_demux.h"

static const char *TAG = "kid_audio";

// 16k/mono/20ms 一帧 = 320 样本 = 640 字节 PCM16。
#define FRAME_SAMPLES  320
#define FRAME_BYTES   (FRAME_SAMPLES * 2)

// 容纳一页封装结果（最大包 ≤1275 字节 + Ogg 页头 ≤64 字节，留足余量）。
#define PAGE_CAP       1536

// ---------------------------------------------------------------------------
// 可增长的输出缓冲：逐页追加 Ogg 页，避免一次性缓存全部裸 Opus 包（太占内存）。
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} grow_t;

static bool grow_append(grow_t *g, const uint8_t *d, size_t n) {
    if (g->len + n > g->cap) {
        size_t nc = (g->len + n) * 2 + 256;
        uint8_t *np = realloc(g->p, nc);
        if (!np) return false;
        g->p = np;
        g->cap = nc;
    }
    memcpy(g->p + g->len, d, n);
    g->len += n;
    return true;
}

// 平均绝对值电平（0..100），整数运算避免浮点/sqrt。
static int frame_level(const int16_t *pcm, int samples) {
    int32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        sum += v < 0 ? -v : v;
    }
    int avg = sum / samples;
    int lvl = avg * 100 / 8000;   // 语音平均幅值通常数百~数千
    return lvl > 100 ? 100 : lvl;
}

// 录音工作区约 6.4KB，必须放堆上：app 任务栈只有 16KB，而 libopus 编码、LVGL 与
// 后续 mbedTLS 调用链同样吃栈。v0.1.2 曾把这组数组全放栈上，按住 OK 即触发
// "Guru Meditation Error: Stack protection fault" 并整机重启。
typedef struct {
    int16_t  pcm[FRAME_SAMPLES];
    uint8_t  opus[1275];                 // Opus 单包上限 1275 字节
    uint8_t  page[PAGE_CAP];
    uint8_t  pending_page[PAGE_CAP];     // 已封装、待落盘的上一页（eos=false，CRC 有效）
    uint8_t  pending_pkt[1275];          // 上一页对应的 Opus 包（结尾重建 eos 页用）
    uint8_t  hdr[192];      // 头部两页（BOS OpusHead + OpusTags）合计约 115 字节
} rec_ws_t;

esp_err_t kid_audio_record_ogg(uint8_t **out_ogg, size_t *out_len,
                               bool (*should_stop)(void), void (*on_level)(int)) {
    *out_ogg = NULL;
    *out_len = 0;

    esp_err_t e = bsp_audio_set_format(KID_OPUS_SR, 16, KID_OPUS_CH);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "录音采样格式设置失败: %s", esp_err_to_name(e));
        return e;
    }

    ESP_LOGI(TAG, "创建 Opus 编码器 ...");
    kid_encoder_t enc = kid_encoder_create(KID_OPUS_BITRATE);
    if (!enc) return ESP_FAIL;
    ESP_LOGI(TAG, "Opus 编码器就绪，进入录音循环");

    rec_ws_t *w = (rec_ws_t *)calloc(1, sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "录音工作区分配失败 (需要 %u 字节)", (unsigned)sizeof(*w));
        kid_encoder_destroy(enc);
        return ESP_ERR_NO_MEM;
    }

    ogg_mux_t m, m_before_pending = {0};
    ogg_mux_init(&m, esp_random());
    grow_t g = {0};
    int    pending_page_len = 0;
    int    pending_pkt_len  = 0;
    const int per_pkt_samples = FRAME_SAMPLES * 3;   // 16k → 48k 粒度增量
    esp_err_t ret = ESP_FAIL;

    int hn = ogg_mux_header_page(&m, w->hdr, sizeof(w->hdr));
    if (hn < 0 || !grow_append(&g, w->hdr, (size_t)hn)) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    const int64_t t0 = esp_timer_get_time();

    for (;;) {
        if (should_stop && should_stop()) break;
        const int64_t now = esp_timer_get_time();
        if (now - t0 >= (int64_t)KID_REC_MAX_MS * 1000) break;

        if (bsp_audio_read(w->pcm, FRAME_BYTES) != ESP_OK) {
            // 瞬态读失败：跳过本帧，不计入时长。
            continue;
        }
        if (on_level) on_level(frame_level(w->pcm, FRAME_SAMPLES));

        int on = kid_encoder_encode(enc, w->pcm, FRAME_SAMPLES, w->opus, sizeof(w->opus));
        if (on <= 0) continue;

        // 上一页已封装完成（eos=false），先落盘。
        if (pending_page_len > 0 &&
            !grow_append(&g, w->pending_page, (size_t)pending_page_len)) {
            ret = ESP_ERR_NO_MEM;
            goto done;
        }
        // 封装当前页（eos=false），记录快照与源包，待下一轮落盘或结尾重建。
        m_before_pending = m;
        int pn = ogg_mux_data_page(&m, w->opus, on, false, per_pkt_samples,
                                   w->page, sizeof(w->page));
        if (pn < 0) continue;
        memcpy(w->pending_page, w->page, (size_t)pn);
        pending_page_len = pn;
        memcpy(w->pending_pkt, w->opus, (size_t)on);
        pending_pkt_len = on;
    }

    const int64_t dur_ms = (esp_timer_get_time() - t0) / 1000;
    if (dur_ms < KID_REC_MIN_MS || pending_page_len == 0) {
        ESP_LOGW(TAG, "录音过短(%lldms)或为空，丢弃", (long long)dur_ms);
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    // 结尾：用快照重建最后一页并置 eos（重新计算 CRC，保证合法）。
    m = m_before_pending;
    int pn = ogg_mux_data_page(&m, w->pending_pkt, pending_pkt_len, true, per_pkt_samples,
                               w->page, sizeof(w->page));
    if (pn < 0 || !grow_append(&g, w->page, (size_t)pn)) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    // grow_append 按 2 倍扩容，交付前收缩到实际长度，避免上行期间白占一倍堆。
    uint8_t *tight = (uint8_t *)realloc(g.p, g.len);
    if (tight) g.p = tight;

    *out_ogg = g.p;
    *out_len = g.len;
    g.p = NULL;                       // 所有权移交调用方
    ESP_LOGI(TAG, "录音完成: %lldms, Ogg %u 字节", (long long)dur_ms, (unsigned)g.len);
    ret = ESP_OK;

done:
    kid_encoder_destroy(enc);
    free(w);
    free(g.p);
    return ret;
}

// ---------------------------------------------------------------------------
// 播放：Ogg Opus → 解码 → I2S。OpusHead 给出解码目标 rate/channels，preskip 直接丢弃。
// ---------------------------------------------------------------------------
typedef struct {
    kid_decoder_t dec;
    uint32_t      rate;
    uint8_t       ch;
    int           preskip;       // 剩余待丢弃的解码样本数（按解码 rate）
    int16_t      *pcm;
    int           pcm_cap;       // 解码缓冲容量（样本数/每通道）
} play_ctx_t;

static void play_on_head(void *user, uint32_t rate, int channels, int preskip) {
    play_ctx_t *c = (play_ctx_t *)user;
    c->rate = rate;
    c->ch = (uint8_t)(channels > 0 ? channels : 1);
    c->preskip = preskip;
    c->dec = kid_decoder_create(rate, c->ch, (uint16_t)preskip);
    if (!c->dec) {
        ESP_LOGE(TAG, "播放解码器创建失败 (rate=%u ch=%d)", rate, channels);
        return;
    }
    // 解码目标采样率 = OpusHead 声明的 rate；本机服务器约定 16k。
    esp_err_t e = bsp_audio_set_format(rate, 16, c->ch);
    if (e != ESP_OK) ESP_LOGE(TAG, "播放采样格式设置失败: %s", esp_err_to_name(e));
    // 每 20ms 帧：最大 48k*0.12s ≈ 5760 样本；留 8k 余量。
    c->pcm_cap = 8192;
    c->pcm = malloc((size_t)c->pcm_cap * c->ch * 2);
}

static void play_on_packet(void *user, const uint8_t *data, int plen) {
    play_ctx_t *c = (play_ctx_t *)user;
    if (!c->dec || !c->pcm || plen <= 0) return;

    int n = kid_decoder_decode(c->dec, data, plen, c->pcm, c->pcm_cap);
    if (n <= 0) return;

    int skip = c->preskip;
    if (skip > n) skip = n;
    c->preskip -= skip;
    int out_n = n - skip;
    if (out_n <= 0) return;

    const uint8_t *src = (const uint8_t *)c->pcm + (size_t)skip * c->ch * 2;
    size_t wb = (size_t)out_n * c->ch * 2;
    size_t w = 0;
    while (w < wb) {
        esp_err_t e = bsp_audio_write(src + w, wb - w);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "播放写入中断: %s", esp_err_to_name(e));
            return;
        }
        w += (wb - w);   // esp_codec_dev_write 阻塞写满 data_size
    }
}

esp_err_t kid_audio_play_ogg(const uint8_t *ogg, size_t len) {
    if (!ogg || len == 0) return ESP_ERR_INVALID_ARG;

    play_ctx_t c = {0};
    ogg_demux_ret_t r = ogg_demux_parse(ogg, (int)len, &c, play_on_head, play_on_packet);
    if (r != OGG_DEMUX_OK) {
        ESP_LOGE(TAG, "Ogg 解封装失败");
        if (c.dec) kid_decoder_destroy(c.dec);
        free(c.pcm);
        return ESP_FAIL;
    }
    if (!c.dec) {
        ESP_LOGE(TAG, "未解析到 OpusHead，无法播放");
        free(c.pcm);
        return ESP_FAIL;
    }
    kid_decoder_destroy(c.dec);
    free(c.pcm);
    ESP_LOGI(TAG, "播放完成 (rate=%u ch=%u)", c.rate, c.ch);
    return ESP_OK;
}
