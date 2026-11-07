// main/ogg_demux.c —— 见 ogg_demux.h。Ogg Opus 下行解封装（RFC 7845）。
//
// 设计取舍：设备端下行整段收完（≤ ~70KB）再解码，故采用一次性解析（非流式状态机），
// 但正确支持 Ogg lacing —— 一个包可由多片（255 字节）组成，并可在页边界处跨页续接。
#include "ogg_demux.h"

#include <stdlib.h>
#include <string.h>

// 单个 Opus 数据包重组缓冲上限。上行封装单页 ≤ 4KB，故单个包不会超过此值。
#define REASM_CAP 8192

ogg_demux_ret_t ogg_demux_parse(const uint8_t *buf, int len, void *user,
                                void (*on_head)(void *user, uint32_t rate, int channels, int preskip),
                                void (*on_packet)(void *user, const uint8_t *data, int plen)) {
    if (len < 0) return OGG_DEMUX_ERR;

    // 8KB 重组缓冲放堆上：它原本是栈数组，单这一项就等于整个 app 任务栈额度，
    // 播放回答时必然触发 Stack protection fault（v0.1.2 实测）。
    uint8_t *reasm = (uint8_t *)malloc(REASM_CAP);
    if (!reasm) return OGG_DEMUX_ERR;
    ogg_demux_ret_t ret = OGG_DEMUX_OK;

    int reasm_len = 0;
    int pkt_index = 0;   // 0 = OpusHead, 1 = OpusTags, ≥2 = 数据包含
    int pos = 0;

    while (pos + 27 <= len) {
        // 对齐到页起点；若此处不是 OggS，向前找下一个。
        if (memcmp(buf + pos, "OggS", 4) != 0) {
            int next = pos + 1;
            while (next + 4 <= len && memcmp(buf + next, "OggS", 4) != 0) next++;
            if (next + 27 > len) break;
            pos = next;
        }

        int nseg = buf[pos + 26];
        int segtab = 27;
        if (pos + segtab + nseg > len) break;            // 页被截断

        int data_len = 0;
        for (int i = 0; i < nseg; i++) data_len += buf[pos + segtab + i];
        int data_off = pos + segtab + nseg;
        if (data_off + data_len > len) break;            // 数据被截断

        int seg_off = data_off;
        for (int i = 0; i < nseg; i++) {
            int sl = buf[pos + segtab + i];
            if (reasm_len + sl > REASM_CAP) { ret = OGG_DEMUX_ERR; goto done; }
            if (sl) memcpy(reasm + reasm_len, buf + seg_off, sl);
            reasm_len += sl;
            seg_off += sl;

            if (sl < 255) {                              // 片 < 255 → 本包结束
                if (pkt_index == 0) {
                    // OpusHead：解析 rate / channels / preskip。
                    if (reasm_len >= 19 && memcmp(reasm, "OpusHead", 8) == 0) {
                        uint32_t rate = (uint32_t)reasm[12] | ((uint32_t)reasm[13] << 8) |
                                        ((uint32_t)reasm[14] << 16) | ((uint32_t)reasm[15] << 24);
                        int channels = reasm[9];
                        int preskip = (int)reasm[10] | ((int)reasm[11] << 8);
                        if (on_head) on_head(user, rate, channels, preskip);
                    }
                } else if (pkt_index >= 2) {
                    if (on_packet) on_packet(user, reasm, reasm_len);
                }
                // pkt_index == 1（OpusTags）忽略
                pkt_index++;
                reasm_len = 0;
            }
            // sl == 255：包跨片/跨页续接，继续累积，不结束。
        }

        pos = data_off + data_len;                       // 跳到下一页
    }

    (void)reasm_len;  // 末尾若仍有未结束的跨页包，说明流截断，忽略。

done:
    free(reasm);
    return ret;
}
