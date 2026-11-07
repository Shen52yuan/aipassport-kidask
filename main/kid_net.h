#pragma once
#include <stdint.h>
#include "esp_err.h"

/* 固定端点（任务卡 §7 真值，末尾必须带 /）。设备侧仅此一个字符串常量（协议「不鉴权」折中：
   随机不可猜路径 + nginx 限流）。 */
#define KID_ENDPOINT "https://tonynotatom.xyz/kid/ask-bb99ef8c69b6d89f8c6f474ebe97fc55/"
#define KID_HTTP_TIMEOUT_MS 30000

#define KID_ANSWER_CAP 512   /* X-Answer-Text URL 解码后字节上限（任务卡 ≤120 字 ≈360B） */
#define KID_ASR_CAP    512   /* X-Asr-Text 上限 */

typedef struct {
    int      status;        /* HTTP 状态码；0 表示连接级失败（DNS/TLS/超时） */
    char     answer[KID_ANSWER_CAP];  /* URL 解码后的要点文本（UTF-8，\0 结尾） */
    size_t   answer_len;
    char     asr[KID_ASR_CAP];        /* URL 解码后的识别文本（UTF-8，\0 结尾） */
    size_t   asr_len;
    uint8_t *audio;         /* Ogg Opus 音频主体（堆分配，需 kid_resp_free 释放） */
    size_t   audio_len;
} kid_resp_t;

/* 上行：把整段 Ogg Opus（Content-Type: application/ogg）POST 到端点。
   成功返回 ESP_OK；resp.status==200 表示业务成功。失败时 resp.audio==NULL。 */
esp_err_t kid_net_post_ogg(const uint8_t *ogg, size_t ogg_len, kid_resp_t *out);

/* 释放 resp.audio（仅当非 NULL）。 */
void kid_resp_free(kid_resp_t *r);
