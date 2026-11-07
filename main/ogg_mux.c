// main/ogg_mux.c —— 见 ogg_mux.h。Ogg Opus 上行封装（RFC 7845）。
#include "ogg_mux.h"

#include <string.h>

// ---- Ogg 标准 CRC-32（多项式 0x04C11DB7, 初值 0, 无反射/无最终异或）----
// 注意：与以太网 CRC32 不同，以太网是 init=0xFFFFFFFF + 最终异或；用错会让
// 服务端 ffmpeg 每页都报 "CRC mismatch!" 并拒收整段音频（v0.1.5 实测 422）。
static uint32_t s_crc_table[256];
static bool s_crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++)
            r = (r & 0x80000000U) ? (r << 1) ^ 0x04C11DB7U : (r << 1);
        s_crc_table[i] = r;
    }
    s_crc_ready = true;
}

static uint32_t crc32_ogg(const uint8_t *buf, int len) {
    if (!s_crc_ready) crc_init();
    uint32_t c = 0;
    for (int i = 0; i < len; i++)
        c = (c << 8) ^ s_crc_table[((c >> 24) ^ buf[i]) & 0xFF];
    return c;
}

void ogg_mux_init(ogg_mux_t *m, uint32_t serial) {
    m->serial = serial;
    m->pageno = 0;
    m->granule = 0;
    m->bos_done = false;
}

// 组装并写入一页。flags/granule 已按调用方要求填好；crc 字段先置 0 计算后回填。
// seg_data[i] / seg_len[i] 为分片表（Ogg lacing：每片 ≤ 255 字节，末片 < 255）。
static int build_page(const ogg_mux_t *m, uint8_t flags, uint64_t granule,
                      const uint8_t *seg_data[], const int seg_len[], int nseg,
                      uint8_t *out, size_t out_cap) {
    int total = 0;
    for (int i = 0; i < nseg; i++) total += seg_len[i];
    int header_len = 27 + nseg;
    if ((size_t)(header_len + total) > out_cap) return -1;

    uint8_t *p = out;
    memcpy(p, "OggS", 4); p += 4;
    *p++ = 0;                       // version
    *p++ = (uint8_t)flags;          // header type flags
    for (int i = 0; i < 8; i++) *p++ = (uint8_t)(granule >> (8 * i));   // granulepos LE
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(m->serial >> (8 * i)); // serial LE
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(m->pageno >> (8 * i)); // page no LE
    for (int i = 0; i < 4; i++) *p++ = 0;                              // CRC32（先 0）
    *p++ = (uint8_t)nseg;                                            // #segments
    for (int i = 0; i < nseg; i++) *p++ = (uint8_t)seg_len[i];        // segment table
    for (int i = 0; i < nseg; i++) {
        if (seg_len[i]) memcpy(p, seg_data[i], seg_len[i]);
        p += seg_len[i];
    }
    // CRC 覆盖整页（含 crc 字段的 0）；回填到偏移 22。
    uint32_t crc = crc32_ogg(out, header_len + total);
    for (int i = 0; i < 4; i++) out[22 + i] = (uint8_t)(crc >> (8 * i));
    return header_len + total;
}

// 计算分片表：把 len 字节切成 255 为单位的片，末片 < 255 表示包结束。
// 返回片数；seg_data/seg_len 由调用方提供足够容量（(len+254)/255，最多 64）。
static int lace_segments(const uint8_t *data, int len,
                         const uint8_t *seg_data[], int seg_len[], int max_seg) {
    if (len == 0) {  // 空包仍需一个 0 长度片
        if (max_seg < 1) return -1;
        seg_data[0] = data; seg_len[0] = 0;
        return 1;
    }
    // 末片必须 < 255 才代表本包结束；len 恰为 255 倍数时补一个 0 长度片。
    int n = len / 255 + 1;
    if (n > max_seg) return -1;
    for (int i = 0; i < n; i++) {
        int chunk = (i == n - 1) ? (len - i * 255) : 255;
        seg_data[i] = data + i * 255;
        seg_len[i] = chunk;
    }
    return n;
}

int ogg_mux_header_page(ogg_mux_t *m, uint8_t *out, size_t out_cap) {
    // OpusHead：magic(8) + ver(1)=1 + channels(1)=1 + preskip(2 LE)=312 +
    //          rate(4 LE)=16000 + gain(2 LE)=0 + mapping(1)=0  → 19 字节
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                          // version
    head[9] = 1;                          // channels = 1
    head[10] = 312 & 0xFF; head[11] = 312 >> 8;   // preskip (匹配 libopus 默认 312)
    uint32_t rate = 16000;
    for (int i = 0; i < 4; i++) head[12 + i] = (uint8_t)(rate >> (8 * i));
    head[16] = 0; head[17] = 0;           // gain = 0
    head[18] = 0;                         // mapping family = 0

    // OpusTags：magic(8) + vendor_len(4) + vendor + ntags(4) + [tag_len(4)+tag]...
    const char *vendor = "KidAsk";
    const char *tag = "encoder=KidAsk";
    uint8_t tags[64];
    int tl = 0;
    memcpy(tags, "OpusTags", 8); tl += 8;
    uint32_t vlen = (uint32_t)strlen(vendor);
    for (int i = 0; i < 4; i++) tags[tl++] = (uint8_t)(vlen >> (8 * i));
    memcpy(tags + tl, vendor, vlen); tl += vlen;
    uint32_t ntags = 1;
    for (int i = 0; i < 4; i++) tags[tl++] = (uint8_t)(ntags >> (8 * i));
    uint32_t tlen = (uint32_t)strlen(tag);
    for (int i = 0; i < 4; i++) tags[tl++] = (uint8_t)(tlen >> (8 * i));
    memcpy(tags + tl, tag, tlen); tl += tlen;

    // RFC 7845：BOS 页只能包含 OpusHead 这一个包，OpusTags 必须单独一页。
    // 两者同页时服务端 ffmpeg 直接拒收（v0.1.5 实测 422）。
    const uint8_t *sd0[1] = { head };
    int sl0[1] = { (int)sizeof(head) };
    int n0 = build_page(m, 0x02 /*BOS*/, 0, sd0, sl0, 1, out, out_cap);
    if (n0 <= 0) return -1;
    m->pageno++;

    const uint8_t *sd1[1] = { tags };
    int sl1[1] = { tl };
    int n1 = build_page(m, 0x00, 0, sd1, sl1, 1, out + n0, out_cap - (size_t)n0);
    if (n1 <= 0) return -1;
    m->pageno++;
    m->bos_done = true;
    return n0 + n1;   // 返回两页总长度，调用方一次追加即可
}

#define MAX_SEG 64

int ogg_mux_data_page(ogg_mux_t *m, const uint8_t *packet, int len,
                      bool eos, uint64_t samples, uint8_t *out, size_t out_cap) {
    if (len < 0) return -1;
    const uint8_t *seg_data[MAX_SEG];
    int seg_len[MAX_SEG];
    int nseg = lace_segments(packet, len, seg_data, seg_len, MAX_SEG);
        if (nseg < 0) return -1;

    m->granule += samples;  // 数据页 granulepos = 本页之前的累计采样
    uint8_t flags = eos ? 0x04 : 0x00;
    int n = build_page(m, flags, m->granule, seg_data, seg_len, nseg, out, out_cap);
    if (n > 0) m->pageno++;
    return n;
}
