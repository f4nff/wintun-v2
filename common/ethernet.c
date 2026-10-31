/* SPDX-License-Identifier: GPL-2.0 */
#include "ethernet.h"
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void copy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static int equal(const uint8_t *a, const uint8_t *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static int zero(const uint8_t *p, uint32_t n) { while (n--) if (*p++) return 0; return 1; }
static uint32_t sum(const uint8_t *p, uint32_t n, uint32_t s) {
    while (n > 1) { s += be16(p); p += 2; n -= 2; } if (n) s += (uint32_t)*p << 8; return s;
}
static uint16_t finish(uint32_t s) { while (s >> 16) s = (s & 65535) + (s >> 16); return (uint16_t)~s; }
static uint32_t header(const uint8_t *f, uint32_t n, uint16_t *type) {
    if (!f || n < PB_ETH_HEADER) return 0;
    *type = be16(f + 12);
    if (*type == 0x8100) { if (n < 18) return 0; *type = be16(f + 16); return 18; }
    return 14;
}
PB_ETH_RESULT PbEthernetInspect(const uint8_t *f, uint32_t n, uint32_t *offset) {
    uint16_t type; uint32_t h = header(f, n, &type);
    if (!offset) return PB_ETH_DROP;
    *offset = 0;
    if (h && n > h && ((type == 0x0800 && f[h] >> 4 == 4) || (type == 0x86dd && f[h] >> 4 == 6))) {
        *offset = h; return PB_ETH_IP;
    }
    return PB_ETH_DROP;
}
PB_ETH_RESULT PbEthernetControl(const uint8_t *f, uint32_t n, const uint8_t peer[6], uint8_t out[PB_ETH_CONTROL_MAX], uint32_t *written) {
    uint16_t type; uint32_t h = header(f, n, &type);
    if (!written) return PB_ETH_DROP;
    *written = 0;
    if (!h || !peer || !out || (f[6] & 1)) return PB_ETH_DROP;
    if (type == 0x0806) {
        const uint8_t *a = f + h;
        if (n - h < 28 || be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || be16(a + 6) != 1 ||
            !equal(a + 8, f + 6, 6) || zero(a + 14, 4) || equal(a + 14, a + 24, 4) || a[24] == 0 || a[24] >= 224) return PB_ETH_DROP;
        for (uint32_t i = 0; i < 60; ++i) out[i] = 0;
        copy(out, f + 6, 6); copy(out + 6, peer, 6); put16(out + 12, 0x0806);
        uint8_t *r = out + 14;
        copy(r, a, 6); put16(r + 6, 2); copy(r + 8, peer, 6); copy(r + 14, a + 24, 4);
        copy(r + 18, a + 8, 6); copy(r + 24, a + 14, 4);
        *written = 60; return PB_ETH_REPLY;
    }
    if (type == 0x86dd && n - h >= 64) {
        const uint8_t *ip = f + h, *ns = ip + 40;
        uint32_t payload = be16(ip + 4);
        if (ip[0] >> 4 != 6 || ip[6] != 58 || ip[7] != 255 || payload < 24 || payload > n - h - 40 ||
            ns[0] != 135 || ns[1] || !zero(ns + 4, 4) || zero(ip + 8, 16) || ip[8] == 0xff ||
            zero(ns + 8, 16) || ns[8] == 0xff || equal(ns + 8, ip + 8, 16) ||
            finish(sum(ns, payload, sum(ip + 8, 32, payload + 58))) != 0) return PB_ETH_DROP;
        /* Reject malformed options, and a source link-layer option contradicting Ethernet. */
        for (uint32_t p = 24; p < payload;) {
            if (payload - p < 2 || !ns[p + 1] || (uint32_t)ns[p + 1] * 8 > payload - p) return PB_ETH_DROP;
            if (ns[p] == 1 && (ns[p + 1] != 1 || !equal(ns + p + 2, f + 6, 6))) return PB_ETH_DROP;
            p += (uint32_t)ns[p + 1] * 8;
        }
        for (uint32_t i = 0; i < 86; ++i) out[i] = 0;
        copy(out, f + 6, 6); copy(out + 6, peer, 6); put16(out + 12, 0x86dd);
        uint8_t *r = out + 14, *na = r + 40;
        r[0] = 0x60; put16(r + 4, 32); r[6] = 58; r[7] = 255;
        copy(r + 8, ns + 8, 16); copy(r + 24, ip + 8, 16);
        na[0] = 136; na[4] = 0x60; copy(na + 8, ns + 8, 16); na[24] = 2; na[25] = 1; copy(na + 26, peer, 6);
        put16(na + 2, finish(sum(na, 32, sum(r + 8, 32, 32 + 58))));
        *written = 86; return PB_ETH_REPLY;
    }
    return PB_ETH_DROP;
}
void PbEthernetHeader(uint8_t out[PB_ETH_HEADER], const uint8_t local[6], const uint8_t peer[6], const uint8_t *ip, uint32_t n) {
    copy(out, local, 6); copy(out + 6, peer, 6);
    int v6 = n && ip[0] >> 4 == 6;
    put16(out + 12, v6 ? 0x86dd : 0x0800);
    if (v6 && n >= 40 && ip[24] == 0xff) { out[0] = out[1] = 0x33; copy(out + 2, ip + 36, 4); }
    else if (!v6 && n >= 20 && (ip[16] & 0xf0) == 0xe0) {
        out[0] = 1; out[1] = 0; out[2] = 0x5e; out[3] = ip[17] & 0x7f; copy(out + 4, ip + 18, 2);
    } else if (!v6 && n >= 20 && ip[16] == 255 && ip[17] == 255 && ip[18] == 255 && ip[19] == 255) {
        for (uint32_t i = 0; i < 6; ++i) out[i] = 255;
    }
}
