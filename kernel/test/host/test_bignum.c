// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_bignum.c */
/* Aurora Tejeda / CATX Systems */
/*
 * lib/crypto/bignum.c: the arithmetic under every signature check.
 *
 * Two oracles, neither of them this code. The exponentiations are checked
 * against values node's BigInt computed (2.8a'4b), so a mistake cannot agree
 * with itself. The remainder is checked against a bit-serial one written here
 * (the algorithm the kernel used before 2.8a'4b): over random and structured
 * numbers, where Algorithm D's corrections live (words of all ones, a lone top
 * bit), and over the one case known to need its add-back step (Hacker's
 * Delight's divmnu test, widened to 32-bit words).
 */
#include "khost.h"
#include "lib/crypto/bignum.h"

#include <string.h>

static void from_hex(bignum *a, const char *hex) {
    uint8_t b[512];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        b[i] = (uint8_t)v;
    }
    bn_from_bytes(a, b, n);
}

static void set_used(bignum *a) {
    a->used = 0;
    for (int i = 0; i < BN_MAXLIMBS; i++) if (a->limb[i]) a->used = i + 1;
}

/* The oracle: shift a in a bit at a time, subtract m whenever it fits. */
static void slow_mod(bignum *r, const bignum *a, const bignum *m) {
    uint32_t rem[BN_MAXLIMBS + 1];
    memset(rem, 0, sizeof rem);
    for (int bit = BN_MAXLIMBS * 32 - 1; bit >= 0; bit--) {
        for (int i = BN_MAXLIMBS; i > 0; i--) rem[i] = (rem[i] << 1) | (rem[i - 1] >> 31);
        rem[0] = (rem[0] << 1) | ((a->limb[bit >> 5] >> (bit & 31)) & 1u);
        int ge = 1;
        for (int i = BN_MAXLIMBS; i >= 0; i--) {
            uint32_t mi = i < BN_MAXLIMBS ? m->limb[i] : 0;
            if (rem[i] != mi) { ge = rem[i] > mi; break; }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i <= BN_MAXLIMBS; i++) {
                uint32_t mi = i < BN_MAXLIMBS ? m->limb[i] : 0;
                uint64_t t = (uint64_t)rem[i] - mi - borrow;
                rem[i] = (uint32_t)t;
                borrow = t >> 63;
            }
        }
    }
    bn_zero(r);
    memcpy(r->limb, rem, sizeof r->limb);
    set_used(r);
}

static int same(const bignum *a, const bignum *b) {
    return a->used == b->used && memcmp(a->limb, b->limb, sizeof a->limb) == 0;
}

static uint64_t rng = 0x2b4b4b4bu;
static uint32_t next32(void) {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng >> 32);
}

/* A word likely to stress a quotient guess: all ones, a lone top bit, zero,
   one, or anything. */
static uint32_t word(void) {
    switch (next32() % 6) {
    case 0:  return 0xFFFFFFFFu;
    case 1:  return 0x80000000u;
    case 2:  return 0;
    case 3:  return 1;
    default: return next32();
    }
}

static void fill(bignum *a, int words) {
    bn_zero(a);
    for (int i = 0; i < words; i++) a->limb[i] = word();
    set_used(a);
}

