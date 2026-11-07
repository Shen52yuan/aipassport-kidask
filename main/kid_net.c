// main/kid_net.c —— KidAsk 网络：把整段 Ogg Opus 上行 POST 到自建端点，
// 回收 X-Answer-Text / X-Asr-Text（URL 解码）与下行 Ogg Opus 音频主体。
#include "kid_net.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "kid_net";

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
        if (strcasecmp(ev->header_key, "X-Answer-Text") == 0) {
            c->resp->answer_len = url_decode(ev->header_value, ev->data_len,
                                             c->resp->answer, KID_ANSWER_CAP);
        } else if (strcasecmp(ev->header_key, "X-Asr-Text") == 0) {
            c->resp->asr_len = url_decode(ev->header_value, ev->data_len,
                                          c->resp->asr, KID_ASR_CAP);
        } else if (strcasecmp(ev->header_key, "Content-Length") == 0) {
            long n = strtol(ev->header_value, NULL, 10);
            if (n > 0 && n <= 256 * 1024) c->expected_len = (size_t)n;
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
        .is_async = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { out->status = 0; return ESP_FAIL; }

    // 走系统默认 CA 包（含 ISRG Root X1 / Let's Encrypt），严禁 setInsecure。
    esp_http_client_set_header(h, "Content-Type", "application/ogg");
    esp_http_client_set_post_field(h, (const char *)ogg, (int)ogg_len);

    esp_err_t err = esp_http_client_perform(h);
    out->status = esp_http_client_get_status_code(h);
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
