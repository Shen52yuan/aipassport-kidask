// main/ogg_demux.h —— Ogg Opus 解封装（RFC 7845），用于下行音频解码前。
//
// 设备收到服务端返回的 audio/ogg（Opus 容器，标称 48kHz / 有效 16k / mono）后，先用本模块
// 按 OggS 页切分出 Opus 包：首包 OpusHead 给出 rate/channels/preskip（据此初始化解码器），
// 跳过 OpusTags，其余数据包含入解码器。支持跨页分包重组。
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OGG_DEMUX_OK = 0,
    OGG_DEMUX_ERR = -1,
} ogg_demux_ret_t;

// 一次性解析整段 Ogg 字节流（设备端 ≤ ~70KB，收完再播）。
// on_head    ：解析到 OpusHead 时回调（rate/channels/preskip，用于初始化解码器）。
// on_packet  ：每解出一个数据 Opus 包回调（喂给 esp-opus-decoder）。
// 返回 OGG_DEMUX_OK / OGG_DEMUX_ERR。
ogg_demux_ret_t ogg_demux_parse(const uint8_t *buf, int len, void *user,
                                void (*on_head)(void *user, uint32_t rate, int channels, int preskip),
                                void (*on_packet)(void *user, const uint8_t *data, int plen));

#ifdef __cplusplus
}
#endif