KH_TEST(bignum_modexp_matches_independent_values) {
    static const char *v[][3] = {
        {   /* a 2048-bit modulus, top bit set */
            "9bb3d65d9d7bb34b00ac65b020dc35072fec1603b52dfe6e40428554c7b2c7508b01cf5646cad526ab6199160de035aedb0404f1351f28a9b2b50133af5239a145339b58a10dbc1768f9fd629c818f1d8070e4d128ade759eb0e0e49fa8768d6d52f7389ff67c4d4d75efa881df6de2efd426e5b4b1d6f6308476ebad5598c2d2de174e5f429304a0ce4937265df632fdb4fe16f6ff69a05e52e3b8e4fc3ae1f4888dbc2c2e3b560e57a4b7e6c8fa819cbe9bcd9771cc63c63a136ba2fc9f84d6817115bc328354a84eb666347c294a161e7857c137ecfa11d5dbaed93de67a0be37f15488a3855ff48a4516afa4985aec22ee59c28cd8e23649a05edf2a10f9",
            "694dfe6a366449a6007fd573b6bd557cfc279870ff8fb41fa66254edd7768518af66f6d8f4e02b882af1e5b0342c8104b3316e754c9f87c2b64a755f2c03509528825a235d04505ab0e6afec72997f35206855c74485ea9618446c8346b70b1adda8fb41c98cd3b892761d5e7cd39db7fb238b435bb5164d664efc78b11ae730e51fab5f957a31d6c25713eda14baee2410687bafa2afb9e1dbd1d80add43a4a7ce462c24852fe41c1a59c42b54033e3bdc1b5409134f7b800524d03516202ef89ba0e6bbba4fbc9a5fad19989c435d2ae8f93929458a6b6e4943eaddb6e0929ec00529250bbaa71b05657dc69ef67eeb9302747d708d343fb2fe0d648cad38f",
            "380e891ad0fac3c59b2408eb50325d1b34aefdb35fc567f8ce6948f6f611e323ba6868c06fd09c422918410c5c5a67ac71e2d0fdce64bbc4fb832e4d66604d583916aa44bd250a5007acc9c9ce613a149adc502018de7a82de597d2bbbd002ac50f42697b5f8b4ab8a55098a407ee77f75078bcfb3fede1bf7cf19ce489dd8d0e0a3f4a8265587b035ee0ed19730fd99f454767d593a9583a94577a4f7c1848838db74403e92f2c09236ccec5ae7f9b219cd69bf0fb55171e39d106e100516101c87e3f3a42c5cb7fd07a3bbf30d4fca2ce1ebe2f0315d9de7efd3b15f19678e900b0244ed19c3b16df5bc163ff62120f25edf872a4d6c2bf9776fcd6e57c60d",
        },
        {   /* a modulus whose top word is 1: the largest normalising shift */
            "0000000110b4a8b0ea4787cdb8dd2f8409cdb48dc543cfad992ecc9a5f2a519883ef0c7a0e0cd56580379c553e20b8433f23d3a3fce399030018894be2bdca16034b690ae6a4f82e584aef3962974e053459e257ba89010eca658227f56f52f3d2d8873cfc62d6244642beb958c0cf3f1dc8b32fdf02c0317eed2c809388d5a36501f77adc6d011c77f808f821d92db41c7e20149960bc8fbee2a3b96ccd69d70f21d0ccb01d5ff071efc5205ace9d09e484d38f5c2c07fbdbcfefcbffd09e6512e5bd292cd9666def462599b4f74167de283105203e128b9cbd422d1a5d1e84ffd7075ee1d833ecbf7e99d9494fea4532a0c179594e53df20dbb46189c9ffb1",
            "000000006a34b5c37c6f57c45c05f998e16ca7f79271efbbc4863974facf7a732c1ceaf05996f6498fd7caf827609e774e94d4b5d68b2c169a607c8168e10b31ebd042409f06c184f6aec6e342b43452295f7c2addd9c4de060c1e523d5c76e97730596df488a3bc501280d669d9b3e2f643e6b7ddeade7155dd2e0b0f4704f3decad79224630c3aac927ac869444e6555cd7008eeaafa8c8b8727463102375fe7c85d049d1c5c47f7cf099a1f4b0c55f172baa6e4ff6c041e8f2d0fc9bd836f6cc39770d860309b4944e99e67e1848cec8fc4913d4ae94a6f58a10ce3aa64d99d730466ea6309394b313a4f0f455c4ae7e95b288aee2d4c12459af1de4df0eb",
            "00000000ba48ad382f8105e957c1a325962af29aa7966394a8f24006935fb5f2306fcd7e615d258a23fe45537a84a27c014331191cb4ebc018d96b55ce81d3b8a88f5e7321f6cac09fb6be14be7c7dc5cb649b5d0eb1772147a5442c189e17d09fbeacbfe75f0c9644000bfd96f649934896a289e8235224795ec7e0c89cef3f880036e96bb3909cbef6a6c71f825082ab015ea835f0553697d0dd302544a442fdba2c904aee4f63581cced0d027755d88f790d643426ab2efa52537ae865a36333d34ed95bd0ae5514847c2e1e59cd560b2c465eeeee3d9e7c6c50fcae822777c72e68a02b9dfb1f2d14b7ff6b31569f34a4e5cd17984493b01698ef98e06a9",
        },
    };
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
        static bignum n, s, want, got;
        from_hex(&n, v[k][0]);
        from_hex(&s, v[k][1]);
        from_hex(&want, v[k][2]);
        bn_modexp(&got, &s, 65537u, &n);
        KH_CHECK(same(&got, &want));
    }

    /* The boot's own case (ktest_arch.c, tlb modexp): n all ones. */
    static bignum n, b, want, got;
    uint8_t nb[256], sb[256];
    for (int i = 0; i < 256; i++) { nb[i] = 0xFF; sb[i] = (uint8_t)(i * 7 + 1); }
    bn_from_bytes(&n, nb, sizeof nb);
    bn_from_bytes(&b, sb, sizeof sb);
    from_hex(&want, "9b7c19063689e8ece8ece8700cd7800073dca10694be32b7a5c8fab24203880772a9969f034dc11995da8e1cf0f6b2d31a29d77e72e64c0ac9c1eb815b820ad4875c7579d7cefed36c53459bdf8dd7826b8c8d76c14e6d7a9f014599a89ad1779667877c3e9d3bbe03bf9c0ce468a6a90597c40e855a6d2fafe5c76983afb8c9a060e9d23741fdc2a6178e6d66d98ba9633493727967ffa01fd0384b52a16bdbcd1982c829874855b96f9472a55a07a6b8f1833512e70228bac7a201908797a81469ae15eb1257355bacdd418c49654ead1884434935b15964ecf343a153ceb5603ec654f2629246e329802d5cd263685c20a144b850faa8698cf997484821a1");
    bn_modexp(&got, &b, 65537u, &n);
    KH_CHECK(same(&got, &want));
}

