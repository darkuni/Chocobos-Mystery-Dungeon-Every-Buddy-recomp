/* src/boot_crypto.c against published test vectors: SHA-256 (FIPS 180-2 "abc"), HMAC-SHA256
 * (RFC 4231 case 2), AES-128 (FIPS-197 C.1), AES-128-CBC (NIST SP 800-38A F.2.2) and
 * XTS-AES-128 (IEEE 1619 vector 1). The RSA path and the whole PKG unpack were checked against
 * scripts/pkg_extract.py on the RESOGUN PKGs (docs/resogun.md). */
#include "../src/boot_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void hex(const char *s, uint8_t *out) {
    for (size_t i = 0; s[2 * i]; ++i) sscanf(s + 2 * i, "%2hhx", &out[i]);
}

int main(void) {
    uint8_t d[32], want[64];
    boot_sha256("abc", 3, d);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    assert(!memcmp(d, want, 32));

    boot_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    assert(!memcmp(d, want, 32));

    uint8_t key[16], block[16], out[16];
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("00112233445566778899aabbccddeeff", block);
    BootAes128 aes;
    boot_aes128_init(&aes, key);
    boot_aes128_encrypt_block(&aes, block, out);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
    assert(!memcmp(out, want, 16));
    boot_aes128_decrypt_block(&aes, out, out);
    assert(!memcmp(out, block, 16));

    uint8_t iv[16], data[64];
    hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    hex("000102030405060708090a0b0c0d0e0f", iv);
    hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7", data);
    boot_aes128_cbc_decrypt(key, iv, data, sizeof(data));
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", want);
    assert(!memcmp(data, want, 64));

    uint8_t zero[16] = {0}, xts[32];
    BootAes128 k1, k2;
    boot_aes128_init(&k1, zero);
    boot_aes128_init(&k2, zero);
    hex("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e", xts);
    boot_aes128_xts_decrypt(&k1, &k2, 0, xts, sizeof(xts));
    for (size_t i = 0; i < sizeof(xts); ++i) assert(!xts[i]);

    puts("boot crypto test passed");
    return 0;
}
