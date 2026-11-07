// main/kid_net.c —— KidAsk 网络：把整段 Ogg Opus 上行 POST 到自建端点，
// 回收 X-Answer-Text / X-Asr-Text（URL 解码）与下行 Ogg Opus 音频主体。
#include "kid_net.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "kid_net";

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
    size_t      expected_len;   // Content-Length：一次分配到位，避免 realloc 翻倍峰值
} net_ctx_t;

static esp_err_t on_event(esp_http_client_event_t *ev) {
    net_ctx_t *c = (net_ctx_t *)ev->user_data;
    switch (ev->event_id) {
    case HTTP_EVENT_ON_HEADER:
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
            } else if (strcasecmp(ev->header_key, "Content-Length") == 0) {
                long n = strtol(ev->header_value, NULL, 10);
                if (n > 0 && n <= 256 * 1024) c->expected_len = (size_t)n;
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (ev->data_len > 0) {
            size_t need = c->len + (size_t)ev->data_len;
            if (need > c->cap) {
                size_t ncap = c->cap ? c->cap * 2
                                     : (c->expected_len ? c->expected_len : 8192);
                while (ncap < need) ncap *= 2;
                uint8_t *nb = (uint8_t *)realloc(c->buf, ncap);
                if (!nb) { ESP_LOGE(TAG, "音频缓冲分配失败"); return ESP_ERR_NO_MEM; }
                c->buf = nb; c->cap = ncap;
            }
            memcpy(c->buf + c->len, ev->data, ev->data_len);
            c->len += ev->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t kid_net_post_ogg(const uint8_t *ogg, size_t ogg_len, kid_resp_t *out) {
    memset(out, 0, sizeof(*out));
    net_ctx_t c = { .resp = out, .buf = NULL, .len = 0, .cap = 0 };

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
    esp_http_client_set_post_field(h, (const char *)ogg, (int)ogg_len);

    esp_err_t err = esp_http_client_perform(h);
    out->status = esp_http_client_get_status_code(h);
    ESP_LOGI(TAG, "HTTP 完成: err=%s status=%d audio=%uB answer=%uB asr=%uB",
             esp_err_to_name(err), out->status, (unsigned)c.len,
             (unsigned)out->answer_len, (unsigned)out->asr_len);
    if (err == ESP_OK) {
        out->audio = c.buf;
        out->audio_len = c.len;
    } else {
        free(c.buf);
        out->audio = NULL;
        out->audio_len = 0;
    }
    esp_http_client_cleanup(h);
    return err;
}

void kid_resp_free(kid_resp_t *r) {
    if (r && r->audio) { free(r->audio); r->audio = NULL; r->audio_len = 0; }
}
