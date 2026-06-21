#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "profile.h"
#include "chord6.h"

/* v5: populate the appended L3/L4 banks with distinct values. Appended at the END
 * of make_full_profile(), just before `return p;`. */
static void fill_v5_extra_layers(struct profile *p)
{
    for (int L = 0; L < NUM_LAYERS - 2; L++) {          /* L=0 -> layer 3, L=1 -> layer 4 */
        for (int i = 0; i < NUM_FADERS; i++)
            p->layer[L].fader_cc[i]     = (uint8_t)(40 + L * 10 + i);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->layer[L].button_value[i] = (uint8_t)(60 + L * 10 + i);
            p->layer[L].button_key[i]   = (uint8_t)(0x14 + L * 10 + i);
            p->layer[L].button_mod[i]   = (uint8_t)((L * 4 + i) & 0x0F);
        }
    }
}

/* v6: populate the appended per-layer ext banks (L2, L3, L4) with NON-ZERO
 * distinct values so every appended offset is exercised. ext[0]=L2 (shift),
 * ext[1]=L3 (layer[0]), ext[2]=L4 (layer[1]). */
static void fill_v6_ext_layers(struct profile *p)
{
    for (int L = 0; L < NUM_LAYERS - 1; L++) {          /* L=0->L2, 1->L3, 2->L4 */
        for (int i = 0; i < NUM_FADERS; i++) {
            p->ext[L].fader_min[i]    = (uint8_t)(L * 4 + i);          /* 0..11 */
            p->ext[L].fader_max[i]    = (uint8_t)(110 + L + i);
            p->ext[L].fader_curve[i]  = (uint8_t)((L + i) % 3);        /* LINEAR/LOG/EXP */
            p->ext[L].fader_invert[i] = (uint8_t)((L + i) % 2);        /* 0/1 */
            p->ext[L].fader_channel[i]= (uint8_t)((L * 2 + i) & 0x0F); /* 0..15 */
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->ext[L].button_type[i]    = (uint8_t)((L + i) % 6);      /* BTN_NONE..SWITCH */
            p->ext[L].button_channel[i] = (uint8_t)((L * 3 + i + 1) & 0x0F);
        }
    }
}

/* Build a fully-populated profile with distinct values in every field. */
static struct profile make_full_profile(void)
{
    struct profile p;
    memset(&p, 0xAB, sizeof p);   /* fill with garbage first */
    p.version = PROFILE_VERSION;
    p.channel = 7;
    for (int i = 0; i < NUM_FADERS; i++) {
        p.fader[i].cc     = (uint8_t)(10 + i);
        p.fader[i].min    = (uint8_t)(i * 5);
        p.fader[i].max    = (uint8_t)(100 + i);
        p.fader[i].curve  = (uint8_t)(i % 3);   /* CURVE_LINEAR/LOG/EXP/LINEAR */
        p.fader[i].invert = (uint8_t)(i % 2);
        p.shift.fader_cc[i] = (uint8_t)(30 + i);
        p.fader_channel[i] = (uint8_t)(i & 0x0F);        /* v2: distinct per-fader ch */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button[i].type  = (uint8_t)(i % 6);   /* cycles BTN_NONE..BTN_PROFILE_SWITCH */
        p.button[i].value = (uint8_t)(40 + i);
        p.shift.button_value[i] = (uint8_t)(50 + i);
        p.button_channel[i] = (uint8_t)((i + 1) & 0x0F); /* v2: distinct per-button ch */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button_key[i] = (uint8_t)(0x04 + i);   /* v3: 'a','b',... usage ids */
        p.button_mod[i] = (uint8_t)(i & 0x0F);   /* v3: modifier bitmask */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button_key_shift[i] = (uint8_t)(0x20 + i);   /* v4: distinct shift usage ids */
        p.button_mod_shift[i] = (uint8_t)((i + 8) & 0x0F); /* v4: distinct shift mods */
    }
    for (int i = 0; i < 16; i++)
        p.name[i] = (uint8_t)('A' + i);
    fill_v5_extra_layers(&p);
    fill_v6_ext_layers(&p);
    /* v8: seed a VALID chord6 tail so make_full_profile() is validate-clean. The
     * 0xAB fill leaves the tail invalid; zero the chord6 grid + fader_role and set
     * the default velocity. Then plant ONE real chord6 of each mode so every packed
     * path is exercised by the round-trip + validate. */
    memset(p.chord6,     0, sizeof p.chord6);
    memset(p.fader_role, 0, sizeof p.fader_role);
    p.chord_flags[0] = 100;   /* default chord velocity */
    p.chord_flags[1] = 0;
    /* L0 b1 explicit C-E-G; L1 b2 range 21..27; L2 b3 Cmin7 root 48 */
    p.chord6[0][1] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };       /* mode0 cnt3 */
    p.chord6[1][2] = (struct chord6){ .b = { (1<<5), 21, 7, 0, 0, 0 } };       /* range */
    p.chord6[2][3] = (struct chord6){ .b = { (2<<5), 48, 5, 0, 0, 0 } };       /* Cmin7 */
    p.fader_role[0][0] = 1;   /* one chord_depth fader */
    return p;
}

/* ---- v8: the profile is 528 bytes, version 8, base64 704 chars ---- */
static void t_v8_size_and_version(void)
{
    assert(PROFILE_VERSION == 8);
    assert((int)sizeof(struct profile) == 528);
    /* chord6[4][9] replaces the shared table; tail = 216 + 16 + 2 = 234; 294+234=528 */
    struct profile p; memset(&p, 0, sizeof p); p.version = PROFILE_VERSION;
    char b64[800];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 704);                 /* ceil(528/3)*4, no padding */
    assert(b64[703] != '=');          /* 528 % 3 == 0 -> no '=' */
}

/* ---- round-trip ---- */
static void t_round_trip(void)
{
    struct profile orig = make_full_profile();
    /* Base64 of the packed profile (v7 = 444 bytes -> ceil(444/3)*4 = 148*4 = 592
     * chars + NUL). Buffer derived from sizeof, never hardcoded. */
    char b64[800];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof(b64));
    assert(enc_len > 0);

    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    int rc = profile_from_b64(b64, enc_len, &decoded);
    assert(rc == 0);
    assert(memcmp(&orig, &decoded, sizeof(struct profile)) == 0);
}

