// main/ogg_mux.h —— Ogg Opus 封装（RFC 7845），用于上行音频。
//
// 设备端把 esp-opus-encoder 产出的裸 Opus 包封装成标准 Ogg(OggS 页 + OpusHead +
// OpusTags + 数据页)。封装后 ffmpeg / opus 解码器可正常解开。内存友好：不分配大缓冲，
// 调用方提供输出缓冲；单页 ≤ 4KB（分帧封页）。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t serial;       // 本流随机序列号（esp_random()）
    uint32_t pageno;
    uint64_t granule;      // 已累计的 48kHz 采样数（仅数据页累加）
    bool bos_done;
} ogg_mux_t;

// 初始化封装器。serial 建议用 esp_random() 生成，保证与同机其它流不冲突。
void ogg_mux_init(ogg_mux_t *m, uint32_t serial);

// 构造头部两页：第 0 页 BOS 仅含 OpusHead，第 1 页仅含 OpusTags（RFC 7845）。
// out 容量需 ≥ 192 字节。返回两页总字节数，失败返回 < 0。
int ogg_mux_header_page(ogg_mux_t *m, uint8_t *out, size_t out_cap);

// 构造一个数据页面：把一个 Opus 数据包封装成一页。
// eos=true 时置末页标志。samples = 本包包含的 48kHz 采样数（用于 granulepos，
// 由调用方依据编码器帧长给出；例如 20ms@48k = 960）。
// out 容量需 ≥ len + 64。返回写入字节数，失败返回 < 0。
int ogg_mux_data_page(ogg_mux_t *m, const uint8_t *packet, int len,
                      bool eos, uint64_t samples, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
