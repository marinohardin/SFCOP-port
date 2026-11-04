// TEMPORARY: replaced by Wave 1 B2 to B7 (helpers the boot modules share)
/*
 * hle_util.c - small helpers the P1-2 HLE modules share. Upstreamable: yes.
 */
#include <stdio.h>
#include <string.h>
#include "sfcop_hle.h"

static const win32hle_shim *const g_tables[] = {
    win32hle_kernel32, win32hle_kernel32_ext, win32hle_kernel32_crt, win32hle_module,
    win32hle_gdidc, win32hle_gdi32, win32hle_user32, win32hle_winmm, win32hle_lz32,
    win32hle_wsock32, win32hle_ole32, win32hle_storage, win32hle_advapi32,
    win32hle_dsound, win32hle_ddraw, win32hle_bink, win32hle_mss,
};

recomp_func_t sfcop_hle_original(const char *name) {
    for (size_t t = 0; t < sizeof g_tables / sizeof g_tables[0]; t++)
        for (const win32hle_shim *s = g_tables[t]; s->name; s++)
            if (!strcmp(s->name, name)) return s->fn;
    return NULL;
}

int sfcop_guid_eq(uint32_t va, const uint8_t *guid) {
    return va && !memcmp(hle_ptr(va), guid, 16);
}

char *sfcop_wide_to_utf8(uint32_t wva, char *out, size_t n) {
    if (!wva) return NULL;
    size_t k = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t c = MEM16(wva + 2 * i);
        if (c >= 0xD800 && c < 0xDC00) {                 /* a surrogate pair */
            uint32_t lo = MEM16(wva + 2 * i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i++;
        }
        if (!c) break;
        uint8_t b[4];
        int m;
        if (c < 0x80) b[0] = (uint8_t)c, m = 1;
        else if (c < 0x800) b[0] = (uint8_t)(0xC0 | c >> 6), b[1] = (uint8_t)(0x80 | (c & 63)), m = 2;
        else if (c < 0x10000) b[0] = (uint8_t)(0xE0 | c >> 12), b[1] = (uint8_t)(0x80 | (c >> 6 & 63)), b[2] = (uint8_t)(0x80 | (c & 63)), m = 3;
        else b[0] = (uint8_t)(0xF0 | c >> 18), b[1] = (uint8_t)(0x80 | (c >> 12 & 63)), b[2] = (uint8_t)(0x80 | (c >> 6 & 63)), b[3] = (uint8_t)(0x80 | (c & 63)), m = 4;
        if (k + (size_t)m + 1 > n) break;
        memcpy(out + k, b, (size_t)m);
        k += (size_t)m;
    }
    out[k] = 0;
    return out;
}

uint32_t sfcop_utf8_to_wide(const char *s, uint32_t wva, uint32_t n) {
    uint32_t k = 0;
    if (!wva || !n) return 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && k + 1 < n;) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u), p += 3;
        else if (c >= 0xE0 && p[0] && p[1]) c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u), p += 2;
        else if (c >= 0xC0 && p[0]) c = (c & 31) << 6 | (p[0] & 63u), p += 1;
        if (c >= 0x10000) {
            if (k + 2 >= n) break;
            c -= 0x10000;
            MEM16(wva + 2 * k++) = (uint16_t)(0xD800 + (c >> 10));
            c = 0xDC00 + (c & 0x3FF);
        }
        MEM16(wva + 2 * k++) = (uint16_t)c;
    }
    MEM16(wva + 2 * k) = 0;
    return k;
}

void sfcop_todo_once(const char *name, const char *note) {
    static const char *seen[256];
    for (int i = 0; i < 256; i++) {
        if (seen[i] == name) return;
        if (!seen[i]) { seen[i] = name; break; }
    }
    fprintf(stderr, "[hle-todo] %s: %s\n", name, note);
}