/* ---- v8: encoded length is exactly 704 chars (ceil(528/3)*4) ---- */
static void t_v4_encoded_length(void)
{
    struct profile p = make_full_profile();
    char b64[800];
    int enc_len = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(enc_len == 704);
    assert((int)sizeof(struct profile) == 528);
}

/* ---- v3: button_key/button_mod survive a round-trip ---- */
static void t_v3_keymap_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[800];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 704);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        assert(decoded.button_key[i] == (uint8_t)(0x04 + i));
        assert(decoded.button_mod[i] == (uint8_t)(i & 0x0F));
    }
}

/* ---- v4: button_key_shift/button_mod_shift survive a round-trip ---- */
static void t_v4_shift_keymap_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[800];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 704);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        assert(decoded.button_key_shift[i] == (uint8_t)(0x20 + i));
        assert(decoded.button_mod_shift[i] == (uint8_t)((i + 8) & 0x0F));
    }
}

/* ---- v6: a re-stamped fixture encodes to exactly 294 bytes / 392 chars (the v6
 *      prefix), proving the v6 wire length is still reachable under v7. The real
 *      v7 size/length test is t_v7_wire_len_and_roundtrip below. ---- */
static void t_v6_size_and_length(void)
{
    assert(NUM_LAYERS == 4);
    struct profile p = make_full_profile();
    p.version = 6;                           /* re-stamp so the encoder slices at v6 */
    char b64[512];
    int enc_len = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(enc_len == 392);                  /* ceil(294/3)*4, no padding */
    assert(b64[391] != '=');                 /* 294 % 3 == 0 -> no '=' */
}

/* ---- v8: full image is 528 bytes / 704 chars and round-trips exactly ---- */
static void t_v8_wire_len_and_roundtrip(void)
{
    assert((int)sizeof(struct profile) == 528);
    assert(PROFILE_VERSION == 8);
    struct profile orig = make_full_profile();   /* valid chord6 tail (Step 1) */
    char b64[800];
    int n = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(n == 704);                            /* ceil(528/3)*4, no padding */
    assert(b64[703] != '=');
    struct profile dec; memset(&dec, 0, sizeof dec);
    assert(profile_from_b64(b64, n, &dec) == 0);
    assert(memcmp(&orig, &dec, sizeof orig) == 0);
}

/* ---- v8: profile_validate chord6 + fader_role + velocity edge cases ---- */
static void t_v8_validate(void)
{
    struct profile p = make_full_profile();
    assert(profile_validate(&p) == 0);                 /* baseline valid */

    /* a BTN_CHORD button type is accepted (v7 raised the bound to 6; v8 keeps it) */
    struct profile q = p; q.button[0].type = BTN_CHORD; assert(profile_validate(&q) == 0);

    /* chord6 explicit count > 5 (stored cap) rejected */
    q = p; q.chord6[0][1].b[0] = (uint8_t)((0<<5) | 6); assert(profile_validate(&q) == -1);
    q = p; q.chord6[0][1].b[0] = (uint8_t)((0<<5) | 5); assert(profile_validate(&q) == 0);  /* 5 ok */

    /* chord6 mode > 2 rejected */
    q = p; q.chord6[0][1].b[0] = (uint8_t)(3<<5);       assert(profile_validate(&q) == -1);

    /* explicit note > 127 rejected (payload bytes are MIDI notes in mode 0) */
    q = p; q.chord6[0][1].b[1] = 128;                   assert(profile_validate(&q) == -1);

    /* range overflow rejected: start 120 + count 10 - 1 = 129 > 127 */
    q = p; q.chord6[1][2].b[0]=(1<<5); q.chord6[1][2].b[1]=120; q.chord6[1][2].b[2]=10;
    assert(profile_validate(&q) == -1);
    /* range count > MAX_CHORD (8) rejected even when it fits under 127 */
    q = p; q.chord6[1][2].b[0]=(1<<5); q.chord6[1][2].b[1]=0; q.chord6[1][2].b[2]=20;
    assert(profile_validate(&q) == -1);
    /* range count exactly 8 ok */
    q = p; q.chord6[1][2].b[0]=(1<<5); q.chord6[1][2].b[1]=0; q.chord6[1][2].b[2]=8;
    assert(profile_validate(&q) == 0);

    /* root+quality: root > 127 rejected; quality > 9 rejected; quality 5 ok */
    q = p; q.chord6[2][3].b[0]=(2<<5); q.chord6[2][3].b[1]=128; assert(profile_validate(&q) == -1);
    q = p; q.chord6[2][3].b[0]=(2<<5); q.chord6[2][3].b[2]=10;  assert(profile_validate(&q) == -1);
    q = p; q.chord6[2][3].b[0]=(2<<5); q.chord6[2][3].b[2]=9;   assert(profile_validate(&q) == 0);

    /* fader_role > 1 rejected; chord velocity > 127 rejected */
    q = p; q.fader_role[1][2] = 2;  assert(profile_validate(&q) == -1);
    q = p; q.chord_flags[0]  = 128; assert(profile_validate(&q) == -1);
}

/* ---- v5: the appended L3/L4 banks survive a round-trip ---- */
static void t_v5_extra_layers_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[800];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 704);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int L = 0; L < NUM_LAYERS - 2; L++) {
        for (int i = 0; i < NUM_FADERS; i++)
            assert(decoded.layer[L].fader_cc[i] == (uint8_t)(40 + L * 10 + i));
        for (int i = 0; i < NUM_BUTTONS; i++) {
            assert(decoded.layer[L].button_value[i] == (uint8_t)(60 + L * 10 + i));
            assert(decoded.layer[L].button_key[i]   == (uint8_t)(0x14 + L * 10 + i));
            assert(decoded.layer[L].button_mod[i]   == (uint8_t)((L * 4 + i) & 0x0F));
        }
    }
}

