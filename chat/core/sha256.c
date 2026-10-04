#include "chat/core/sha256.h"

#include <stdint.h>
#include <string.h>

/* Straightforward FIPS 180-4 SHA-256. Hand-rolled because the project has no
   third-party dependencies and Windows offers no hash API. */

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    unsigned char block[64];
    size_t block_fill;
} Sha256;

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(Sha256 *h) {
    static const uint32_t init[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(h->state, init, sizeof init);
    h->total_bits = 0;
    h->block_fill = 0;
}

static void sha256_compress(Sha256 *h, const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h->state[0]; b = h->state[1]; c = h->state[2]; d = h->state[3];
    e = h->state[4]; f = h->state[5]; g = h->state[6]; hh = h->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h->state[0] += a; h->state[1] += b; h->state[2] += c; h->state[3] += d;
    h->state[4] += e; h->state[5] += f; h->state[6] += g; h->state[7] += hh;
}

static void sha256_update(Sha256 *h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    h->total_bits += (uint64_t)n * 8u;
    while (n) {
        size_t take = 64 - h->block_fill;
        if (take > n) take = n;
        memcpy(h->block + h->block_fill, p, take);
        h->block_fill += take;
        p += take;
        n -= take;
        if (h->block_fill == 64) {
            sha256_compress(h, h->block);
            h->block_fill = 0;
        }
    }
}

static void sha256_final(Sha256 *h, unsigned char out[32]) {
    uint64_t bits = h->total_bits;
    unsigned char pad = 0x80, zero = 0;
    int i;
    sha256_update(h, &pad, 1);
    h->total_bits = bits; /* the length field counts only the message */
    while (h->block_fill != 56) {
        sha256_update(h, &zero, 1);
        h->total_bits = bits;
    }
    {
        unsigned char len[8];
        for (i = 0; i < 8; i++)
            len[7 - i] = (unsigned char)(bits >> (8 * (unsigned)i));
        sha256_update(h, len, 8);
    }
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(h->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h->state[i];
    }
}

void sha256_hex(const void *data, size_t n, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    Sha256 h;
    unsigned char raw[32];
    int i;
    sha256_init(&h);
    if (n) sha256_update(&h, data, n);
    sha256_final(&h, raw);
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[64] = 0;
}