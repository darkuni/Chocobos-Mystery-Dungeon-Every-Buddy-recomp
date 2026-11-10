/* Crypto for unpacking fake PKGs at first start (src/boot_pkg.c): SHA-256, HMAC-SHA256, AES-128
 * (CBC and XTS decryption) and RSA-2048 PKCS#1 v1.5 decryption with the public fake keysets.
 * Small portable C: speed matters only for XTS, which runs once per game install. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[64];
    size_t used;
} BootSha256;
void boot_sha256_init(BootSha256 *s);
void boot_sha256_update(BootSha256 *s, const void *data, size_t size);
void boot_sha256_final(BootSha256 *s, uint8_t digest[32]);
void boot_sha256(const void *data, size_t size, uint8_t digest[32]);
void boot_hmac_sha256(const uint8_t *key, size_t key_size, const void *data, size_t size, uint8_t digest[32]);

/* Round keys for the AES-NI instructions (every CPU this port runs on has them: it needs AVX). */
typedef struct {
    uint8_t enc[11][16], dec[11][16];
} BootAes128;
void boot_aes128_init(BootAes128 *aes, const uint8_t key[16]);
void boot_aes128_encrypt_block(const BootAes128 *aes, const uint8_t in[16], uint8_t out[16]);
void boot_aes128_decrypt_block(const BootAes128 *aes, const uint8_t in[16], uint8_t out[16]);
/* In place; size is a multiple of 16. */
void boot_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t size);
/* One XTS data unit (size a multiple of 16) numbered `sector`, in place. */
void boot_aes128_xts_decrypt(const BootAes128 *data_key, const BootAes128 *tweak_key, uint64_t sector,
                             uint8_t *data, size_t size);

/* RSA-2048 with modulus n and private exponent d (both 256-byte big-endian): PKCS#1 v1.5 type 2
 * unpadding of c. Returns the message length (copied to out, at most out_size bytes), or -1 when
 * the padding is wrong (not a fake PKG). */
int boot_rsa2048_decrypt(const uint8_t n[256], const uint8_t d[256], const uint8_t c[256], uint8_t *out, size_t out_size);