/* ---- v6: the appended per-layer ext banks (L2/L3/L4) survive a round-trip ---- */
static void t_v6_ext_layers_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[800];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 704);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            assert(decoded.ext[L].fader_min[i]     == (uint8_t)(L * 4 + i));
            assert(decoded.ext[L].fader_max[i]     == (uint8_t)(110 + L + i));
            assert(decoded.ext[L].fader_curve[i]   == (uint8_t)((L + i) % 3));
            assert(decoded.ext[L].fader_invert[i]  == (uint8_t)((L + i) % 2));
            assert(decoded.ext[L].fader_channel[i] == (uint8_t)((L * 2 + i) & 0x0F));
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            assert(decoded.ext[L].button_type[i]    == (uint8_t)((L + i) % 6));
            assert(decoded.ext[L].button_channel[i] == (uint8_t)((L * 3 + i + 1) & 0x0F));
        }
    }
}

/* ---- v5 cross-repo byte-parity: a FIXED profile encodes to the canonical
 *      240-char base64 shared by C / sp1ctl.py / the web codec. The first 118
 *      bytes are the unchanged v4 parity fixture (version byte restamped to 5);
 *      L3/L4 carry fixed distinct data. This is THE reconciliation gate. ---- */
static struct profile make_parity_v5(void)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = 5;
    p.channel = 5;
    static const uint8_t fcc[NUM_FADERS]   = {7, 74, 71, 76};
    static const uint8_t fmin[NUM_FADERS]  = {0, 10, 0, 5};
    static const uint8_t fmax[NUM_FADERS]  = {127, 120, 100, 127};
    static const uint8_t fcur[NUM_FADERS]  = {0, 1, 2, 0};
    static const uint8_t finv[NUM_FADERS]  = {0, 1, 0, 1};
    for (int i = 0; i < NUM_FADERS; i++) {
        p.fader[i].cc = fcc[i]; p.fader[i].min = fmin[i]; p.fader[i].max = fmax[i];
        p.fader[i].curve = fcur[i]; p.fader[i].invert = finv[i];
        p.fader_channel[i] = (uint8_t)i;
    }
    static const uint8_t btype[NUM_BUTTONS] = {1,2,3,4,5,0,1,2,3};
    static const uint8_t bval[NUM_BUTTONS]  = {60,64,65,1,2,0,62,80,81};
    static const uint8_t bch[NUM_BUTTONS]   = {4,5,6,7,8,9,10,11,12};
    static const uint8_t bkey[NUM_BUTTONS]  = {0x04,0x05,0x28,0x2c,0x2b,0x29,0x50,0x4f,0x00};
    static const uint8_t bmod[NUM_BUTTONS]  = {0x01,0x02,0x00,0x04,0x08,0x05,0x00,0x00,0x00};
    static const uint8_t bks[NUM_BUTTONS]   = {0x00,0x06,0x07,0x09,0x0a,0x0b,0x4a,0x4d,0x00};
    static const uint8_t bms[NUM_BUTTONS]   = {0x00,0x08,0x08,0x01,0x01,0x05,0x00,0x00,0x00};
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button[i].type = btype[i]; p.button[i].value = bval[i];
        p.button_channel[i] = bch[i];
        p.button_key[i] = bkey[i]; p.button_mod[i] = bmod[i];
        p.button_key_shift[i] = bks[i]; p.button_mod_shift[i] = bms[i];
    }
    static const uint8_t shfcc[NUM_FADERS]  = {20, 21, 22, 23};
    for (int i = 0; i < NUM_FADERS; i++) p.shift.fader_cc[i] = shfcc[i];
    static const uint8_t shbv[NUM_BUTTONS]  = {30,31,32,33,34,35,36,37,38};
    for (int i = 0; i < NUM_BUTTONS; i++) p.shift.button_value[i] = shbv[i];
    static const char nm[] = "OP-XY mix";
    memcpy(p.name, nm, sizeof nm - 1);
    /* L3 */
    static const uint8_t l3fcc[NUM_FADERS]  = {40,41,42,43};
    static const uint8_t l3bv[NUM_BUTTONS]  = {60,61,62,63,64,65,66,67,68};
    static const uint8_t l3bk[NUM_BUTTONS]  = {0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c};
    static const uint8_t l3bm[NUM_BUTTONS]  = {0x01,0x02,0x04,0x08,0x01,0x02,0x04,0x08,0x00};
    for (int i = 0; i < NUM_FADERS; i++)  p.layer[0].fader_cc[i]     = l3fcc[i];
    for (int i = 0; i < NUM_BUTTONS; i++){ p.layer[0].button_value[i]=l3bv[i];
        p.layer[0].button_key[i]=l3bk[i]; p.layer[0].button_mod[i]=l3bm[i]; }
    /* L4 */
    static const uint8_t l4fcc[NUM_FADERS]  = {50,51,52,53};
    static const uint8_t l4bv[NUM_BUTTONS]  = {70,71,72,73,74,75,76,77,78};
    static const uint8_t l4bk[NUM_BUTTONS]  = {0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26};
    static const uint8_t l4bm[NUM_BUTTONS]  = {0x08,0x04,0x02,0x01,0x08,0x04,0x02,0x01,0x00};
    for (int i = 0; i < NUM_FADERS; i++)  p.layer[1].fader_cc[i]     = l4fcc[i];
    for (int i = 0; i < NUM_BUTTONS; i++){ p.layer[1].button_value[i]=l4bv[i];
        p.layer[1].button_key[i]=l4bk[i]; p.layer[1].button_mod[i]=l4bm[i]; }
    return p;
}

#define PARITY_V5_B64 \
    "BQUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhce" \
    "HyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAwQFBgcICQoLDAQFKCwrKVBP" \
    "AAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYX" \
    "GBkaGxwBAgQIAQIECAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEA"

