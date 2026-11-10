/* sceKernelLoadStartModule / sceKernelDlsym for modules linked at prepare time.
 *
 * Unity opens its native plugins (the PRXs in Media/Plugins: CRIWARE, SaveData, NpToolkit, ...) at run
 * time and looks their functions up by C name. Here every such plugin is already linked into the
 * image (game profile) and started before the game, in link order, like the PS4 dynamic linker
 * does for its dependencies. Loading only hands out a handle; dlsym hashes the name to its NID
 * (SHA-1 of name + the SDK suffix, first 8 bytes reversed, base64 with '+' and '-') and looks it up
 * in that module's export table, read once from its PRX in the game folder.
 *
 * Kept out of src/runtime*.c because it needs the PRX parser of boot_prepare.c, which the runtime
 * unit tests do not link. */
#include "dynlib.h"
#include "boot_prepare.h"
#include "runtime.h"
#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODULES 16
#define HANDLE_BASE 0x1000 /* real module handles are small integers; keep ours recognisable */

typedef struct { char nid[12]; uint64_t value; } Export;
typedef struct {
    char file[256];          /* as linked: "libc.prx", "Media/Plugins/cri_ware_unity.prx" */
    uintptr_t base;          /* host address of the module's image */
    Export *exports; size_t nexports, cap;
    int exports_loaded;
} Dynmod;

static Dynmod mods[MAX_MODULES];
static size_t nmods;
static char game_dir[4096];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- SHA-1 (FIPS 180-4), only for NIDs ---- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } Sha1;
static uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h0, sizeof(h0)); s->len = 0; s->n = 0;
}
static void sha1_update(Sha1 *s, const void *data, size_t size) {
    const uint8_t *p = data;
    s->len += size;
    while (size) {
        size_t take = 64 - s->n < size ? 64 - s->n : size;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; size -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}
static void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha1_update(s, &pad, 1);
    while (s->n != 56) sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; ++i) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}

void dynlib_nid(const char *name, char out[12]) {
    static const uint8_t suffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    Sha1 s;
    uint8_t h[20], b[9] = {0};
    sha1_init(&s); sha1_update(&s, name, strlen(name)); sha1_update(&s, suffix, sizeof(suffix)); sha1_final(&s, h);
    for (int i = 0; i < 8; ++i) b[i] = h[7 - i];
    /* 8 bytes = 64 bits -> 11 base64 digits (the last one carries 4 bits). */
    int o = 0;
    for (int i = 0; i < 9 && o < 11; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (uint32_t)b[i + 1] << 8 | b[i + 2];
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        if (o < 11) out[o++] = alphabet[v >> 6 & 63];
        if (o < 11) out[o++] = alphabet[v & 63];
    }
    out[11] = 0;
}

/* ---- module table ---- */
int dynlib_init(const char *boot_path, const char *app0, unsigned char *image, const uint64_t *bases, size_t count) {
    char path[4200];
    const char *slash = strrchr(boot_path, '/');
#ifdef _WIN32
    const char *back = strrchr(boot_path, '\\');
    if (!slash || (back && back > slash)) slash = back;
#endif
    int dir_len = slash ? (int)(slash - boot_path) : 1;
    snprintf(path, sizeof(path), "%.*s/modules.txt", dir_len, slash ? boot_path : ".");
    snprintf(game_dir, sizeof(game_dir), "%s", app0 ? app0 : ".");
    FILE *f = fopen(path, "rb");
    if (!f) { printf("Dynlib: no %s; plugins cannot be loaded\n", path); return -1; }
    char line[256];
    nmods = 0;
    while (nmods < count && nmods < MAX_MODULES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        Dynmod *m = &mods[nmods];
        memcpy(m->file, line, strlen(line) + 1);
        m->base = (uintptr_t)image + bases[nmods];
        ++nmods;
    }
    fclose(f);
    printf("Dynlib: %zu linked modules available to sceKernelLoadStartModule\n", nmods);
    return 0;
}

/* "cri_ware_unity", "/app0/Media/Plugins/cri_ware_unity.prx", "libSceNpToolkit2.sprx" -> lower-case
 * base name without extension. */
static void stem(const char *path, char *out, size_t size) {
    const char *b = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    size_t n = 0;
    for (; b[n] && n + 1 < size; ++n) out[n] = (char)tolower((unsigned char)b[n]);
    out[n] = 0;
    char *dot = strrchr(out, '.');
    if (dot && (!strcmp(dot, ".prx") || !strcmp(dot, ".sprx") || !strcmp(dot, ".elf") || !strcmp(dot, ".self"))) *dot = 0;
}

static int find_module(const char *path) {
    char want[256], have[256];
    stem(path, want, sizeof(want));
    for (size_t i = 0; i < nmods; ++i) {
        stem(mods[i].file, have, sizeof(have));
        if (!strcmp(want, have)) return (int)i;
    }
    return -1;
}

int32_t dynlib_load(const char *path) {
    if (!path) return -1;
    int i = find_module(path);
    if (i < 0) { printf("Dynlib: sceKernelLoadStartModule(%s): not linked\n", path); return -1; }
    printf("Dynlib: sceKernelLoadStartModule(%s) -> %s (already started)\n", path, mods[i].file);
    return HANDLE_BASE + i;
}

static void add_export(void *ctx, const char *nid, uint64_t value) {
    Dynmod *m = ctx;
    if (m->nexports == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        Export *e = realloc(m->exports, cap * sizeof(Export));
        if (!e) return;
        m->exports = e; m->cap = cap;
    }
    Export *e = &m->exports[m->nexports++];
    snprintf(e->nid, sizeof(e->nid), "%s", nid);
    e->value = value;
}

static int load_exports(Dynmod *m) {
    if (m->exports_loaded) return 0;
    char path[4500];
    snprintf(path, sizeof(path), "%s/%s%s", game_dir, strchr(m->file, '/') ? "" : "sce_module/", m->file);
    if (boot_module_exports(path, add_export, m)) { printf("Dynlib: cannot read exports of %s\n", path); return -1; }
    m->exports_loaded = 1;
    printf("Dynlib: %s exports %zu symbols\n", m->file, m->nexports);
    return 0;
}

uintptr_t dynlib_sym(int32_t handle, const char *name) {
    if (!name || handle < HANDLE_BASE || (size_t)(handle - HANDLE_BASE) >= nmods) return 0;
    Dynmod *m = &mods[handle - HANDLE_BASE];
    char nid[12];
    dynlib_nid(name, nid);
    uintptr_t found = 0;
    pthread_mutex_lock(&lock);
    if (!load_exports(m))
        for (size_t i = 0; i < m->nexports; ++i)
            if (!strcmp(m->exports[i].nid, nid)) { found = m->base + m->exports[i].value; break; }
    pthread_mutex_unlock(&lock);
    if (!found) printf("Dynlib: sceKernelDlsym(%s, %s [%s]): not exported\n", m->file, name, nid);
    return found;
}
