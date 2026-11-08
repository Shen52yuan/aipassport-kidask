#pragma once
#include <stdint.h>
#include <stdbool.h>
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
    /* v0.1.10：响应 Content-Length（0 表示未知/chunked）。上层据此判断音频是否
       收全；旧版在 esp_http_client_perform 返回非 OK 时直接丢掉已收 body，
       造成「服务端 200 且答案文案到位、设备却无音频可播」的静默失败。 */
    size_t   content_len;
    /* v0.1.9 / 服务端 v0.2.0：X-Need-Retry: 1 表示「没听清 / 问题不完整」，
       此时 audio 是童声引导提示音而非答案，播完回待机等待孩子按 OK 补充。 */
    bool     need_retry;
    char     retry_reason[24];        /* no_speech/hallucination/too_short/incomplete */
} kid_resp_t;

/* v0.1.9：开机调用一次，生成本次开机的会话 id。之后每轮 POST 都带 X-Session-Id，
   服务端据此维护最近 3 轮上下文；重启换 id = 开新话题。不带该头则退化为无状态单轮。 */
void kid_net_init_session(void);

/* 上行：把整段 Ogg Opus（Content-Type: application/ogg）POST 到端点。
   成功返回 ESP_OK；resp.status==200 表示业务成功。失败时 resp.audio==NULL。 */
esp_err_t kid_net_post_ogg(const uint8_t *ogg, size_t ogg_len, kid_resp_t *out);

/* 释放 resp.audio（仅当非 NULL）。 */
void kid_resp_free(kid_resp_t *r);