static void t_v5_golden_parity(void)
{
    struct profile p = make_parity_v5();
    char b64[256];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 240);
    assert(strcmp(b64, PARITY_V5_B64) == 0);   /* byte-identical to sp1ctl.py */
    /* spot-offsets, mirror sp1ctl.py's offset asserts */
    struct profile d;
    memset(&d, 0, sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(d.version == 5);
    assert(d.layer[0].fader_cc[0]     == 40);     /* off 118 */
    assert(d.layer[0].button_value[0] == 60);     /* off 122 */
    assert(d.layer[0].button_key[0]   == 0x14);   /* off 131 */
    assert(d.layer[1].fader_cc[0]     == 50);     /* off 149 */
    assert(d.layer[1].button_key[8]   == 0x26);   /* off 170 */
    assert(d.layer[1].button_mod[0]   == 0x08);   /* off 171 */
}

/* ---- v6 cross-repo byte-parity: a FIXED profile encodes to the canonical
 *      392-char base64 shared by C / sp1ctl.py / the web codec. The first 180
 *      bytes are the v5 parity fixture (version byte restamped to 6); the 114-byte
 *      tail carries fixed NON-ZERO ext data for L2/L3/L4 so every appended offset
 *      is exercised. This is THE v6 reconciliation gate the web side reconciles
 *      against. ---- */
static struct profile make_parity_v6(void)
{
    struct profile p = make_parity_v5();
    p.version = 6;
    /* ext[0] = L2 (shift), ext[1] = L3 (layer[0]), ext[2] = L4 (layer[1]). */
    static const uint8_t e_fmin[NUM_LAYERS - 1][NUM_FADERS] = {
        {0, 5, 10, 15}, {1, 2, 3, 4}, {20, 0, 7, 0} };
    static const uint8_t e_fmax[NUM_LAYERS - 1][NUM_FADERS] = {
        {127, 120, 100, 90}, {110, 111, 112, 113}, {127, 64, 80, 96} };
    static const uint8_t e_fcur[NUM_LAYERS - 1][NUM_FADERS] = {
        {0, 1, 2, 0}, {1, 2, 0, 1}, {2, 0, 1, 2} };
    static const uint8_t e_finv[NUM_LAYERS - 1][NUM_FADERS] = {
        {0, 1, 0, 1}, {1, 0, 1, 0}, {1, 1, 0, 0} };
    static const uint8_t e_btype[NUM_LAYERS - 1][NUM_BUTTONS] = {
        {1, 2, 3, 4, 5, 0, 1, 2, 3}, {2, 3, 4, 5, 0, 1, 2, 3, 4},
        {3, 4, 5, 0, 1, 2, 3, 4, 5} };
    static const uint8_t e_fch[NUM_LAYERS - 1][NUM_FADERS] = {
        {1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12} };
    static const uint8_t e_bch[NUM_LAYERS - 1][NUM_BUTTONS] = {
        {0, 1, 2, 3, 4, 5, 6, 7, 8}, {9, 10, 11, 12, 13, 14, 15, 0, 1},
        {2, 3, 4, 5, 6, 7, 8, 9, 10} };
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            p.ext[L].fader_min[i]     = e_fmin[L][i];
            p.ext[L].fader_max[i]     = e_fmax[L][i];
            p.ext[L].fader_curve[i]   = e_fcur[L][i];
            p.ext[L].fader_invert[i]  = e_finv[L][i];
            p.ext[L].fader_channel[i] = e_fch[L][i];
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p.ext[L].button_type[i]    = e_btype[L][i];
            p.ext[L].button_channel[i] = e_bch[L][i];
        }
    }
    return p;
}

#define PARITY_V6_B64 \
    "BgUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAw" \
    "QFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIE" \
    "CAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAUKD394ZFoAAQIAAAEAAQECAwQFAAECAwECAwQAAQIDBAUGBwgBAg" \
    "MEbm9wcQECAAEBAAEAAgMEBQABAgMEBQYHCAkKCwwNDg8AARQABwB/QFBgAgABAgEBAAADBAUAAQIDBAUJCgsMAgMEBQYHCAkK"

static void t_v6_golden_parity(void)
{
    struct profile p = make_parity_v6();
    char b64[512];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 392);
    assert(strcmp(b64, PARITY_V6_B64) == 0);   /* byte-identical to the web codec */
    /* prefix-superset proof: the SAME profile re-stamped to v5 encodes to a 180-byte
     * image that is byte-identical to the v6 image's first 180 bytes EXCEPT the
     * version byte at offset 0 (v6 stamps 6, v5 stamps 5). Compare the raw bytes so
     * the proof is exact, not base64-group-aligned. */
    struct profile p5 = p; p5.version = 5;
    char b64_5[256];
    int n5 = profile_to_b64(&p5, b64_5, (int)sizeof b64_5);
    assert(n5 == 240);
    {
        struct profile d6, d5;
        memset(&d6, 0, sizeof d6); memset(&d5, 0, sizeof d5);
        assert(profile_from_b64(b64,   n,  &d6) == 0);   /* full v6 */
        assert(profile_from_b64(b64_5, n5, &d5) == 0);   /* v5 prefix, ext inherited */
        const uint8_t *r6 = (const uint8_t *)&d6;
        const uint8_t *r5 = (const uint8_t *)&d5;
        assert(r6[0] == 6 && r5[0] == 5);                /* only the version differs */
        assert(memcmp(r6 + 1, r5 + 1, 180 - 1) == 0);    /* bytes 1..179 identical */
    }
    /* decode + offset asserts mirror the wire layout doc in profile.h */
    struct profile d;
    memset(&d, 0, sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(d.version == 6);
    assert(d.ext[0].fader_min[0]      == 0);     /* off 180  (L2) */
    assert(d.ext[0].fader_max[0]      == 127);   /* off 184 */
    assert(d.ext[0].fader_curve[1]    == 1);     /* off 189 */
    assert(d.ext[0].fader_invert[3]   == 1);     /* off 195 */
    assert(d.ext[0].button_type[0]    == 1);     /* off 196 */
    assert(d.ext[0].fader_channel[0]  == 1);     /* off 205 */
    assert(d.ext[0].button_channel[8] == 8);     /* off 217 */
    assert(d.ext[1].fader_min[0]      == 1);     /* off 218  (L3) */
    assert(d.ext[1].button_type[0]    == 2);     /* off 234 */
    assert(d.ext[1].fader_channel[3]  == 8);     /* off 246 */
    assert(d.ext[2].fader_min[0]      == 20);    /* off 256  (L4) */
    assert(d.ext[2].button_type[8]    == 5);     /* off 280 */
    assert(d.ext[2].button_channel[8] == 10);    /* off 293, last byte */
    /* the v5 prefix fields are untouched by the tail */
    assert(d.layer[1].button_mod[0]   == 0x08);  /* off 171 (v5) */
}

