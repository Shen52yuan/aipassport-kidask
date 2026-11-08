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
    /* v0.1.12：audio 指向启动期一次性分配的常驻下行缓冲，运行期不再 malloc。
       audio_static=true 时 kid_resp_free() 不得 free（下一轮 POST 会原地复用）。 */
    uint8_t *audio;
    size_t   audio_len;
    bool     audio_static;
    /* v0.1.10：响应 Content-Length（0 表示未知/chunked）。v0.1.12 起仅用于日志与
       「是否截断」提示，不再作为「能不能播」的硬门槛：Ogg 按页独立，尾部截断时
       前面完整页照样能播，硬判完整反而让每次正常回答都被丢弃并重发。 */
    size_t   content_len;
    /* v0.1.9 / 服务端 v0.2.0：X-Need-Retry: 1 表示「没听清 / 问题不完整」，
       此时 audio 是童声引导提示音而非答案，播完回待机等待孩子按 OK 补充。 */
    bool     need_retry;
    char     retry_reason[24];        /* no_speech/hallucination/too_short/incomplete */
} kid_resp_t;

/* v0.1.9：开机调用一次，生成本次开机的会话 id。之后每轮 POST 都带 X-Session-Id，
   服务端据此维护最近 3 轮上下文；重启换 id = 开新话题。不带该头则退化为无状态单轮。 */
void kid_net_init_session(void);

/* v0.1.12：在 app_main() 最早期（堆最空、尚无 Wi-Fi/TLS/LVGL 压力）一次性申请
   常驻缓冲，整个运行期复用。ESP32-C3 无 PSRAM，以前每轮问答按 Content-Length
   现场 realloc 约 40KB 连续块，在「上行 Ogg + TLS + LVGL」同时驻留时偶发失败
   → body 收不全 → 设备报「声音没收到」并重发。

   v0.1.13：缓冲**同时**作为上行录音 Ogg 的输出区（录音与接收在时间上不重叠）。
   上一版单独拿 64KB 只做下行，把启动后剩余堆从 84KB 压到 18KB，Opus 编码器
   （十几 KB）创建失败 → 屏幕「录音出问题了」（2026-11-08 实机）。共用一块后
   常驻占用降到 40KB，录音阶段只需再拿编码器与 6.4KB 工作区。 */
void kid_net_init_rx(void);

/* v0.1.13：取常驻共享缓冲供录音使用（返回 NULL 表示缓冲申请失败）。
   用法：录音前取，录完把数据交给 kid_net_post_ogg()；不要在两次调用之间保存指针。 */
uint8_t *kid_net_pool(size_t *cap);

/* 上行：把整段 Ogg Opus（Content-Type: application/ogg）POST 到端点。
   成功返回 ESP_OK；resp.status==200 表示业务成功。失败时 resp.audio==NULL。 */
esp_err_t kid_net_post_ogg(const uint8_t *ogg, size_t ogg_len, kid_resp_t *out);

/* 释放 resp.audio（仅当非 NULL）。 */
void kid_resp_free(kid_resp_t *r);
