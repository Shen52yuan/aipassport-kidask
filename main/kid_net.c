// main/kid_net.c —— KidAsk 网络：把整段 Ogg Opus 上行 POST 到自建端点，
// 回收 X-Answer-Text / X-Asr-Text（URL 解码）与下行 Ogg Opus 音频主体。
#include "kid_net.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"

static const char *TAG = "kid_net";

/* v0.1.12：常驻下行接收缓冲。
   v0.1.13：从 64KB 降到 40KB，并**同时**用作上行录音 Ogg 的输出区 —— 上一版
   单独占 64KB 只服务下行，把启动后剩余堆从 84KB 压到 18KB（实测日志
   `剩余堆 18440`），Opus 编码器创建失败，屏幕直接报「录音出问题了」。
   录音（上行）与接收（下行）在时间上严格不重叠：
     录音写池 → POST 把池当请求体发出 → 服务端响应头到达（ON_HEADER）→
     响应体再写回池。
   HTTP/1.1 下请求体必然先写完才会收到响应头，所以接收覆盖池是安全的；
   ON_DATA 里额外校验 hdr_seen 作为结构性保护。
   全设备只有一个 app 任务串行收发，无需加锁。

   v0.1.14：池尺寸维持 40KB 不变 —— 实测下行回答 12~56KB，缩小会截掉长回答尾巴。
   真正致命的是「池常驻后 HTTPS 阶段只剩 45KB 堆」，一次 TLS 会话要吃掉约 44KB
   （实机 min 堆 996B/768B），正好卡在悬崖边：同一固件有时 -0x2700 握手失败、
   有时请求体发不出去（服务端侧 nginx 记 499/408）。余量改从 LVGL 池、Wi-Fi
   收发缓冲与 TLS 1.3 开关里省（见 sdkconfig.defaults），池本身不动。 */
#define KID_RX_CAP        (40 * 1024)
#define KID_RX_CAP_MIN    (28 * 1024)
static uint8_t *s_rx_buf;
static size_t   s_rx_cap;