/* ---- v6: flipping one byte of the golden literal breaks parity (load-bearing) ---- */
static void t_v6_golden_is_load_bearing(void)
{
    struct profile p = make_parity_v6();
    char b64[512];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 392);
    assert(strcmp(b64, PARITY_V6_B64) == 0);
    /* flip a char IN THE TAIL region (>240) -> must no longer match */
    char broken[512];
    memcpy(broken, PARITY_V6_B64, (size_t)n + 1);
    broken[300] = (broken[300] == 'A') ? 'B' : 'A';
    assert(strcmp(b64, broken) != 0);
    /* restore -> matches again */
    memcpy(broken, PARITY_V6_B64, (size_t)n + 1);
    assert(strcmp(b64, broken) == 0);
}

/* v8 cross-repo byte-parity: make_parity_v6 re-stamped to v8 + a FIXED chord6 tail
 * exercising every packed mode + the prefix-superset boundary. THE reconciliation
 * gate sp1ctl.py and codec.ts reconcile against. Diagonal layout (mirrors sp1ctl.py):
 *   L0 b1 = explicit C-E-G; L1 b2 = range 21..27; L2 b3 = Cmin7 (root 48, q 5);
 *   L3 b4 = explicit C-E-G; fader_role identity diagonal; velocity 100. */
static struct profile make_parity_v8(void)
{
    struct profile p = make_parity_v6();
    p.version = 8;
    memset(p.chord6,     0, sizeof p.chord6);
    memset(p.fader_role, 0, sizeof p.fader_role);
    p.chord6[0][1] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };   /* explicit C-E-G */
    p.chord6[1][2] = (struct chord6){ .b = { (1<<5), 21, 7, 0, 0, 0 } };   /* range 21..27 */
    p.chord6[2][3] = (struct chord6){ .b = { (2<<5), 48, 5, 0, 0, 0 } };   /* Cmin7 */
    p.chord6[3][4] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };   /* explicit C-E-G */
    for (int L = 0; L < NUM_LAYERS; L++) p.fader_role[L][L % NUM_FADERS] = 1;
    p.chord_flags[0] = 100; p.chord_flags[1] = 0;
    return p;
}

#define PARITY_V8_B64 \
    "CAUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAw" \
    "QFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIE" \
    "CAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAUKD394ZFoAAQIAAAEAAQECAwQFAAECAwECAwQAAQIDBAUGBwgBAg" \
    "MEbm9wcQECAAEBAAEAAgMEBQABAgMEBQYHCAkKCwwNDg8AARQABwB/QFBgAgABAgEBAAADBAUAAQIDBAUJCgsMAgMEBQYHCAkK" \
    "AAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAA" \
    "ABAAAAAAEAAAAAAWQA"

static void t_v8_golden_parity(void)
{
    struct profile p = make_parity_v8();
    char b64[800];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 704);
    assert(strcmp(b64, PARITY_V8_B64) == 0);   /* byte-identical to sp1ctl.py + codec.ts */
    assert(b64[703] != '=');                   /* 528 % 3 == 0 -> no padding */
}
static void t_v8_golden_is_load_bearing(void)
{
    struct profile p = make_parity_v8();
    char b64[800]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(strcmp(b64, PARITY_V8_B64) == 0);
    char broken[800]; memcpy(broken, PARITY_V8_B64, (size_t)n + 1);
    broken[450] = (broken[450] == 'A') ? 'B' : 'A';   /* flip a char IN THE TAIL (>392) */
    assert(strcmp(b64, broken) != 0);
}
static void t_v8_chord_round_trip(void)
{
    struct profile orig = make_parity_v8();
    char b64[800]; int n = profile_to_b64(&orig, b64, (int)sizeof b64);
    struct profile dec; memset(&dec, 0, sizeof dec);
    assert(profile_from_b64(b64, n, &dec) == 0);
    assert(memcmp(&orig, &dec, sizeof orig) == 0);
    /* spot-check the packed tail survived */
    assert(dec.chord6[1][2].b[1] == 21 && dec.chord6[1][2].b[2] == 7);   /* range */
    assert(dec.chord6[2][3].b[1] == 48 && dec.chord6[2][3].b[2] == 5);   /* Cmin7 */
    assert(dec.fader_role[0][0] == 1);
    assert(dec.chord_flags[0] == 100);
    /* and that it unpacks back to a usable chord_def */
    struct chord_def cd;
    chord6_unpack(&dec.chord6[0][1], &cd);
    assert(cd.count == 3 && cd.notes[0] == 60);
}
static void t_v8_prefix_superset(void)
{
    struct profile p8 = make_parity_v8();
    struct profile p6 = p8; p6.version = 6;
    char b8[800], b6[512];
    int n8 = profile_to_b64(&p8, b8, (int)sizeof b8);
    int n6 = profile_to_b64(&p6, b6, (int)sizeof b6);
    assert(n8 == 704 && n6 == 392);
    struct profile d8, d6; memset(&d8,0,sizeof d8); memset(&d6,0,sizeof d6);
    assert(profile_from_b64(b8, n8, &d8) == 0);
    assert(profile_from_b64(b6, n6, &d6) == 0);
    const uint8_t *r8=(const uint8_t*)&d8, *r6=(const uint8_t*)&d6;
    assert(r8[0]==8 && r6[0]==6);
    assert(memcmp(r8+1, r6+1, 294-1) == 0);   /* first 294 bytes are the v6 image (only byte0 differs) */
}
static void t_v8_rejects_v7_blob(void)
{
    struct profile p = make_parity_v8();
    p.version = 7;                 /* a v7-firmware blob reaching v8 firmware */
    assert(profile_validate(&p) == -1);
}
/* t_v8_default_seed_layout is defined after mirror_default (below). */

