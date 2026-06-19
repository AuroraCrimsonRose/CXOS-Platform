/* /CXK/kernel/lib/sha256.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* SHA-256 per FIPS 180-4. Big-endian internally; careful byte handling. */

#include "sha256.h"

/* right-rotate a 32-bit word */
static inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

/* SHA-256 round constants (first 32 bits of the fractional parts of the cube
   roots of the first 64 primes). */
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

/* compress one 64-byte block into the running state */
static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    /* load 16 big-endian words */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]   << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] <<  8) |
               ((uint32_t)block[i*4+3]);
    }
    /* extend to 64 words */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],
             e=state[4],f=state[5],g=state[6],h=state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

void sha256_init(struct sha256_ctx *c) {
    /* initial hash values: fractional parts of the square roots of the first
       eight primes. */
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u;
    c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu;
    c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;
    while (len > 0) {
        uint32_t space = SHA256_BLOCK_SIZE - c->buflen;
        uint32_t take = (len < space) ? (uint32_t)len : space;
        for (uint32_t i = 0; i < take; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == SHA256_BLOCK_SIZE) {
            sha256_block(c->state, c->buf);
            c->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint64_t bitlen = c->bitlen;   /* captured before we append padding */

    /* append 0x80 then zeros until 56 bytes mod 64, then the 64-bit BE length */
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0x00;
    while (c->buflen != 56) sha256_update(c, &zero, 1);

    /* append message length as 64-bit big-endian, WITHOUT changing bitlen via
       sha256_update (write directly into the buffer + compress). */
    for (int i = 7; i >= 0; i--) {
        c->buf[c->buflen++] = (uint8_t)(bitlen >> (i * 8));
    }
    sha256_block(c->state, c->buf);
    c->buflen = 0;

    /* output the digest big-endian */
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >>  8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}