KH_TEST(bignum_mod_takes_the_add_back_step) {
    /* u = 7fffffff 80000000 00000000 00000000, v = 80000000 00000000 00000001
       (most significant first): the first quotient guess survives both
       corrections and is still one too large. */
    static bignum a, m, got, want;
    bn_zero(&a); bn_zero(&m);
    a.limb[3] = 0x7FFFFFFFu; a.limb[2] = 0x80000000u; set_used(&a);
    m.limb[2] = 0x80000000u; m.limb[0] = 1; set_used(&m);
    bn_mod(&got, &a, &m);
    slow_mod(&want, &a, &m);
    KH_CHECK(same(&got, &want));
    /* and the value by hand: a - 0xFFFFFFFE * m */
    KH_CHECK_EQ(got.limb[0], 2);
    KH_CHECK_EQ(got.limb[1], 0xFFFFFFFFu);
    KH_CHECK_EQ(got.limb[2], 0x7FFFFFFFu);
    KH_CHECK_EQ(got.used, 3);
}

KH_TEST(bignum_mod_agrees_with_the_bit_serial_oracle) {
    static bignum a, m, got, want;
    int bad = 0;
    for (int k = 0; k < 400; k++) {
        int mw = 1 + (int)(next32() % (k < 200 ? 4 : BN_LIMBS));
        int aw = (int)(next32() % (BN_LIMBS * 2 + 1));
        fill(&m, mw);
        if (m.used == 0) m.limb[0] = 7, m.used = 1;
        fill(&a, aw);
        bn_mod(&got, &a, &m);
        slow_mod(&want, &a, &m);
        if (!same(&got, &want)) bad++;
    }
    KH_CHECK_EQ(bad, 0);
}

KH_TEST(bignum_mod_edges) {
    static bignum a, m, r;
    /* a < m: a itself; aliasing r and a */
    fill(&m, 10); m.limb[9] |= 1u; set_used(&m);
    fill(&a, 9);
    bn_mod(&r, &a, &m);
    KH_CHECK(same(&r, &a));
    bignum keep = m;
    bn_mod(&m, &m, &keep);          /* m mod m, aliased: zero */
    KH_CHECK_EQ(m.used, 0);
    /* a one-word modulus */
    fill(&a, 40);
    bn_zero(&m); m.limb[0] = 0xFFFFFFFBu; m.used = 1;
    static bignum want;
    slow_mod(&want, &a, &m);
    bn_mod(&a, &a, &m);
    KH_CHECK(same(&a, &want));
    /* m == 0 gives 0 */
    fill(&a, 5);
    bn_zero(&m);
    bn_mod(&r, &a, &m);
    KH_CHECK_EQ(r.used, 0);
}