void kid_net_init_rx(void) {
    if (s_rx_buf) return;
    s_rx_cap = KID_RX_CAP;
    s_rx_buf = (uint8_t *)heap_caps_malloc(s_rx_cap, MALLOC_CAP_8BIT);
    if (!s_rx_buf) {
        s_rx_cap = KID_RX_CAP_MIN;
        s_rx_buf = (uint8_t *)heap_caps_malloc(s_rx_cap, MALLOC_CAP_8BIT);
    }
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "共享缓冲分配失败，将退回运行期动态分配（有碎片风险）");
        s_rx_cap = 0;
    } else {
        ESP_LOGI(TAG, "上行/下行共享缓冲就绪: %uB @ %p, 分配后剩余堆=%uB 最大连续块=%uB",
                 (unsigned)s_rx_cap, s_rx_buf,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
}

uint8_t *kid_net_pool(size_t *cap) {
    if (cap) *cap = s_rx_cap;
    return s_rx_buf;
}

/* v0.1.9：一次开机 = 一个会话。16 位十六进制随机串，服务端按此维护最近 3 轮
   问答文本（纯内存、5 分钟超时、不存音频）。重启即换新 id，等于开新话题。 */
static char s_session_id[17];

void kid_net_init_session(void) {
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    snprintf(s_session_id, sizeof(s_session_id), "%08x%08x",
             (unsigned)a, (unsigned)b);
    ESP_LOGI(TAG, "会话 id 已生成: %s", s_session_id);
}

// Let's Encrypt / ISRG Root X1 信任锚（公开根证书，非敏感信息）。
// 站点链: leaf -> YE2 -> Root YE -> ISRG Root X2 -> ISRG Root X1，
// 实测仅用 X1 即可验证通过；根证书有效期至 2035-06-04，续期不影响。
// 显式提供 CA 根，执行完整证书链与域名校验。
static const char ISRG_ROOT_X1_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode（%XX 与 + → 空格），src 长度 len，写入 dst（dst_cap 含 \0）。
static size_t url_decode(const char *src, size_t len, char *dst, size_t dst_cap) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < dst_cap; ) {
        if (src[i] == '%' && i + 2 < len) {
            int hi = hexval((unsigned char)src[i + 1]);
            int lo = hexval((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) { dst[j++] = (char)(hi * 16 + lo); i += 3; continue; }
        }
        if (src[i] == '+') { dst[j++] = ' '; i++; continue; }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
    return j;
}

typedef struct {
    kid_resp_t *resp;
    uint8_t    *buf;
    size_t      len;
    size_t      cap;
    size_t      expected_len;   // Content-Length，仅用于日志与截断提示
    bool        dynamic;        // buf 是运行期 malloc 的（常驻缓冲申请失败时的回退）
    bool        overflow;       // 常驻缓冲装不下，尾部被丢弃
    bool        hdr_seen;       // 已收到响应头 = 请求体肯定已发完，池可以安全覆盖
    bool        tx_in_pool;     // 本次请求体就是共享池（录音直接写池）
} net_ctx_t;

static esp_err_t on_event(esp_http_client_event_t *ev) {
    net_ctx_t *c = (net_ctx_t *)ev->user_data;
    switch (ev->event_id) {
    case HTTP_EVENT_ON_HEADER:
        // 收到响应头 ⇒ 请求体（可能就放在共享池里）已经全部发完，
        // 之后 ON_DATA 覆盖池不会破坏正在发送的数据。
        c->hdr_seen = true;
        // ⚠ esp_http_client 的 ON_HEADER 事件里 ev->data_len 恒为 0：头值是
        // NUL 结尾字符串，长度并不通过 data_len 传递。v0.1.6 误用 data_len
        // 作 url_decode 的长度，解出 0 字节，屏幕要点卡片因此一直空白。
        if (ev->header_key && ev->header_value) {
            const size_t vlen = strlen(ev->header_value);
            if (strcasecmp(ev->header_key, "X-Answer-Text") == 0) {
                c->resp->answer_len = url_decode(ev->header_value, vlen,
                                                 c->resp->answer, KID_ANSWER_CAP);
                ESP_LOGI(TAG, "收到屏幕要点: %u 字节", (unsigned)c->resp->answer_len);
            } else if (strcasecmp(ev->header_key, "X-Asr-Text") == 0) {
                c->resp->asr_len = url_decode(ev->header_value, vlen,
                                              c->resp->asr, KID_ASR_CAP);
            } else if (strcasecmp(ev->header_key, "X-Need-Retry") == 0) {
                /* 服务端 v0.2.0：1 = 没听清 / 问题不完整，body 是童声引导提示音。 */
                c->resp->need_retry = (ev->header_value[0] == '1');
            } else if (strcasecmp(ev->header_key, "X-Retry-Reason") == 0) {
                snprintf(c->resp->retry_reason, sizeof(c->resp->retry_reason),
                         "%s", ev->header_value);
            } else if (strcasecmp(ev->header_key, "Content-Length") == 0) {
                long n = strtol(ev->header_value, NULL, 10);
                if (n > 0 && n <= 256 * 1024) c->expected_len = (size_t)n;
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (ev->data_len > 0) {
            if (c->tx_in_pool && !c->hdr_seen) {
                // 结构性保护：请求体还放在共享池里、响应头还没到就写池，
                // 会破坏正在发送的录音。正常 HTTP/1.1 顺序下不会发生。
                ESP_LOGE(TAG, "异常时序：响应体早于响应头，拒绝覆盖上行数据");
                return ESP_FAIL;
            }
            size_t take = (size_t)ev->data_len;

            // 常驻缓冲未就绪时的回退：沿用运行期按需增长（有碎片风险，仅兜底）。
            if (!c->buf) {
                size_t ncap = c->expected_len ? c->expected_len : 8192;
                while (ncap < take) ncap *= 2;
                uint8_t *nb = (uint8_t *)malloc(ncap);
                if (!nb) {
                    ESP_LOGE(TAG, "下行缓冲分配失败(需要 %uB, 最大连续块 %uB)",
                             (unsigned)ncap,
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                    return ESP_ERR_NO_MEM;
                }
                c->buf = nb; c->cap = ncap; c->dynamic = true;
            }

            if (c->len + take > c->cap) {
                if (c->dynamic) {
                    size_t ncap = c->cap * 2;
                    while (ncap < c->len + take) ncap *= 2;
                    uint8_t *nb = (uint8_t *)realloc(c->buf, ncap);
                    if (!nb) {
                        ESP_LOGE(TAG, "下行缓冲扩容失败(需要 %uB)", (unsigned)ncap);
                        return ESP_ERR_NO_MEM;
                    }
                    c->buf = nb; c->cap = ncap;
                } else {
                    // 常驻缓冲封顶：保留已收到的完整页，绝不越界。
                    c->overflow = true;
                    take = c->cap - c->len;
                }
            }
            if (take) {
                memcpy(c->buf + c->len, ev->data, take);
                c->len += take;
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t kid_net_post_ogg(const uint8_t *ogg, size_t ogg_len, kid_resp_t *out) {
    memset(out, 0, sizeof(*out));
    // v0.1.12：直接复用启动期常驻缓冲，本轮 POST 不再有任何大块堆分配。
    // v0.1.13：录音也可能直接写进这块缓冲（省掉上行 60KB 的增长峰值）。
    net_ctx_t c = { .resp = out, .buf = s_rx_buf, .len = 0, .cap = s_rx_cap,
                    .tx_in_pool = (s_rx_buf && ogg == (const uint8_t *)s_rx_buf) };

    esp_http_client_config_t cfg = {
        .url = KID_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = KID_HTTP_TIMEOUT_MS,
        .event_handler = on_event,
        .user_data = &c,
        .buffer_size = 4096,       // 收方向 4KB（配合 tls_mfl 的 MFL 协商）
        .buffer_size_tx = 4096,    // 发方向 4KB
        .cert_pem = ISRG_ROOT_X1_PEM,   // 必须显式给 CA 根，否则 esp_tls 报
                                        // "No server verification option set" 直接握手失败
        .is_async = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { out->status = 0; return ESP_FAIL; }

    // 使用上方显式根证书做完整校验。
    esp_http_client_set_header(h, "Content-Type", "application/ogg");
    /* v0.1.9：服务端按 session id 串起最近 3 轮问答；空 id 时服务端退化为单轮。
       两端互不感知，可单独升级任意一端。 */
    if (s_session_id[0]) {
        esp_http_client_set_header(h, "X-Session-Id", s_session_id);
    }
    esp_http_client_set_post_field(h, (const char *)ogg, (int)ogg_len);

    esp_err_t err = esp_http_client_perform(h);
    out->status = esp_http_client_get_status_code(h);
    out->content_len = c.expected_len;
    ESP_LOGI(TAG, "HTTP 完成: err=%s status=%d 音频=%uB/Content-Length=%uB 溢出=%d "
                  "最大连续块=%uB min堆=%uB 文案=%uB asr=%uB retry=%d(%s)",
             esp_err_to_name(err), out->status, (unsigned)c.len,
             (unsigned)c.expected_len, c.overflow ? 1 : 0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)out->answer_len, (unsigned)out->asr_len,
             out->need_retry ? 1 : 0,
             out->retry_reason[0] ? out->retry_reason : "-");
    /* v0.1.10：无论 perform 返回什么，只要收到了 body 就交给上层。
       旧版在 err != ESP_OK 时 free(c.buf)，把「头已到（含答案文案）、音频已收
       大半」的响应整段丢掉，上层只能显示文案 + 两声嘀。
       v0.1.12：不再因「少于 Content-Length」就判失败 —— Ogg 每页独立带 CRC，
       解封装器会跳过被截断的尾页，前面完整页完全可以正常播。 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "传输异常 %s，保留已收 %uB（期望 %uB）",
                 esp_err_to_name(err), (unsigned)c.len, (unsigned)c.expected_len);
    }
    if (c.expected_len && c.len < c.expected_len) {
        ESP_LOGW(TAG, "响应被截断: 实收 %uB < Content-Length %uB，按已收完整页播放",
                 (unsigned)c.len, (unsigned)c.expected_len);
    }
    if (c.overflow) {
        ESP_LOGW(TAG, "下行超过常驻缓冲 %uB，尾部已丢弃（仍可播前面完整页）",
                 (unsigned)c.cap);
    }
    out->audio        = c.buf;
    out->audio_len    = c.len;
    out->audio_static = !c.dynamic;   // 常驻缓冲不可 free
    esp_http_client_cleanup(h);
    return err;
}

void kid_resp_free(kid_resp_t *r) {
    if (!r || !r->audio) return;
    if (!r->audio_static) free(r->audio);   // 仅释放运行期回退分配的缓冲
    r->audio = NULL;
    r->audio_len = 0;
}
