// main/kid_audio.c —— 见 kid_audio.h。
//
// 设备端没有 PSRAM，故录音缓冲走堆（≤10s@24kbps Opus ≈ 30KB Ogg），播放前整段
// Ogg 已落内存（服务端≤70KB），逐包解码直放。Opus 编解码直接调用 78/esp-opus 核心库。
#include "kid_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "driver/i2c_master.h"
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

// v0.1.9 录音留白：儿童说话尾音常被松手瞬间切掉，按键机械声又会混进首帧。
// v0.1.10：改成「按一下开录 / 再按一下发送」后，开录动作由完整短按 CLICK 触发，
// 此时按键声早已过去，前导只需滤掉 codec 刚 open 的直流瞬态；留 150ms 反而会
// 吃掉孩子开口的第一个字。收尾仍补采 250ms 保护拖长的尾音。
#define KID_REC_LEADIN_MS  60
#define KID_REC_TAIL_MS    250

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
    int64_t stop_at = 0;

    for (;;) {
        // v0.1.9：松手后不立即收尾，再录 KID_REC_TAIL_MS。儿童常拖长尾音，
        // 之前松手瞬间切掉最后一个字，whisper 便识别成别的词甚至产生幻觉。
        if (should_stop && should_stop() && stop_at == 0) {
            stop_at = esp_timer_get_time();
        }
        const int64_t now = esp_timer_get_time();
        if (stop_at && now - stop_at >= (int64_t)KID_REC_TAIL_MS * 1000) break;
        if (now - t0 >= (int64_t)KID_REC_MAX_MS * 1000) break;

        if (bsp_audio_read(w->pcm, FRAME_BYTES) != ESP_OK) {
            // 瞬态读失败：跳过本帧，不计入时长。
            continue;
        }
        // 前导 KID_REC_LEADIN_MS 只消费不编码：按键机械声与 codec 刚 open 时的
        // 直流瞬态都落在这段，编进去会拉低服务端的有效语音判定。
        if (now - t0 < (int64_t)KID_REC_LEADIN_MS * 1000) continue;
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
    // v0.1.7 诊断：用来一次判定「解码出的是真音频」还是「静音/没写进 I2S」。
    uint32_t      packets;
    uint32_t      decoded_samples;
    uint32_t      written_bytes;
    uint32_t      write_errors;
    uint32_t      vol_sets;      // v0.1.8：播放链路里实际重设 ES8311 音量的次数
    int32_t       peak;          // PCM16 绝对峰值，0 表示解码结果全静音
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

    // ⚠ v0.1.8：上游 qca_voice 的顺序是 set_format() 之后才 set_volume()。
    // 只在 open 之前设 s_volume 不够：codec 打开/重配 DAC 后音量会被打回默认，
    // 表现是 PCM 全写进 I2S（werr=0、peak 正常）但喇叭无声。此时 s_opened 已为
    // true，setter 会真正写 ES8311 的 DAC 音量寄存器。
    bsp_audio_set_volume(KID_VOLUME);
    c->vol_sets++;
    ESP_LOGI(TAG, "播放前重设输出音量 %d%% (set_format=%s)", KID_VOLUME, esp_err_to_name(e));

    // 每 20ms 帧：最大 48k*0.12s ≈ 5760 样本；留 8k 余量。
    c->pcm_cap = 8192;
    c->pcm = malloc((size_t)c->pcm_cap * c->ch * 2);
}

static void play_on_packet(void *user, const uint8_t *data, int plen) {
    play_ctx_t *c = (play_ctx_t *)user;
    if (!c->dec || !c->pcm || plen <= 0) return;

    // v0.1.8：首个包写入前再设一次。esp_codec_dev_write() 首次调用可能触发
    // codec 内部 lazy 配置并重置 DAC 音量，这一下兜住该路径。
    if (c->packets == 0) {
        bsp_audio_set_volume(KID_VOLUME);
        c->vol_sets++;
    }

    int n = kid_decoder_decode(c->dec, data, plen, c->pcm, c->pcm_cap);
    if (n <= 0) return;
    c->packets++;
    c->decoded_samples += (uint32_t)n;
    for (int i = 0; i < n * c->ch; i++) {
        int32_t v = c->pcm[i] < 0 ? -(int32_t)c->pcm[i] : (int32_t)c->pcm[i];
        if (v > c->peak) c->peak = v;
    }

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
            c->write_errors++;
            ESP_LOGW(TAG, "播放写入中断: %s", esp_err_to_name(e));
            return;
        }
        c->written_bytes += (uint32_t)(wb - w);
        w += (wb - w);   // esp_codec_dev_write 阻塞写满 data_size
    }
}

