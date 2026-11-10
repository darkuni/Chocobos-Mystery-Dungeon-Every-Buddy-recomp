/* See boot_crypto.h. */
#include "boot_crypto.h"
#include <string.h>
#include <wmmintrin.h>
#include <emmintrin.h>

/* ---------------------------------------------------------------- SHA-256 (FIPS 180-4) */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = k + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
void boot_sha256_init(BootSha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->state, iv, sizeof(iv));
    s->length = 0;
    s->used = 0;
}
void boot_sha256_update(BootSha256 *s, const void *data, size_t size) {
    const uint8_t *p = data;
    s->length += size;
    while (size) {
        size_t n = 64 - s->used < size ? 64 - s->used : size;
        memcpy(s->buffer + s->used, p, n);
        s->used += n; p += n; size -= n;
        if (s->used == 64) { sha256_block(s->state, s->buffer); s->used = 0; }
    }
}
void boot_sha256_final(BootSha256 *s, uint8_t digest[32]) {
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80;
    boot_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) boot_sha256_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    boot_sha256_update(s, len, 8);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) digest[4 * i + j] = (uint8_t)(s->state[i] >> (24 - 8 * j));
}
void boot_sha256(const void *data, size_t size, uint8_t digest[32]) {
    BootSha256 s;
    boot_sha256_init(&s);
    boot_sha256_update(&s, data, size);
    boot_sha256_final(&s, digest);
}
void boot_hmac_sha256(const uint8_t *key, size_t key_size, const void *data, size_t size, uint8_t digest[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    if (key_size > 64) boot_sha256(key, key_size, k);
    else memcpy(k, key, key_size);
    BootSha256 s;
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    boot_sha256_init(&s); boot_sha256_update(&s, pad, 64); boot_sha256_update(&s, data, size); boot_sha256_final(&s, inner);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    boot_sha256_init(&s); boot_sha256_update(&s, pad, 64); boot_sha256_update(&s, inner, 32); boot_sha256_final(&s, digest);
}

/* ---------------------------------------------------------------- AES-128 (AES-NI) */
#define AES_TARGET __attribute__((target("aes,sse2")))
AES_TARGET static __m128i expand_step(__m128i key, __m128i assist) {
    assist = _mm_shuffle_epi32(assist, 0xff);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, assist);
}
AES_TARGET void boot_aes128_init(BootAes128 *aes, const uint8_t key[16]) {
    __m128i k[11];
    k[0] = _mm_loadu_si128((const __m128i *)key);
    k[1] = expand_step(k[0], _mm_aeskeygenassist_si128(k[0], 0x01));
    k[2] = expand_step(k[1], _mm_aeskeygenassist_si128(k[1], 0x02));
    k[3] = expand_step(k[2], _mm_aeskeygenassist_si128(k[2], 0x04));
    k[4] = expand_step(k[3], _mm_aeskeygenassist_si128(k[3], 0x08));
    k[5] = expand_step(k[4], _mm_aeskeygenassist_si128(k[4], 0x10));
    k[6] = expand_step(k[5], _mm_aeskeygenassist_si128(k[5], 0x20));
    k[7] = expand_step(k[6], _mm_aeskeygenassist_si128(k[6], 0x40));
    k[8] = expand_step(k[7], _mm_aeskeygenassist_si128(k[7], 0x80));
    k[9] = expand_step(k[8], _mm_aeskeygenassist_si128(k[8], 0x1b));
    k[10] = expand_step(k[9], _mm_aeskeygenassist_si128(k[9], 0x36));
    for (int i = 0; i < 11; ++i) _mm_storeu_si128((__m128i *)aes->enc[i], k[i]);
    _mm_storeu_si128((__m128i *)aes->dec[0], k[10]);
    for (int i = 1; i < 10; ++i) _mm_storeu_si128((__m128i *)aes->dec[i], _mm_aesimc_si128(k[10 - i]));
    _mm_storeu_si128((__m128i *)aes->dec[10], k[0]);
}
AES_TARGET static __m128i encrypt(const BootAes128 *aes, __m128i b) {
    b = _mm_xor_si128(b, _mm_loadu_si128((const __m128i *)aes->enc[0]));
    for (int i = 1; i < 10; ++i) b = _mm_aesenc_si128(b, _mm_loadu_si128((const __m128i *)aes->enc[i]));
    return _mm_aesenclast_si128(b, _mm_loadu_si128((const __m128i *)aes->enc[10]));
}
AES_TARGET static __m128i decrypt(const BootAes128 *aes, __m128i b) {
    b = _mm_xor_si128(b, _mm_loadu_si128((const __m128i *)aes->dec[0]));
    for (int i = 1; i < 10; ++i) b = _mm_aesdec_si128(b, _mm_loadu_si128((const __m128i *)aes->dec[i]));
    return _mm_aesdeclast_si128(b, _mm_loadu_si128((const __m128i *)aes->dec[10]));
}
AES_TARGET void boot_aes128_encrypt_block(const BootAes128 *aes, const uint8_t in[16], uint8_t out[16]) {
    _mm_storeu_si128((__m128i *)out, encrypt(aes, _mm_loadu_si128((const __m128i *)in)));
}
AES_TARGET void boot_aes128_decrypt_block(const BootAes128 *aes, const uint8_t in[16], uint8_t out[16]) {
    _mm_storeu_si128((__m128i *)out, decrypt(aes, _mm_loadu_si128((const __m128i *)in)));
}
AES_TARGET void boot_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t size) {
    BootAes128 aes;
    boot_aes128_init(&aes, key);
    __m128i chain = _mm_loadu_si128((const __m128i *)iv);
    for (size_t i = 0; i + 16 <= size; i += 16) {
        __m128i c = _mm_loadu_si128((const __m128i *)(data + i));
        _mm_storeu_si128((__m128i *)(data + i), _mm_xor_si128(decrypt(&aes, c), chain));
        chain = c;
    }
}
/* Multiply the tweak by x in GF(2^128), little-endian byte order (IEEE 1619). */
static void xts_next(uint8_t t[16]) {
    unsigned carry = t[15] >> 7;
    for (int i = 15; i > 0; --i) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
    t[0] = (uint8_t)(t[0] << 1 ^ (carry ? 0x87 : 0));
}
AES_TARGET void boot_aes128_xts_decrypt(const BootAes128 *data_key, const BootAes128 *tweak_key, uint64_t sector,
                                        uint8_t *data, size_t size) {
    uint8_t t[16] = {0};
    for (int i = 0; i < 8; ++i) t[i] = (uint8_t)(sector >> (8 * i));
    boot_aes128_encrypt_block(tweak_key, t, t);
    for (size_t i = 0; i + 16 <= size; i += 16) {
        __m128i tw = _mm_loadu_si128((const __m128i *)t);
        __m128i c = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(data + i)), tw);
        _mm_storeu_si128((__m128i *)(data + i), _mm_xor_si128(decrypt(data_key, c), tw));
        xts_next(t);
    }
}

