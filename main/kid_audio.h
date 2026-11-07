// main/kid_audio.h —— 录音（麦克风 → Opus → Ogg）与播放（Ogg → Opus → 喇叭）。
//
// 录音用按住 OK 触发（见 main.c）：本模块只负责「采集→编码→封装」与「解封装→解码→播放」，
// 不碰网络、不碰 UI。电平回调由 main 转交 app_ui_set_level 驱动呼吸圈。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define KID_REC_MAX_MS   10000   // 单次录音上限（任务卡 ≤10s）
#define KID_REC_MIN_MS   300     // 过短视为误触，丢弃不发送

// ES8311 输出音量 0..100。⚠ BSP 内部 s_volume 是静态变量，默认 0（= 静音），
// 且 bsp_audio_set_format() 每次 open 都会把它写进 codec。必须显式设置，
// 否则 PCM 全部写进 I2S 但喇叭一点声音都没有（v0.1.6/v0.1.7 实机现象）。
// v0.1.8: 提到 100 排除音量偏小；并在 set_format 之后、首个包写入前各重设一次。
#define KID_VOLUME       100

// 录音 → Opus 编码 → Ogg Opus 封装。
// 成功时 *out_ogg 指向堆分配缓冲区（调用方 free()），*out_len 为字节数，返回 ESP_OK。
// 录音过短（< KID_REC_MIN_MS）返回 ESP_ERR_INVALID_STATE，不分配。
// 其它失败返回对应错误码，*out_ogg 置 NULL。
//   should_stop  ：非空时每帧后调用，返回 true 立即结束录音（仍封装已采集数据）。
//   on_level     ：非空时每帧回调 0..100 电平。
esp_err_t kid_audio_record_ogg(uint8_t **out_ogg, size_t *out_len,
                               bool (*should_stop)(void), void (*on_level)(int));

// Ogg Opus → Opus 解码 → I2S 直放。阻塞直到播完或出错。
// 解码目标采样率跟随 Ogg OpusHead（服务器约定 16k），与原 QCA 的 24k→16k 重采样说再见。
esp_err_t kid_audio_play_ogg(const uint8_t *ogg, size_t len);

// 开机自检：不依赖网络，播 1 秒 1kHz 正弦波，并回读 ES8311 关键寄存器。
// 用来一次区分「音量被重置」与「硬件/寄存器级静音」。由 app_main() 调用。
void kid_audio_selftest(void);

// 通过共享 I2C 总线回读 ES8311 寄存器（0x32=DAC 音量、0x31/0x37=静音/输出控制）。
void kid_audio_dump_regs(const char *tag);