esp_err_t kid_audio_play_ogg(const uint8_t *ogg, size_t len) {
    if (!ogg || len == 0) return ESP_ERR_INVALID_ARG;

    // ⚠ 必须在播放前设置输出音量：BSP 的 s_volume 静态变量默认 0（静音），
    // 且 esp_codec_dev_open() 每次都会把它写进 ES8311 的 DAC 音量寄存器。
    // v0.1.6 之前 KidAsk 从未调用过本函数，PCM 全部写出但喇叭无声。
    // 这里再设一次也能兜住 BSP 失败回滚（audio_reset_state 会把 s_volume 清零）。
    bsp_audio_set_volume(KID_VOLUME);

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
    ESP_LOGI(TAG, "播放完成 (rate=%u ch=%u vol=%d volset=%u pkts=%u samples=%u bytes=%u peak=%d werr=%u)",
             c.rate, c.ch, KID_VOLUME, (unsigned)c.vol_sets, (unsigned)c.packets,
             (unsigned)c.decoded_samples, (unsigned)c.written_bytes,
             (int)c.peak, (unsigned)c.write_errors);
    kid_audio_dump_regs("play_done");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// v0.1.8 诊断：ES8311 寄存器回读 + 开机自检音
// ---------------------------------------------------------------------------
void kid_audio_dump_regs(const char *tag) {
    i2c_master_bus_handle_t bus = bsp_i2c_bus();
    if (!bus) {
        ESP_LOGW(TAG, "[%s] I2C 总线未就绪，跳过寄存器回读", tag);
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = 0x18,          // ES8311 7-bit 地址（见 bsp_i2c.h）
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "[%s] ES8311 设备句柄创建失败", tag);
        return;
    }
    // 0x32 = DAC 音量；0x31/0x37 = DAC 静音与输出控制；其余为电源/时钟状态。
    static const uint8_t regs[] = {0x0D, 0x0E, 0x12, 0x31, 0x32, 0x37, 0x45};
    char line[128];
    int off = 0;
    for (size_t i = 0; i < sizeof(regs) && off < (int)sizeof(line) - 12; i++) {
        uint8_t r = regs[i], v = 0;
        esp_err_t e = i2c_master_transmit_receive(dev, &r, 1, &v, 1, 200);
        off += snprintf(line + off, sizeof(line) - off, " %02X=%02X%s",
                        r, v, e == ESP_OK ? "" : "!");
    }
    ESP_LOGI(TAG, "[%s] ES8311 regs:%s", tag, line);
    i2c_master_bus_rm_device(dev);
}

// 16 点正弦表：16kHz 采样率时每周期 16 样本 = 1kHz，幅度 ±12000。
static const int16_t k_sine1k[16] = {
        0,  4595,  8505, 11100, 12000, 11100,  8505,  4595,
        0, -4595, -8505, -11100, -12000, -11100, -8505, -4595
};

void kid_audio_selftest(void) {
    ESP_LOGI(TAG, "=== 音频自检开始: 1kHz / 1s / 音量 %d%% ===", KID_VOLUME);
    esp_err_t e = bsp_audio_set_format(16000, 16, 1);
    ESP_LOGI(TAG, "自检 set_format: %s", esp_err_to_name(e));
    bsp_audio_set_volume(KID_VOLUME);
    kid_audio_dump_regs("selftest_before");

    static int16_t buf[800];              // 1600B = 50ms @16k/mono，静态区避免占任务栈
    uint32_t bytes = 0, errs = 0;
    for (int block = 0; block < 20; block++) {
        for (int i = 0; i < 800; i++) buf[i] = k_sine1k[(block * 800 + i) & 15];
        bsp_audio_set_volume(KID_VOLUME); // 兜住 esp_codec_dev 的 lazy 重配
        esp_err_t w = bsp_audio_write(buf, sizeof(buf));
        if (w != ESP_OK) {
            errs++;
            ESP_LOGW(TAG, "自检写入失败: %s", esp_err_to_name(w));
        } else {
            bytes += sizeof(buf);
        }
    }
    ESP_LOGI(TAG, "=== 音频自检结束: 写入 %u 字节, 错误 %u ===", (unsigned)bytes, (unsigned)errs);
    kid_audio_dump_regs("selftest_after");
}

// v0.1.9：错误页/网络异常的短促「嘀」提示音。复用自检音正弦表与静态缓冲，
// 全程不占 app 任务栈（32KB 栈要留给 libopus 的 alloca，见 MEMORY 红线）。
// 注意：这不是语音；「没听清请补充」的童声引导由服务端 X-Need-Retry 提供。
void kid_audio_beep(int count) {
    if (count <= 0) count = 1;
    if (count > 4) count = 4;
    static int16_t buf[800];              // 50ms @16k/mono，静态区避免占栈
    esp_err_t e = bsp_audio_set_format(16000, 16, 1);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "提示音 set_format 失败: %s", esp_err_to_name(e));
        return;
    }
    bsp_audio_set_volume(KID_VOLUME);
    for (int n = 0; n < count; n++) {
        for (int block = 0; block < 2; block++) {   // 2 × 50ms = 100ms 一声
            for (int i = 0; i < 800; i++) buf[i] = k_sine1k[(block * 800 + i) & 15];
            bsp_audio_set_volume(KID_VOLUME);       // 兜住 codec lazy 重配
            if (bsp_audio_write(buf, sizeof(buf)) != ESP_OK) return;
        }
        memset(buf, 0, sizeof(buf));                 // 声间 50ms 静音
        bsp_audio_write(buf, sizeof(buf));
    }
    ESP_LOGI(TAG, "提示音播放完成 (%d 声)", count);
}