/* ---- v4: a v3 blob (version byte 3) is rejected by validate ---- */
static void t_v4_rejects_v3_blob(void)
{
    struct profile p = make_full_profile();
    p.version = 3;                 /* a v3-firmware blob reaching v4 firmware */
    assert(profile_validate(&p) == -1);
}

/* ---- validate: accepts good profile ---- */
static void t_validate_good(void)
{
    struct profile p = make_full_profile();
    assert(profile_validate(&p) == 0);
}

/* ---- validate: rejects bad version ---- */
static void t_validate_bad_version(void)
{
    struct profile p = make_full_profile();
    p.version = PROFILE_VERSION + 1;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects channel > 15 ---- */
static void t_validate_bad_channel(void)
{
    struct profile p = make_full_profile();
    p.channel = 16;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects fader.cc > 127 ---- */
static void t_validate_bad_fader_cc(void)
{
    struct profile p = make_full_profile();
    p.fader[2].cc = 128;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects fader.curve > 2 ---- */
static void t_validate_bad_fader_curve(void)
{
    struct profile p = make_full_profile();
    p.fader[1].curve = 3;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects button.type > BTN_CHORD (v7 raised the bound to 6) ---- */
static void t_validate_bad_button_type(void)
{
    struct profile p = make_full_profile();
    p.button[3].type = 7;            /* one past BTN_CHORD */
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects fader_channel > 15 (v2) ---- */
static void t_validate_bad_fader_channel(void)
{
    struct profile p = make_full_profile();
    p.fader_channel[2] = 16;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects button_channel > 15 (v2) ---- */
static void t_validate_bad_button_channel(void)
{
    struct profile p = make_full_profile();
    p.button_channel[5] = 16;
    assert(profile_validate(&p) == -1);
}

/* ---- validate: rejects a bad v6 ext field (per-layer curve/channel/type) ---- */
static void t_validate_bad_ext_fields(void)
{
    struct profile p = make_full_profile();
    p.ext[1].fader_curve[2] = 3;             /* > CURVE_EXP */
    assert(profile_validate(&p) == -1);
    p = make_full_profile();
    p.ext[2].fader_channel[0] = 16;          /* > 15 */
    assert(profile_validate(&p) == -1);
    p = make_full_profile();
    p.ext[0].button_type[3] = 7;             /* > BTN_CHORD (v7 raised the bound to 6) */
    assert(profile_validate(&p) == -1);
    p = make_full_profile();
    p.ext[2].button_channel[8] = 16;         /* > 15 */
    assert(profile_validate(&p) == -1);
    p = make_full_profile();
    p.ext[0].fader_max[1] = 128;             /* > 127 */
    assert(profile_validate(&p) == -1);
}

/* ---- from_b64 rejects wrong length ---- */
static void t_from_b64_wrong_length(void)
{
    /* Base64 of 4 bytes (too short) */
    const char *short_b64 = "AAAA";   /* decodes to 3 bytes, not sizeof(struct profile) */
    struct profile out;
    memset(&out, 0x55, sizeof out);
    int rc = profile_from_b64(short_b64, (int)strlen(short_b64), &out);
    assert(rc == -1);
    /* Struct must not have been clobbered to all-zeros (first byte still 0x55) */
    unsigned char first;
    memcpy(&first, &out, 1);
    assert(first == 0x55);
}

/* ---- to_b64 respects outcap ---- */
static void t_to_b64_outcap(void)
{
    struct profile p = make_full_profile();
    char tiny[4];   /* way too small */
    int rc = profile_to_b64(&p, tiny, (int)sizeof(tiny));
    assert(rc == -1);
}

/* ---- from_b64 rejects lying len (ASAN regression) ---- */
static void t_from_b64_lying_len(void){
    struct profile out;
    memset(&out, 0, sizeof out);
    /* short real string (8 chars), but claim it's the full expected length */
    int r = profile_from_b64("AAAABBBB", 64, &out);      /* len lies */
    assert(r == -1);
}

/* Mirror librarian.c make_default's v6 seed (0.12.1 three-channel default). HAND
 * MIRROR (librarian.c is not host-buildable), so it must track make_default exactly:
 *
 *   fader_cc       = P*16 + L*4 + F                 (one counter, 0..127 unique, ch1)
 *   front button   = P*16 + L*4 + (i-1)  on ch2     (idx 1..4, 0..127 unique)
 *   side  button   = P*16 + L*4 + (i-5)  on ch3     (idx 5..8, 0..127 unique)
 *   PLAY (idx 0)   = value 0 on the profile channel (gesture button, never emits)
 *
 * with P = within-bank index (0..7), L = layer (0..3: L1/L2/L3/L4). Faders own ch1,
 * front track buttons ch2 (channel 1), side buttons ch3 (channel 2), so a fader and a
 * button can never share a (channel, CC). Keyboard key/mod default unbound (0). The
 * appended ext banks INHERIT L1's per-fader min/max/curve/invert, L1's button type,
 * and L1's per-control channels, so every layer is a complete, sensible default out of
 * the box (NOT min=max=0) and the channel scheme propagates to all 4 layers. This pins
 * the seed the librarian must produce. */
static uint8_t md_btn_channel(int i)
{
    if (i >= 1 && i <= 4) return 1;   /* front -> ch2 */
    if (i >= 5 && i <= 8) return 2;   /* side  -> ch3 */
    return 0;                          /* PLAY  -> profile channel */
}
static uint8_t md_btn_value(int within, int L, int i)
{
    if (i >= 1 && i <= 4) return (uint8_t)(within * 16 + L * 4 + (i - 1));
    if (i >= 5 && i <= 8) return (uint8_t)(within * 16 + L * 4 + (i - 5));
    return 0;
}
static struct profile mirror_default(int within)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;
    p.channel = 0;
    int fbase = within * (NUM_LAYERS * NUM_FADERS);   /* P*16 */
    for (int i = 0; i < NUM_FADERS; i++) {
        p.fader[i].cc = (uint8_t)(fbase + 0 * NUM_FADERS + i);          /* L1: P*16 + 0..3 */
        p.fader[i].min = 0; p.fader[i].max = 127;
        p.fader[i].curve = CURVE_LINEAR; p.fader[i].invert = 0;
        p.shift.fader_cc[i]    = (uint8_t)(fbase + 1 * NUM_FADERS + i); /* L2: P*16 + 4..7  */
        p.layer[0].fader_cc[i] = (uint8_t)(fbase + 2 * NUM_FADERS + i); /* L3: P*16 + 8..11 */
        p.layer[1].fader_cc[i] = (uint8_t)(fbase + 3 * NUM_FADERS + i); /* L4: P*16 + 12..15 */
        p.fader_channel[i] = 0;
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button[i].type = BTN_CC_MOMENTARY;
        p.button[i].value          = md_btn_value(within, 0, i); /* L1 */
        p.shift.button_value[i]    = md_btn_value(within, 1, i); /* L2 */
        p.layer[0].button_value[i] = md_btn_value(within, 2, i); /* L3 */
        p.layer[1].button_value[i] = md_btn_value(within, 3, i); /* L4 */
        p.button_channel[i] = md_btn_channel(i);
        /* keyboard layers default unbound: button_key/mod and layer[*].button_key/mod all 0 */
    }
    /* v6: each ext bank inherits L1's scale fields, button type, and channels. */
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            p.ext[L].fader_min[i]     = p.fader[i].min;
            p.ext[L].fader_max[i]     = p.fader[i].max;
            p.ext[L].fader_curve[i]   = p.fader[i].curve;
            p.ext[L].fader_invert[i]  = p.fader[i].invert;
            p.ext[L].fader_channel[i] = p.fader_channel[i];
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p.ext[L].button_type[i]    = p.button[i].type;
            p.ext[L].button_channel[i] = p.button_channel[i];
        }
    }
    /* v8: default chord tail = no chords (chord6 grid all-zero from the memset) +
     * chord velocity 100 + every fader_role = cc (also from the memset). */
    p.chord_flags[0] = 100;
    return p;
}

/* Return the 4 fader CCs of layer L (0=L1, 1=L2, 2=L3, 3=L4) of profile p into out[4]. */
static void layer_fader_ccs(const struct profile *p, int L, uint8_t out[NUM_FADERS])
{
    for (int i = 0; i < NUM_FADERS; i++) {
        if (L == 0)      out[i] = p->fader[i].cc;
        else if (L == 1) out[i] = p->shift.fader_cc[i];
        else if (L == 2) out[i] = p->layer[0].fader_cc[i];
        else             out[i] = p->layer[1].fader_cc[i];
    }
}

/* Return the 9 button values of layer L (0=L1,1=L2,2=L3,3=L4) of profile p into out[9]. */
static void layer_button_values(const struct profile *p, int L, uint8_t out[NUM_BUTTONS])
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (L == 0)      out[i] = p->button[i].value;
        else if (L == 1) out[i] = p->shift.button_value[i];
        else if (L == 2) out[i] = p->layer[0].button_value[i];
        else             out[i] = p->layer[1].button_value[i];
    }
}

static void t_v6_default_seed_layout(void)
{
    /* ---- pin the new fader-CC layout at the corners ---- */
    /* slot 0 (within 0): L1 faders = {0,1,2,3} */
    struct profile p0 = mirror_default(0);
    uint8_t l[NUM_FADERS];
    layer_fader_ccs(&p0, 0, l);
    assert(l[0] == 0 && l[1] == 1 && l[2] == 2 && l[3] == 3);
    /* slot 1 (within 1): L1 faders = {16,17,18,19} */
    struct profile p1 = mirror_default(1);
    layer_fader_ccs(&p1, 0, l);
    assert(l[0] == 16 && l[1] == 17 && l[2] == 18 && l[3] == 19);
    /* slot 7 (within 7): L4 faders = {124,125,126,127} (the global max) */
    struct profile p7 = mirror_default(7);
    layer_fader_ccs(&p7, 3, l);
    assert(l[0] == 124 && l[1] == 125 && l[2] == 126 && l[3] == 127);

    /* ---- the 128 default fader CCs (8 profiles x 4 layers x 4 faders) are UNIQUE,
     *      cover 0..127 with no overlap, and max is exactly 127 ---- */
    int seen[128] = {0};
    int max_cc = -1, count = 0;
    for (int P = 0; P < NUM_BANK_PROFILES; P++) {
        struct profile pp = mirror_default(P);
        for (int L = 0; L < NUM_LAYERS; L++) {
            uint8_t ccs[NUM_FADERS];
            layer_fader_ccs(&pp, L, ccs);
            for (int i = 0; i < NUM_FADERS; i++) {
                assert(ccs[i] <= 127);
                assert(seen[ccs[i]] == 0);     /* no overlap: each CC used exactly once */
                seen[ccs[i]] = 1;
                if (ccs[i] > max_cc) max_cc = ccs[i];
                count++;
            }
        }
    }
    assert(count == 128);                      /* 8*4*4 fader slots */
    assert(max_cc == 127);                     /* P7L3F3 = 127 */
    for (int v = 0; v < 128; v++) assert(seen[v] == 1);  /* full 0..127 coverage */

    /* ---- buttons: the 0.12.1 three-channel "stable, non-conflicting default".
     *      FRONT track buttons (idx 1..4) default to MIDI ch2 (channel 1), SIDE
     *      buttons (idx 5..8) to ch3 (channel 2), PLAY (idx 0) stays on the profile
     *      channel and emits nothing. Every layer inherits the same channels. All
     *      values stay <= 127. ---- */
    for (int P = 0; P < NUM_BANK_PROFILES; P++) {
        struct profile pp = mirror_default(P);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            uint8_t want_ch = (i >= 1 && i <= 4) ? 1 : (i >= 5 && i <= 8) ? 2 : 0;
            assert(pp.button_channel[i] == want_ch);            /* faders ch1 / front ch2 / side ch3 */
            for (int L = 0; L < NUM_LAYERS - 1; L++)
                assert(pp.ext[L].button_channel[i] == want_ch); /* every layer inherits the scheme */
            assert(pp.button[i].value          <= 127);
            assert(pp.shift.button_value[i]    <= 127);
            assert(pp.layer[0].button_value[i] <= 127);
            assert(pp.layer[1].button_value[i] <= 127);
        }
        assert(pp.button[0].value == 0);                        /* PLAY emits nothing */
    }

    /* the 128 FRONT-button CCs (idx 1..4 over 8 profiles x 4 layers) are UNIQUE and
     * cover 0..127; likewise the 128 SIDE-button CCs (idx 5..8). Each of ch2/ch3 is a
     * full non-overlapping surface, exactly like the faders on ch1 — so faders, front,
     * and side never collide (different channels) AND never self-collide. */
    {
        int seen_front[128] = {0}, seen_side[128] = {0};
        int cf = 0, cs = 0;
        for (int P = 0; P < NUM_BANK_PROFILES; P++) {
            struct profile pp = mirror_default(P);
            for (int L = 0; L < NUM_LAYERS; L++) {
                uint8_t bv[NUM_BUTTONS];
                layer_button_values(&pp, L, bv);
                for (int i = 1; i <= 4; i++) { assert(seen_front[bv[i]] == 0); seen_front[bv[i]] = 1; cf++; }
                for (int i = 5; i <= 8; i++) { assert(seen_side[bv[i]]  == 0); seen_side[bv[i]]  = 1; cs++; }
            }
        }
        assert(cf == 128 && cs == 128);
        for (int v = 0; v < 128; v++) { assert(seen_front[v] == 1); assert(seen_side[v] == 1); }
    }

    /* ---- the default profile still encodes to exactly 528B/704ch and round-trips,
     *      validates, and keeps every invariant (slot 0 as the representative) ---- */
    struct profile p = mirror_default(0);
    assert(profile_validate(&p) == 0);
    char b64[800];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 704);
    assert((int)sizeof(struct profile) == 528);
    struct profile d; memset(&d, 0, sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(memcmp(&p, &d, sizeof p) == 0);
    /* L3/L4 keyboard banks are unbound */
    for (int i = 0; i < NUM_BUTTONS; i++) {
        assert(d.layer[0].button_key[i] == 0 && d.layer[0].button_mod[i] == 0);
        assert(d.layer[1].button_key[i] == 0 && d.layer[1].button_mod[i] == 0);
    }
    /* no fader CC anywhere exceeds 127 */
    for (int i = 0; i < NUM_FADERS; i++) {
        assert(d.fader[i].cc <= 127 && d.shift.fader_cc[i] <= 127);
        assert(d.layer[0].fader_cc[i] <= 127 && d.layer[1].fader_cc[i] <= 127);
    }
    /* v6: ext banks INHERIT L1 -> every layer's faders have a non-zero usable max
     * (127) and L1's curve/range, so all 4 layers look like L1 out of the box. */
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            assert(d.ext[L].fader_max[i] == 127);                 /* non-zero, usable */
            assert(d.ext[L].fader_max[i] == d.fader[i].max);      /* matches L1 range */
            assert(d.ext[L].fader_min[i] == d.fader[i].min);
            assert(d.ext[L].fader_curve[i] == d.fader[i].curve);  /* matches L1 curve */
            assert(d.ext[L].fader_invert[i] == d.fader[i].invert);
            assert(d.ext[L].fader_channel[i] == d.fader_channel[i]);
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            assert(d.ext[L].button_type[i] == d.button[i].type);  /* matches L1 type */
            assert(d.ext[L].button_channel[i] == d.button_channel[i]);
        }
    }
}