/* ---------------------------------------------------------------- RSA-2048 (Montgomery) */
enum { LIMBS = 64 }; /* 32-bit limbs, least significant first */
typedef struct { uint32_t v[LIMBS]; } Num;
static void num_from_be(Num *x, const uint8_t b[256]) {
    for (int i = 0; i < LIMBS; ++i) {
        const uint8_t *p = b + 256 - 4 * (i + 1);
        x->v[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}
static void num_to_be(const Num *x, uint8_t b[256]) {
    for (int i = 0; i < LIMBS; ++i) {
        uint8_t *p = b + 256 - 4 * (i + 1);
        p[0] = (uint8_t)(x->v[i] >> 24); p[1] = (uint8_t)(x->v[i] >> 16); p[2] = (uint8_t)(x->v[i] >> 8); p[3] = (uint8_t)x->v[i];
    }
}
static int num_cmp(const Num *a, const Num *b) {
    for (int i = LIMBS - 1; i >= 0; --i)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}
static uint32_t num_sub(Num *a, const Num *b) { /* a -= b; borrow out */
    uint64_t borrow = 0;
    for (int i = 0; i < LIMBS; ++i) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        a->v[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
    return (uint32_t)borrow;
}
/* r = a*b*R^-1 mod n (CIOS); n0 = -n^-1 mod 2^32. */
static void mont_mul(Num *r, const Num *a, const Num *b, const Num *n, uint32_t n0) {
    uint32_t t[LIMBS + 2] = {0};
    for (int i = 0; i < LIMBS; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < LIMBS; ++j) {
            uint64_t s = (uint64_t)a->v[j] * b->v[i] + t[j] + carry;
            t[j] = (uint32_t)s; carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[LIMBS] + carry;
        t[LIMBS] = (uint32_t)s; t[LIMBS + 1] = (uint32_t)(s >> 32);
        uint32_t m = t[0] * n0;
        s = (uint64_t)m * n->v[0] + t[0];
        carry = s >> 32;
        for (int j = 1; j < LIMBS; ++j) {
            s = (uint64_t)m * n->v[j] + t[j] + carry;
            t[j - 1] = (uint32_t)s; carry = s >> 32;
        }
        s = (uint64_t)t[LIMBS] + carry;
        t[LIMBS - 1] = (uint32_t)s;
        t[LIMBS] = t[LIMBS + 1] + (uint32_t)(s >> 32);
    }
    memcpy(r->v, t, sizeof(r->v));
    if (t[LIMBS] || num_cmp(r, n) >= 0) num_sub(r, n);
}
int boot_rsa2048_decrypt(const uint8_t n_be[256], const uint8_t d_be[256], const uint8_t c_be[256], uint8_t *out, size_t out_size) {
    Num n, c, x, r2, one = {{1}};
    num_from_be(&n, n_be);
    num_from_be(&c, c_be);
    if (!(n.v[0] & 1) || num_cmp(&c, &n) >= 0) return -1;
    uint32_t inv = 1; /* n^-1 mod 2^32 by Newton iteration */
    for (int i = 0; i < 5; ++i) inv *= 2 - n.v[0] * inv;
    const uint32_t n0 = 0u - inv;
    /* R^2 mod n by doubling 1 4096 times. */
    memset(&r2, 0, sizeof(r2));
    r2.v[0] = 1;
    for (int i = 0; i < 2 * 32 * LIMBS; ++i) {
        uint32_t top = r2.v[LIMBS - 1] >> 31;
        for (int j = LIMBS - 1; j > 0; --j) r2.v[j] = r2.v[j] << 1 | r2.v[j - 1] >> 31;
        r2.v[0] <<= 1;
        if (top || num_cmp(&r2, &n) >= 0) num_sub(&r2, &n);
    }
    Num cm, acc;
    mont_mul(&cm, &c, &r2, &n, n0);      /* c in Montgomery form */
    mont_mul(&acc, &one, &r2, &n, n0);   /* 1 in Montgomery form */
    for (int byte = 0; byte < 256; ++byte)
        for (int bit = 7; bit >= 0; --bit) {
            mont_mul(&acc, &acc, &acc, &n, n0);
            if (d_be[byte] >> bit & 1) mont_mul(&acc, &acc, &cm, &n, n0);
        }
    mont_mul(&x, &acc, &one, &n, n0);    /* out of Montgomery form */
    uint8_t m[256];
    num_to_be(&x, m);
    if (m[0] != 0 || m[1] != 2) return -1;
    size_t i = 2;
    while (i < 256 && m[i]) ++i;
    if (i == 256) return -1;
    ++i;
    size_t len = 256 - i;
    if (len > out_size) len = out_size;
    memcpy(out, m + i, len);
    return (int)len;
}