static void t_v8_default_seed_layout(void)
{
    struct profile p = mirror_default(0);
    assert(profile_validate(&p) == 0);
    /* no chords seeded: every chord6 slot is empty (all-zero packed bytes) */
    for (int L=0;L<NUM_LAYERS;L++)
        for (int i=0;i<NUM_BUTTONS;i++)
            for (int b=0;b<6;b++) assert(p.chord6[L][i].b[b] == 0);
    /* every fader role is cc */
    for (int L=0;L<NUM_LAYERS;L++)
        for (int f=0;f<NUM_FADERS;f++) assert(p.fader_role[L][f]==FADER_ROLE_CC);
    assert(p.chord_flags[0] == 100);
    /* no BTN_CHORD seeded anywhere (default buttons are CC_MOMENTARY) */
    for (int i=0;i<NUM_BUTTONS;i++) assert(p.button[i].type != BTN_CHORD);
    /* still 528 B / 704 ch and round-trips */
    char b64[800]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 704);
    struct profile d; memset(&d,0,sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(memcmp(&p, &d, sizeof p) == 0);
}

int main(void)
{
    t_round_trip();
    t_v8_size_and_version();
    t_v4_encoded_length();
    t_v3_keymap_round_trip();
    t_v4_shift_keymap_round_trip();
    t_v6_size_and_length();
    t_v8_wire_len_and_roundtrip();
    t_v8_validate();
    t_v5_extra_layers_round_trip();
    t_v6_ext_layers_round_trip();
    t_v5_golden_parity();
    t_v6_golden_parity();
    t_v6_golden_is_load_bearing();
    t_v8_golden_parity();
    t_v8_golden_is_load_bearing();
    t_v8_chord_round_trip();
    t_v8_prefix_superset();
    t_v8_rejects_v7_blob();
    t_v8_default_seed_layout();
    t_v4_rejects_v3_blob();
    t_validate_good();
    t_validate_bad_version();
    t_validate_bad_channel();
    t_validate_bad_fader_cc();
    t_validate_bad_fader_curve();
    t_validate_bad_button_type();
    t_validate_bad_fader_channel();
    t_validate_bad_button_channel();
    t_validate_bad_ext_fields();
    t_from_b64_wrong_length();
    t_to_b64_outcap();
    t_from_b64_lying_len();
    t_v6_default_seed_layout();
    printf("all profile tests passed\n");
    return 0;
}
