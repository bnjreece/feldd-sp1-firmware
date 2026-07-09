#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "profile.h"
#include "chord6.h"

/* v5: populate the appended L3..L8 banks with distinct values. Appended at the END
 * of make_full_profile(), just before `return p;`. The loop covers layer[0..5]
 * automatically at NUM_LAYERS==8. */
static void fill_v5_extra_layers(struct profile *p)
{
    for (int L = 0; L < NUM_LAYERS - 2; L++) {          /* L=0 -> layer 3, ... L=5 -> layer 8 */
        for (int i = 0; i < NUM_FADERS; i++)
            p->layer[L].fader_cc[i]     = (uint8_t)(40 + L * 10 + i);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->layer[L].button_value[i] = (uint8_t)(60 + L * 10 + i);
            p->layer[L].button_key[i]   = (uint8_t)(0x14 + L * 10 + i);
            p->layer[L].button_mod[i]   = (uint8_t)((L * 4 + i) & 0x0F);
        }
    }
}

/* v6: populate the appended per-layer ext banks (L2..L8) with NON-ZERO distinct
 * values so every appended offset is exercised. The loop covers ext[0..6]
 * automatically at NUM_LAYERS==8. */
static void fill_v6_ext_layers(struct profile *p)
{
    for (int L = 0; L < NUM_LAYERS - 1; L++) {          /* L=0->L2, ... 6->L8 */
        for (int i = 0; i < NUM_FADERS; i++) {
            p->ext[L].fader_min[i]    = (uint8_t)(L * 4 + i);          /* stays <=127 */
            p->ext[L].fader_max[i]    = (uint8_t)((110 + L + i) & 0x7F);
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
    /* v8/v9: seed a VALID chord6 tail so make_full_profile() is validate-clean. The
     * 0xAB fill leaves the tail invalid; zero the chord6 grid + fader_role and set
     * the default velocity. Then plant ONE real chord6 of each mode so every packed
     * path is exercised by the round-trip + validate. */
    memset(p.chord6,     0, sizeof p.chord6);
    memset(p.fader_role, 0, sizeof p.fader_role);
    memset(p.chord_flags, 0, sizeof p.chord_flags);   /* clear the v9 pad bytes too */
    p.chord_flags[0] = 100;   /* default chord velocity */
    /* L0 b1 explicit C-E-G; L1 b2 range 21..27; L2 b3 Cmin7 root 48 */
    p.chord6[0][1] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };       /* mode0 cnt3 */
    p.chord6[1][2] = (struct chord6){ .b = { (1<<5), 21, 7, 0, 0, 0 } };       /* range */
    p.chord6[2][3] = (struct chord6){ .b = { (2<<5), 48, 5, 0, 0, 0 } };       /* Cmin7 */
    p.fader_role[0][0] = 1;   /* one chord_depth fader */
    return p;
}

/* v9 3-way byte-parity fixture (THE reconciliation gate, firmware authoritative).
 * A fully-populated 1038-byte v9 profile: all 8 layers carry distinct faders,
 * buttons, keymaps and a full ext page bank; a diagonal chord6 exercises every
 * packed mode. Heterogeneous per-layer storage (spec 8.4): L0 inline, L1 shift +
 * ext[0], L2..L7 layer[L-2] + ext[L-1]; chord6/fader_role are DIRECT-indexed by
 * layer 0..7. sp1ctl.py + codec.test.ts pin PARITY_V9_B64 to whatever this encodes. */
static void v9_set_layer(struct profile *p, int L,
        const uint8_t fcc[NUM_FADERS], const uint8_t bval[NUM_BUTTONS],
        const uint8_t bkey[NUM_BUTTONS], const uint8_t bmod[NUM_BUTTONS],
        const uint8_t fmin[NUM_FADERS], const uint8_t fmax[NUM_FADERS],
        const uint8_t fcur[NUM_FADERS], const uint8_t finv[NUM_FADERS],
        const uint8_t btype[NUM_BUTTONS], const uint8_t fch[NUM_FADERS],
        const uint8_t bch[NUM_BUTTONS])
{
    if (L == 0) {                                   /* L0: inline + top-level page */
        for (int i = 0; i < NUM_FADERS; i++) {
            p->fader[i].cc = fcc[i]; p->fader[i].min = fmin[i]; p->fader[i].max = fmax[i];
            p->fader[i].curve = fcur[i]; p->fader[i].invert = finv[i];
            p->fader_channel[i] = fch[i];
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->button[i].type = btype[i]; p->button[i].value = bval[i];
            p->button_channel[i] = bch[i]; p->button_key[i] = bkey[i]; p->button_mod[i] = bmod[i];
        }
        return;
    }
    struct layer_ext *e = &p->ext[L - 1];           /* L1..L7 own ext[0..6] */
    for (int i = 0; i < NUM_FADERS; i++) {
        e->fader_min[i] = fmin[i]; e->fader_max[i] = fmax[i]; e->fader_curve[i] = fcur[i];
        e->fader_invert[i] = finv[i]; e->fader_channel[i] = fch[i];
    }
    for (int i = 0; i < NUM_BUTTONS; i++) { e->button_type[i] = btype[i]; e->button_channel[i] = bch[i]; }
    if (L == 1) {                                   /* L1: shift bank + shift keymap */
        for (int i = 0; i < NUM_FADERS; i++)  p->shift.fader_cc[i] = fcc[i];
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->shift.button_value[i] = bval[i];
            p->button_key_shift[i] = bkey[i]; p->button_mod_shift[i] = bmod[i];
        }
    } else {                                        /* L2..L7: layer[0..5] bank */
        struct layer_bank *b = &p->layer[L - 2];
        for (int i = 0; i < NUM_FADERS; i++)  b->fader_cc[i] = fcc[i];
        for (int i = 0; i < NUM_BUTTONS; i++) {
            b->button_value[i] = bval[i]; b->button_key[i] = bkey[i]; b->button_mod[i] = bmod[i];
        }
    }
}

static struct profile make_parity_v9(void)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;                     /* == 9 */
    p.channel = 5;
    memcpy(p.name, "OP-XY 8layer", 12);
    for (int L = 0; L < NUM_LAYERS; L++) {           /* NUM_LAYERS == 8 */
        uint8_t fcc[NUM_FADERS], fmin[NUM_FADERS], fmax[NUM_FADERS];
        uint8_t fcur[NUM_FADERS], finv[NUM_FADERS], fch[NUM_FADERS];
        uint8_t btype[NUM_BUTTONS], bval[NUM_BUTTONS], bch[NUM_BUTTONS];
        uint8_t bkey[NUM_BUTTONS], bmod[NUM_BUTTONS];
        for (int i = 0; i < NUM_FADERS; i++) {
            fcc[i]  = (uint8_t)((10 + L * NUM_FADERS + i) & 0x7F);
            fmin[i] = (uint8_t)((L + i) & 0x7F);
            fmax[i] = (uint8_t)((100 + L + i) & 0x7F);
            fcur[i] = (uint8_t)((L + i) % 3);
            finv[i] = (uint8_t)((L + i) % 2);
            fch[i]  = (uint8_t)((L + i) & 0x0F);
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            btype[i] = (uint8_t)((L + i) % 6);
            bval[i]  = (uint8_t)((20 + L * NUM_BUTTONS + i) & 0x7F);
            bch[i]   = (uint8_t)((L + i + 1) & 0x0F);
            bkey[i]  = (uint8_t)((0x04 + L * 3 + i) & 0xFF);
            bmod[i]  = (uint8_t)((L + i) & 0x0F);
        }
        v9_set_layer(&p, L, fcc, bval, bkey, bmod, fmin, fmax, fcur, finv, btype, fch, bch);
    }
    /* diagonal chord6 (direct layer index 0..7) exercising all three packed modes */
    p.chord6[0][1] = (struct chord6){ .b = { 0x03,   60, 64, 67, 0, 0 } };   /* explicit C-E-G */
    p.chord6[1][2] = (struct chord6){ .b = { (1<<5), 21, 7,  0, 0, 0 } };    /* range 21..27  */
    p.chord6[2][3] = (struct chord6){ .b = { (2<<5), 48, 5,  0, 0, 0 } };    /* Cmin7         */
    p.chord6[5][4] = (struct chord6){ .b = { 0x03,   36, 40, 43, 0, 0 } };   /* explicit, high layer */
    for (int L = 0; L < NUM_LAYERS; L++) p.fader_role[L][L % NUM_FADERS] = 1;
    p.chord_flags[0] = 100;                          /* velocity */
    /* chord_flags[1..3] stay 0 (bytes 1035..1037 are the v9 pad, spec 2.1) */
    return p;
}

/* ---- v9: the profile is 1038 bytes, version 9, base64 1384 chars, and every
 *      moved interior offset matches spec 2.3 (grow-in-place, padded to a
 *      multiple of 3). The four fixed-prefix anchors are byte-identical to v8. ---- */
static void t_v9_geometry(void)
{
    assert(PROFILE_VERSION == 9);
    assert(NUM_LAYERS == 8);
    assert(sizeof(struct profile) == 1038);
    /* fixed prefix [0..117] unchanged from v8 */
    assert(offsetof(struct profile, fader_channel)    == 69);
    assert(offsetof(struct profile, button_key)       == 82);
    assert(offsetof(struct profile, button_key_shift) == 100);
    assert(offsetof(struct profile, layer)            == 118);
    /* moved interior arrays (v9) */
    assert(offsetof(struct profile, ext)              == 304);
    assert(offsetof(struct profile, chord6)           == 570);
    assert(offsetof(struct profile, fader_role)       == 1002);
    assert(offsetof(struct profile, chord_flags)      == 1034);
    /* the codec emits the full padded image with no '=' padding */
    struct profile p; memset(&p, 0, sizeof p); p.version = PROFILE_VERSION;
    char b64[1400];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);            /* ceil(1038/3)*4 */
    assert(b64[1383] != '=');     /* 1038 % 3 == 0 -> no padding */
}

/* ---- round-trip ---- */
static void t_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[1400];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof(b64));
    assert(enc_len > 0);

    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    int rc = profile_from_b64(b64, enc_len, &decoded);
    assert(rc == 0);
    assert(memcmp(&orig, &decoded, sizeof(struct profile)) == 0);
}

/* ---- v9: encoded length is exactly 1384 chars (ceil(1038/3)*4) ---- */
static void t_v4_encoded_length(void)
{
    struct profile p = make_full_profile();
    char b64[1400];
    int enc_len = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(enc_len == 1384);
    assert((int)sizeof(struct profile) == 1038);
}

/* ---- v3: button_key/button_mod survive a round-trip ---- */
static void t_v3_keymap_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[1400];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 1384);
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
    char b64[1400];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 1384);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        assert(decoded.button_key_shift[i] == (uint8_t)(0x20 + i));
        assert(decoded.button_mod_shift[i] == (uint8_t)((i + 8) & 0x0F));
    }
}

/* v9 full image is 1038 bytes / 1384 chars, validates, and round-trips exactly. */
static void t_v9_wire_len_and_roundtrip(void)
{
    assert(PROFILE_VERSION == 9);
    assert((int)sizeof(struct profile) == 1038);
    struct profile orig = make_parity_v9();
    assert(profile_validate(&orig) == 0);            /* fully-populated AND legal */
    char b64[1500];
    int n = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(n == 1384);                               /* ceil(1038/3)*4, 1038 % 3 == 0 */
    assert(b64[1383] != '=');                        /* no padding */
    struct profile dec; memset(&dec, 0, sizeof dec);
    assert(profile_from_b64(b64, n, &dec) == 0);
    assert(memcmp(&orig, &dec, sizeof orig) == 0);
    /* spot the v9 offsets (spec 2.3): layer@118, ext@304, chord6@570,
     * fader_role@1002, chord_flags@1034 */
    assert(dec.layer[0].fader_cc[0]  == 18);         /* L2 fcc[0] = (10+2*4+0)&0x7F, off 118 */
    assert(dec.ext[0].fader_min[0]   == 1);          /* L1 ext fmin[0] = (1+0),     off 304 */
    assert(dec.chord6[0][1].b[0]     == 0x03);       /*                              off 576 */
    assert(dec.fader_role[0][0]      == 1);          /*                              off 1002 */
    assert(dec.chord_flags[0]        == 100);        /*                              off 1034 */
}

/* ---- v9: profile_validate chord6 + fader_role + velocity edge cases ---- */
static void t_v9_validate(void)
{
    struct profile p = make_full_profile();
    assert(profile_validate(&p) == 0);                 /* baseline valid */

    /* a BTN_CHORD button type is accepted (v7 raised the bound to 6; v8/v9 keep it) */
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

/* Feature 1: a cc_value button validates with a legal reused slot; the reserved
   bytes + sub range are locked; an existing all-zero non-chord slot still passes. */
static void t_ccval_validate(void)
{
    struct profile p = make_full_profile();
    /* Put a cc_value button on L0 idx3 and on ext layer L3 (ext[2]) idx4. */
    p.button[3].type = BTN_CC_VALUE;
    p.chord6[0][3] = (struct chord6){ .b = { 0, 9, 45, 0, 0, 0 } };   /* set-on-press */
    p.ext[2].button_type[4] = BTN_CC_VALUE;
    p.chord6[3][4] = (struct chord6){ .b = { 0, 9, 45, 2, 0, 0 } };   /* toggle */
    assert(profile_validate(&p) == 0);                                /* (a) legal passes */

    struct profile q;
    q = p; q.chord6[0][3].b[0] = 1;   assert(profile_validate(&q) == -1); /* reserved b[0] */
    q = p; q.chord6[0][3].b[4] = 1;   assert(profile_validate(&q) == -1); /* reserved b[4] */
    q = p; q.chord6[0][3].b[5] = 1;   assert(profile_validate(&q) == -1); /* reserved b[5] */
    q = p; q.chord6[0][3].b[3] = 3;   assert(profile_validate(&q) == -1); /* sub > 2       */
    q = p; q.chord6[0][3].b[1] = 128; assert(profile_validate(&q) == -1); /* on  > 127     */
    q = p; q.chord6[0][3].b[2] = 128; assert(profile_validate(&q) == -1); /* off > 127     */
    /* (c) an all-zero slot on a plain (non-chord, non-ccval) button still passes */
    q = p; q.button[0].type = BTN_NOTE; memset(q.chord6[0][0].b, 0, 6);
    assert(profile_validate(&q) == 0);
}

/* ---- v5: the appended L3..L8 banks survive a round-trip ---- */
static void t_v5_extra_layers_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[1400];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 1384);
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

/* ---- v6: the appended per-layer ext banks (L2..L8) survive a round-trip ---- */
static void t_v6_ext_layers_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[1400];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 1384);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            assert(decoded.ext[L].fader_min[i]     == (uint8_t)(L * 4 + i));
            assert(decoded.ext[L].fader_max[i]     == (uint8_t)((110 + L + i) & 0x7F));
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

/* Authoritative v9 cross-repo byte-parity golden (1384 chars, no '=' padding).
 * FIRMWARE IS AUTHORITATIVE: sp1ctl.py run_selftest + codec.test.ts pin this exact
 * string. Regenerate only by re-deriving make_parity_v9 (spec 7.3, P2). */
#define PARITY_V9_B64 \
    "CQUKAGQAAAsBZQEBDAJmAgANA2cAAQAUARUCFgMXBBgFGQAaARsCHA4PEBEdHh8gISIjJCVPUC1YWSA4bGF5ZXIAAAAAAAECAwECAwQFBgcICQQFBgcICQoLDAABAgMEBQYHCAcICQoLDA0ODwECAwQFBgcICRITFBUmJygpKissLS4KCwwNDg8QERICAwQFBgcICQoWFxgZLzAxMjM0NTY3DQ4PEBESExQVAwQFBgcICQoLGhscHTg5Ojs8PT4/QBAREhMUFRYXGAQFBgcICQoLDB4fICFBQkNERUZHSEkTFBUWFxgZGhsFBgcICQoLDA0iIyQlSktMTU5PUFFSFhcYGR" \
    "obHB0eBgcICQoLDA0OJicoKVNUVVZXWFlaWxkaGxwdHh8gIQcICQoLDA0ODwECAwRlZmdoAQIAAQEAAQABAgMEBQABAgMBAgMEAgMEBQYHCAkKAgMEBWZnaGkCAAECAAEAAQIDBAUAAQIDBAIDBAUDBAUGBwgJCgsDBAUGZ2hpagABAgABAAEAAwQFAAECAwQFAwQFBgQFBgcICQoLDAQFBgdoaWprAQIAAQABAAEEBQABAgMEBQAEBQYHBQYHCAkKCwwNBQYHCGlqa2wCAAECAQABAAUAAQIDBAUAAQUGBwgGBwgJCgsMDQ4GBwgJamtsbQABAgAAAQABAAECAwQFAAEC" \
    "BgcICQcICQoLDA0ODwcICQprbG1uAQIAAQEAAQABAgMEBQABAgMHCAkKCAkKCwwNDg8AAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAyQoKwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAABAAAAAAEAAAAAAQEAAAAAAQAAAAABAAAAAAFkAAAA"

static void t_v9_golden_parity(void)
{
    struct profile p = make_parity_v9();
    char b64[1500];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);
    assert(strcmp(b64, PARITY_V9_B64) == 0);         /* byte-identical to sp1ctl.py + codec.ts */
    assert(b64[1383] != '=');
}

/* flipping one char IN THE TAIL (L4..L7 / chord region, index > 500) breaks parity */
static void t_v9_golden_is_load_bearing(void)
{
    struct profile p = make_parity_v9();
    char b64[1500]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(strcmp(b64, PARITY_V9_B64) == 0);
    char broken[1500]; memcpy(broken, PARITY_V9_B64, (size_t)n + 1);
    broken[900] = (broken[900] == 'A') ? 'B' : 'A';
    assert(strcmp(b64, broken) != 0);
}

/* Feature 1 golden fixture (heterogeneous per-layer storage): two cc_value buttons
 * placed on indices that hold CHORDS on OTHER layers, proving cc_value and chords
 * never collide on shared button indices. C is the reconciliation source; Python +
 * TS pin PARITY_V9_CCVAL_B64 to whatever this encodes.
 *   L0 idx3: button[3].type = BTN_CC_VALUE, chord6[0][3] = {0,9,45,0,0,0} set-on-press
 *   L3 idx4: ext[2].button_type[4] = BTN_CC_VALUE, chord6[3][4] = {0,9,45,2,0,0} toggle
 * Index 3 holds a chord on L2 (chord6[2][3]); index 4 holds a chord on L5 (chord6[5][4]). */
static struct profile make_parity_v9_ccval(void)
{
    struct profile p = make_parity_v9();
    p.button[3].type        = BTN_CC_VALUE;                 /* L0 idx3 set-on-press */
    p.chord6[0][3]          = (struct chord6){ .b = { 0, 9, 45, 0, 0, 0 } };
    p.ext[2].button_type[4] = BTN_CC_VALUE;                 /* L3 idx4 toggle */
    p.chord6[3][4]          = (struct chord6){ .b = { 0, 9, 45, 2, 0, 0 } };
    return p;
}
static void t_v9_ccval_round_trip(void)
{
    struct profile p = make_parity_v9_ccval();
    assert(profile_validate(&p) == 0);                      /* validate-clean */
    char b64[1500]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);                                      /* no format change */
    struct profile d; memset(&d, 0, sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(memcmp(&p, &d, sizeof p) == 0);                  /* exact round-trip */
    /* the two cc_value slots survive; the collision-index chords survive */
    assert(d.chord6[0][3].b[0]==0 && d.chord6[0][3].b[1]==9 && d.chord6[0][3].b[2]==45 && d.chord6[0][3].b[3]==0);
    assert(d.chord6[3][4].b[0]==0 && d.chord6[3][4].b[1]==9 && d.chord6[3][4].b[2]==45 && d.chord6[3][4].b[3]==2);
    assert(d.chord6[2][3].b[0]==(2<<5));                    /* L2 idx3 still Cmin7 chord */
    assert(d.chord6[5][4].b[0]==0x03);                      /* L5 idx4 still explicit chord */
    assert(d.button[3].type==BTN_CC_VALUE && d.ext[2].button_type[4]==BTN_CC_VALUE);
}

/* Authoritative Feature 1 golden (1384 chars, no '=' padding). FIRMWARE IS
 * AUTHORITATIVE: sp1ctl.py run_selftest + codec.test.ts pin this exact string. */
#define PARITY_V9_CCVAL_B64 \
    "CQUKAGQAAAsBZQEBDAJmAgANA2cAAQAUARUCFgcXBBgFGQAaARsCHA4PEBEdHh8gISIjJCVPUC1YWSA4bGF5ZXIAAA" \
    "AAAAECAwECAwQFBgcICQQFBgcICQoLDAABAgMEBQYHCAcICQoLDA0ODwECAwQFBgcICRITFBUmJygpKissLS4KCwwN" \
    "Dg8QERICAwQFBgcICQoWFxgZLzAxMjM0NTY3DQ4PEBESExQVAwQFBgcICQoLGhscHTg5Ojs8PT4/QBAREhMUFRYXGA" \
    "QFBgcICQoLDB4fICFBQkNERUZHSEkTFBUWFxgZGhsFBgcICQoLDA0iIyQlSktMTU5PUFFSFhcYGRobHB0eBgcICQoL" \
    "DA0OJicoKVNUVVZXWFlaWxkaGxwdHh8gIQcICQoLDA0ODwECAwRlZmdoAQIAAQEAAQABAgMEBQABAgMBAgMEAgMEBQ" \
    "YHCAkKAgMEBWZnaGkCAAECAAEAAQIDBAUAAQIDBAIDBAUDBAUGBwgJCgsDBAUGZ2hpagABAgABAAEAAwQFAAcCAwQF" \
    "AwQFBgQFBgcICQoLDAQFBgdoaWprAQIAAQABAAEEBQABAgMEBQAEBQYHBQYHCAkKCwwNBQYHCGlqa2wCAAECAQABAA" \
    "UAAQIDBAUAAQUGBwgGBwgJCgsMDQ4GBwgJamtsbQABAgAAAQABAAECAwQFAAECBgcICQcICQoLDA0ODwcICQprbG1u" \
    "AQIAAQEAAQABAgMEBQABAgMHCAkKCAkKCwwNDg8AAAAAAAAAAzxAQwAAAAAAAAAAAAktAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAktAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAyQoKwAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAABAAAAAA" \
    "EAAAAAAQEAAAAAAQAAAAABAAAAAAFkAAAA"

static void t_v9_ccval_golden(void)
{
    struct profile p = make_parity_v9_ccval();
    char b64[1500]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);
    assert(strcmp(b64, PARITY_V9_CCVAL_B64) == 0);   /* byte-identical to sp1ctl.py + codec.ts */
    assert(b64[1383] != '=');
    /* offset asserts (spec Section 7): slot at 570 + (L*9+i)*6 */
    struct profile d; memset(&d,0,sizeof d); assert(profile_from_b64(b64,n,&d)==0);
    assert(d.button[3].type == BTN_CC_VALUE);         /* offset 28 */
    assert(d.ext[2].button_type[4] == BTN_CC_VALUE);  /* offset 400 */
}

/* Frozen 528-byte v8 blob (verbatim the retired PARITY_V8_B64). The input to the
 * v8 -> v9 migration; Python/TS decode THIS via the frozen 4-layer path. */
#define PARITY_V8_LEGACY_SRC_B64 \
    "CAUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAw" \
    "QFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIE" \
    "CAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAUKD394ZFoAAQIAAAEAAQECAwQFAAECAwECAwQAAQIDBAUGBwgBAg" \
    "MEbm9wcQECAAEBAAEAAgMEBQABAgMEBQYHCAkKCwwNDg8AARQABwB/QFBgAgABAgEBAAADBAUAAQIDBAUJCgsMAgMEBQYHCAkK" \
    "AAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAA" \
    "ABAAAAAAEAAAAAAWQA"

/* Hand-authored v8 -> v9 upconvert reference: the four frozen v8 layers land in v9
 * L0..L3 (0-indexed), L4..L7 stay empty. Same values the retired make_parity_v8
 * carried (spec 2.3: v8 [0..179] is byte-identical to v9 [0..179]; divergence at 180). */
static struct profile make_v8_upconvert_v9(void)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;                     /* stamped v9 */
    p.channel = 5;
    memcpy(p.name, "OP-XY mix", 9);
    /* L0 (v8 inline) */
    { static const uint8_t fcc[]={7,74,71,76}, fmin[]={0,10,0,5}, fmax[]={127,120,100,127},
        fcur[]={0,1,2,0}, finv[]={0,1,0,1}, fch[]={0,1,2,3},
        btype[]={1,2,3,4,5,0,1,2,3}, bval[]={60,64,65,1,2,0,62,80,81}, bch[]={4,5,6,7,8,9,10,11,12},
        bkey[]={0x04,0x05,0x28,0x2c,0x2b,0x29,0x50,0x4f,0x00}, bmod[]={0x01,0x02,0x00,0x04,0x08,0x05,0,0,0};
      v9_set_layer(&p,0,fcc,bval,bkey,bmod,fmin,fmax,fcur,finv,btype,fch,bch); }
    /* L1 (v8 shift + ext[0]) */
    { static const uint8_t fcc[]={20,21,22,23}, fmin[]={0,5,10,15}, fmax[]={127,120,100,90},
        fcur[]={0,1,2,0}, finv[]={0,1,0,1}, fch[]={1,2,3,4},
        btype[]={1,2,3,4,5,0,1,2,3}, bval[]={30,31,32,33,34,35,36,37,38}, bch[]={0,1,2,3,4,5,6,7,8},
        bkey[]={0x00,0x06,0x07,0x09,0x0a,0x0b,0x4a,0x4d,0x00}, bmod[]={0x00,0x08,0x08,0x01,0x01,0x05,0,0,0};
      v9_set_layer(&p,1,fcc,bval,bkey,bmod,fmin,fmax,fcur,finv,btype,fch,bch); }
    /* L2 (v8 layer[0] + ext[1]) */
    { static const uint8_t fcc[]={40,41,42,43}, fmin[]={1,2,3,4}, fmax[]={110,111,112,113},
        fcur[]={1,2,0,1}, finv[]={1,0,1,0}, fch[]={5,6,7,8},
        btype[]={2,3,4,5,0,1,2,3,4}, bval[]={60,61,62,63,64,65,66,67,68}, bch[]={9,10,11,12,13,14,15,0,1},
        bkey[]={0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c}, bmod[]={0x01,0x02,0x04,0x08,0x01,0x02,0x04,0x08,0x00};
      v9_set_layer(&p,2,fcc,bval,bkey,bmod,fmin,fmax,fcur,finv,btype,fch,bch); }
    /* L3 (v8 layer[1] + ext[2]) */
    { static const uint8_t fcc[]={50,51,52,53}, fmin[]={20,0,7,0}, fmax[]={127,64,80,96},
        fcur[]={2,0,1,2}, finv[]={1,1,0,0}, fch[]={9,10,11,12},
        btype[]={3,4,5,0,1,2,3,4,5}, bval[]={70,71,72,73,74,75,76,77,78}, bch[]={2,3,4,5,6,7,8,9,10},
        bkey[]={0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26}, bmod[]={0x08,0x04,0x02,0x01,0x08,0x04,0x02,0x01,0x00};
      v9_set_layer(&p,3,fcc,bval,bkey,bmod,fmin,fmax,fcur,finv,btype,fch,bch); }
    /* v8 chord6 diagonal (L0..L3 only); L4..L7 stay zero */
    p.chord6[0][1] = (struct chord6){ .b = { 0x03,   60, 64, 67, 0, 0 } };
    p.chord6[1][2] = (struct chord6){ .b = { (1<<5), 21, 7,  0, 0, 0 } };
    p.chord6[2][3] = (struct chord6){ .b = { (2<<5), 48, 5,  0, 0, 0 } };
    p.chord6[3][4] = (struct chord6){ .b = { 0x03,   60, 64, 67, 0, 0 } };
    for (int L = 0; L < 4; L++) p.fader_role[L][L % NUM_FADERS] = 1;   /* v8 filled only L0..L3 */
    p.chord_flags[0] = 100;
    return p;
}

#define PARITY_V8_LEGACY_B64 \
    "CQUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAwQFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIECAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFCg9/eGRaAAECAAABAAEBAgMEBQABAgMBAgMEAAECAwQFBgcIAQIDBG5vcHEBAgABAQABAAIDBAUAAQIDBAUGBwgJCgsMDQ4PAAEUAAcAf0BQYAIAAQIBAQAAAwQFAAECAwQFCQoLDAIDBAUGBwgJCgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAA" \
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAABAAAAAAEAAAAAAQAAAAAAAAAAAAAAAAAAAABkAAAA"

static void t_v8_legacy_upconvert_golden(void)
{
    struct profile p = make_v8_upconvert_v9();
    assert(profile_validate(&p) == 0);
    char b64[1500];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);
    assert(strcmp(b64, PARITY_V8_LEGACY_B64) == 0);      /* byte-identical to sp1ctl.py + codec.ts */
    /* L1..L4 populated from v8, L5..L8 empty; divergence at byte 180 (spec 2.3) */
    assert(p.fader[0].cc          == 7);    /* L0 inline (v8),          off 2   */
    assert(p.layer[0].fader_cc[0] == 40);   /* L2 = v8 layer[0],        off 118 */
    assert(p.layer[1].fader_cc[0] == 50);   /* L3 = v8 layer[1],        off 149 */
    assert(p.layer[2].fader_cc[0] == 0);    /* L4 EMPTY,                off 180 (divergence) */
    assert(p.ext[0].fader_min[1]  == 5);    /* L1 ext carries v8,       off 305 */
    assert(p.ext[3].fader_min[0]  == 0);    /* L4 ext EMPTY,            off 418 */
    assert(p.chord6[3][4].b[0]    == 0x03); /* v8 diagonal last chord              */
    assert(p.chord6[4][0].b[0]    == 0);    /* L5 chord EMPTY                       */
    assert(p.fader_role[4][0]     == 0);    /* L5 role EMPTY                        */
    assert(p.chord_flags[0]       == 100);
}

/* ---- v9: chord tail survives a round-trip and unpacks to a usable chord_def ---- */
static void t_v9_chord_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[1400]; int n = profile_to_b64(&orig, b64, (int)sizeof b64);
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

/* ---- v9: a legacy (v7) blob is rejected by validate ---- */
static void t_v9_rejects_legacy_blob(void)
{
    struct profile p = make_full_profile();
    p.version = 7;                 /* a legacy-firmware blob reaching v9 firmware */
    assert(profile_validate(&p) == -1);
}

/* ---- v4: a v3 blob (version byte 3) is rejected by validate ---- */
static void t_v4_rejects_v3_blob(void)
{
    struct profile p = make_full_profile();
    p.version = 3;                 /* a v3-firmware blob reaching v9 firmware */
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

/* ---- validate: rejects button.type > BTN_CC_VALUE (F1 raised the bound to 7) ---- */
static void t_validate_bad_button_type(void)
{
    struct profile p = make_full_profile();
    p.button[3].type = 8;            /* one past BTN_CC_VALUE */
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

/* ---- validate: rejects a MIDI d1 (button value) or appended-layer fader CC > 127.
 *      These fields were previously unchecked, so a seed-formula overflow (spec 3.6)
 *      shipped a malformed CC silently. ---- */
static void t_validate_bad_button_value(void)
{
    struct profile p;
    p = make_full_profile(); p.button[2].value = 200;         /* L1 button d1 */
    assert(profile_validate(&p) == -1);
    p = make_full_profile(); p.shift.button_value[0] = 128;   /* L2 button d1 */
    assert(profile_validate(&p) == -1);
    p = make_full_profile(); p.shift.fader_cc[3] = 128;       /* L2 fader CC */
    assert(profile_validate(&p) == -1);
    p = make_full_profile(); p.layer[5].button_value[1] = 200;/* L8 button d1 */
    assert(profile_validate(&p) == -1);
    p = make_full_profile(); p.layer[3].fader_cc[0] = 128;    /* L6 fader CC */
    assert(profile_validate(&p) == -1);
    /* baseline is still accepted (make_full_profile keeps every value <= 127) */
    p = make_full_profile();
    assert(profile_validate(&p) == 0);
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
    p.ext[0].button_type[3] = 8;             /* > BTN_CC_VALUE (F1 raised the bound to 7) */
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

/* ============================================================================
 * make_default RESEED (Phase 8, spec 3.6) — hand-mirror of librarian.c's
 * make_default(). librarian.c pulls in Zephyr and is NOT host-buildable, so the
 * seed layout is exercised through this mirror, which MUST track make_default
 * exactly (edited in lockstep). mirror_default(within) reproduces ONLY the
 * generic (non-TE-seed) path: the 8-layer 7-bit-masked fader/button loops + the
 * ext-inheritance loop + chord velocity 100.
 * ==========================================================================*/

/* Mirror of librarian.c default_btn_channel: FRONT track buttons (idx 1..4) ->
 * MIDI ch2 (channel 1), SIDE buttons (idx 5..8) -> ch3 (channel 2), PLAY (idx 0)
 * stays on the profile channel. */
static uint8_t md_btn_channel(int i)
{
    if (i >= 1 && i <= 4) return 1;   /* front -> ch2 */
    if (i >= 5 && i <= 8) return 2;   /* side  -> ch3 */
    return 0;                          /* PLAY  -> profile channel */
}

/* Mirror of librarian.c default_btn_value: within*32 + L*4 + slot, 7-bit-masked
 * so no default button d1 overflows 127 (profile_validate does not range-check
 * a button d1 before Phase 8's validate hardening). */
static uint8_t md_btn_value(int within, int L, int i)
{
    int base = within * (NUM_LAYERS * NUM_FADERS);   /* within*32 */
    if (i >= 1 && i <= 4) return (uint8_t)((base + L * NUM_FADERS + (i - 1)) & 0x7F); /* front slot 0..3 */
    if (i >= 5 && i <= 8) return (uint8_t)((base + L * NUM_FADERS + (i - 5)) & 0x7F); /* side  slot 0..3 */
    return 0;                                                                          /* PLAY */
}

static struct profile mirror_default(int within)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;
    p.channel = 0;
    int fbase = within * (NUM_LAYERS * NUM_FADERS);   /* within*32 (0,32,...,224) */
    for (int L = 0; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            uint8_t cc = (uint8_t)((fbase + L * NUM_FADERS + i) & 0x7F);
            if (L == 0) {
                p.fader[i].cc = cc; p.fader[i].min = 0; p.fader[i].max = 127;
                p.fader[i].curve = CURVE_LINEAR; p.fader[i].invert = 0; p.fader_channel[i] = 0;
            } else if (L == 1) {
                p.shift.fader_cc[i] = cc;
            } else {
                p.layer[L - 2].fader_cc[i] = cc;
            }
        }
    }
    for (int L = 0; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            uint8_t bv = md_btn_value(within, L, i);
            if (L == 0) {
                p.button[i].type = BTN_CC_MOMENTARY; p.button[i].value = bv;
                p.button_channel[i] = md_btn_channel(i);
            } else if (L == 1) {
                p.shift.button_value[i] = bv;
            } else {
                p.layer[L - 2].button_value[i] = bv;
            }
            /* keyboard layers default unbound: button_key/mod all 0 (memset above) */
        }
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

/* Return the 4 fader CCs of layer L (0=inline,1=shift,2..7=layer[L-2]) into out[4]. */
static void layer_fader_ccs(const struct profile *p, int L, uint8_t out[NUM_FADERS])
{
    for (int i = 0; i < NUM_FADERS; i++) {
        if (L == 0)      out[i] = p->fader[i].cc;
        else if (L == 1) out[i] = p->shift.fader_cc[i];
        else             out[i] = p->layer[L - 2].fader_cc[i];
    }
}

/* Return the 9 button values of layer L (0=inline,1=shift,2..7=layer[L-2]) into out[9]. */
static void layer_button_values(const struct profile *p, int L, uint8_t out[NUM_BUTTONS])
{
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (L == 0)      out[i] = p->button[i].value;
        else if (L == 1) out[i] = p->shift.button_value[i];
        else             out[i] = p->layer[L - 2].button_value[i];
    }
}

static void t_v6_default_seed_layout(void)
{
    /* ---- pin the v9 masked fader-CC layout at the corners ---- */
    struct profile p0 = mirror_default(0);           /* within 0, L0: {0,1,2,3} */
    uint8_t l[NUM_FADERS];
    layer_fader_ccs(&p0, 0, l);
    assert(l[0] == 0 && l[1] == 1 && l[2] == 2 && l[3] == 3);
    struct profile p1 = mirror_default(1);           /* within 1, L0: within*32 = {32,33,34,35} */
    layer_fader_ccs(&p1, 0, l);
    assert(l[0] == 32 && l[1] == 33 && l[2] == 34 && l[3] == 35);
    struct profile p7 = mirror_default(7);           /* within 7, L7: 224+28+{0..3} masked = {124,125,126,127} */
    layer_fader_ccs(&p7, 7, l);
    assert(l[0] == 124 && l[1] == 125 && l[2] == 126 && l[3] == 127);

    /* ---- v9: 256 default fader CCs (8 profiles x 8 layers x 4 faders). The masked
     *      running counter within*32 + L*4 + F covers 0..255 uniquely, so &0x7F yields
     *      EACH CC 0..127 exactly TWICE; all in range, min 0, max 127; and NO layer is
     *      degenerate all-CC0 (guards the old L5-L8 memset-0 bug). ---- */
    int seen[128] = {0};
    int max_cc = -1, count = 0;
    for (int P = 0; P < NUM_BANK_PROFILES; P++) {
        struct profile pp = mirror_default(P);
        for (int L = 0; L < NUM_LAYERS; L++) {
            uint8_t ccs[NUM_FADERS];
            layer_fader_ccs(&pp, L, ccs);
            int all_zero = 1;
            for (int i = 0; i < NUM_FADERS; i++) {
                assert(ccs[i] <= 127);
                seen[ccs[i]]++;
                if (ccs[i] != 0) all_zero = 0;
                if (ccs[i] > max_cc) max_cc = ccs[i];
                count++;
            }
            assert(!all_zero);                        /* no degenerate layer, incl. L5-L8 */
        }
    }
    assert(count == 256);                             /* 8*8*4 fader slots */
    assert(max_cc == 127);
    for (int v = 0; v < 128; v++) assert(seen[v] == 2); /* each CC used exactly twice */

    /* buttons: FRONT (idx1..4) on ch2, SIDE (idx5..8) on ch3, PLAY(0) inert on ch0;
     * every layer inherits the channel scheme. All values <= 127 (masked). */
    for (int P = 0; P < NUM_BANK_PROFILES; P++) {
        struct profile pp = mirror_default(P);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            uint8_t want_ch = (i >= 1 && i <= 4) ? 1 : (i >= 5 && i <= 8) ? 2 : 0;
            assert(pp.button_channel[i] == want_ch);
            for (int L = 0; L < NUM_LAYERS - 1; L++)
                assert(pp.ext[L].button_channel[i] == want_ch); /* every layer inherits */
        }
        assert(pp.button[0].value == 0);                        /* PLAY emits nothing */
    }
    /* the 256 FRONT-button CCs (idx1..4 over 8 profiles x 8 layers) cover 0..127 exactly
     * twice; likewise the 256 SIDE-button CCs (idx5..8). Same masked counter as faders. */
    {
        int seen_front[128] = {0}, seen_side[128] = {0};
        int cf = 0, cs = 0;
        for (int P = 0; P < NUM_BANK_PROFILES; P++) {
            struct profile pp = mirror_default(P);
            for (int L = 0; L < NUM_LAYERS; L++) {
                uint8_t bv[NUM_BUTTONS];
                layer_button_values(&pp, L, bv);
                for (int i = 1; i <= 4; i++) { assert(bv[i] <= 127); seen_front[bv[i]]++; cf++; }
                for (int i = 5; i <= 8; i++) { assert(bv[i] <= 127); seen_side[bv[i]]++;  cs++; }
            }
        }
        assert(cf == 256 && cs == 256);
        for (int v = 0; v < 128; v++) { assert(seen_front[v] == 2); assert(seen_side[v] == 2); }
    }

    /* ---- the default profile encodes to exactly 1038B/1384ch and round-trips,
     *      validates, and keeps every invariant (slot 0 as the representative) ---- */
    struct profile p = mirror_default(0);
    assert(profile_validate(&p) == 0);
    char b64[1400];
    int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);
    assert((int)sizeof(struct profile) == 1038);
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
     * (127) and L1's curve/range, so all layers look like L1 out of the box. */
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
    /* still 1038 B / 1384 ch and round-trips */
    char b64[1400]; int n = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(n == 1384);
    struct profile d; memset(&d,0,sizeof d);
    assert(profile_from_b64(b64, n, &d) == 0);
    assert(memcmp(&p, &d, sizeof p) == 0);
}

/* ============================================================================
 * OP-XY 8-track seed (Phase 8, spec 8.3/8.4). Mirrors librarian.c SEED_OPXY8 +
 * set_layer + apply_te_seed8. L1..L8 = tracks 1..8 on MIDI ch 0..7.
 * ==========================================================================*/

/* OP-XY 8-track constants - kept byte-identical to librarian.c SEED_OPXY8. */
static const uint8_t OPXY8_FCC[NUM_FADERS]    = { 32, 33, 31, 38 };
static const uint8_t OPXY8_BTYPE[NUM_BUTTONS] =
    { BTN_NONE, BTN_CC_TOGGLE, BTN_CC_MOMENTARY, BTN_CC_TOGGLE, BTN_CC_MOMENTARY,
      BTN_CC_MOMENTARY, BTN_CC_MOMENTARY, BTN_CC_MOMENTARY, BTN_CC_MOMENTARY };
static const uint8_t OPXY8_BVAL[NUM_BUTTONS]  = { 0, 9, 37, 29, 39, 83, 84, 104, 105 };

/* Mirror of librarian.c set_layer: heterogeneous per-layer storage behind one call. */
static void md_set_layer(struct profile *p, int L, uint8_t chan)
{
    for (int i = 0; i < NUM_FADERS; i++) {
        if (L == 0) { p->fader[i].cc=OPXY8_FCC[i]; p->fader[i].min=0; p->fader[i].max=127;
            p->fader[i].curve=CURVE_LINEAR; p->fader[i].invert=0; p->fader_channel[i]=chan; }
        else if (L == 1) { p->shift.fader_cc[i]=OPXY8_FCC[i]; p->ext[0].fader_min[i]=0;
            p->ext[0].fader_max[i]=127; p->ext[0].fader_curve[i]=CURVE_LINEAR;
            p->ext[0].fader_invert[i]=0; p->ext[0].fader_channel[i]=chan; }
        else { p->layer[L-2].fader_cc[i]=OPXY8_FCC[i]; p->ext[L-1].fader_min[i]=0;
            p->ext[L-1].fader_max[i]=127; p->ext[L-1].fader_curve[i]=CURVE_LINEAR;
            p->ext[L-1].fader_invert[i]=0; p->ext[L-1].fader_channel[i]=chan; }
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (L == 0) { p->button[i].type=OPXY8_BTYPE[i]; p->button[i].value=OPXY8_BVAL[i];
            p->button_channel[i]=chan; }
        else if (L == 1) { p->shift.button_value[i]=OPXY8_BVAL[i];
            p->ext[0].button_type[i]=OPXY8_BTYPE[i]; p->ext[0].button_channel[i]=chan; }
        else { p->layer[L-2].button_value[i]=OPXY8_BVAL[i];
            p->ext[L-1].button_type[i]=OPXY8_BTYPE[i]; p->ext[L-1].button_channel[i]=chan; }
    }
}

/* Mirror of make_default(slot==1), CORRECT order: generic 8-layer seed + ext
 * inheritance (via mirror_default), THEN apply_te_seed8 so its per-layer ext[]
 * channels 0..7 are not clobbered back to ch0. */
static struct profile mirror_opxy8(void)
{
    struct profile p = mirror_default(1);              /* generic seed + inheritance */
    p.channel = 0;
    for (int L = 0; L < NUM_LAYERS; L++)               /* apply_te_seed8: AFTER inheritance */
        md_set_layer(&p, L, (uint8_t)L);
    return p;
}

static void t_opxy8_seed_cc_map(void)
{
    struct profile p = mirror_opxy8();
    assert(profile_validate(&p) == 0);
    for (int L = 0; L < NUM_LAYERS; L++) {              /* fader CCs identical on all 8 layers */
        uint8_t f[NUM_FADERS];
        layer_fader_ccs(&p, L, f);
        assert(f[0]==32 && f[1]==33 && f[2]==31 && f[3]==38);   /* cutoff/res/engine-vol/FX-I */
        uint8_t bv[NUM_BUTTONS];
        layer_button_values(&p, L, bv);
        for (int i = 0; i < NUM_BUTTONS; i++) assert(bv[i] == OPXY8_BVAL[i]);
    }
    /* inline (L1/track1) button types: T2 = send-to-tape CC37 momentary (opxy.md:54),
     * NOT the removed dead scene CC85 */
    assert(p.button[0].type == BTN_NONE);                                 /* PLAY gesture */
    assert(p.button[1].type == BTN_CC_TOGGLE   && p.button[1].value == 9);  /* T1 mute */
    assert(p.button[2].type == BTN_CC_MOMENTARY&& p.button[2].value == 37); /* T2 tape */
    assert(p.button[3].type == BTN_CC_TOGGLE   && p.button[3].value == 29); /* T3 porta */
    assert(p.button[4].type == BTN_CC_MOMENTARY&& p.button[4].value == 39); /* T4 FX-II */
}

static void t_opxy8_per_track_channels_survive_inheritance(void)
{
    struct profile p = mirror_opxy8();
    /* Every control on layer L must ride channel L (track L+1): L0..L7 -> ch0..7.
     * Holds ONLY if apply_te_seed8 runs AFTER the ext-inheritance loop. */
    for (int i = 0; i < NUM_FADERS; i++)  assert(p.fader_channel[i]  == 0);   /* L0 = ch0 */
    for (int i = 0; i < NUM_BUTTONS; i++) assert(p.button_channel[i] == 0);
    for (int L = 1; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_FADERS; i++)
            assert(p.ext[L-1].fader_channel[i]  == (uint8_t)L);               /* ch = L = track */
        for (int i = 0; i < NUM_BUTTONS; i++)
            assert(p.ext[L-1].button_channel[i] == (uint8_t)L);
        assert(p.ext[L-1].button_type[1] == BTN_CC_TOGGLE);     /* T1 mute per-track survives */
        assert(p.ext[L-1].button_type[2] == BTN_CC_MOMENTARY);  /* T2 tape throw per-track */
    }

    /* Negative guard: seeding BEFORE inheritance clobbers ext channels back to ch0,
     * proving the ordering is load-bearing (documents the trap). */
    struct profile wrong;
    memset(&wrong, 0, sizeof wrong);
    wrong.version = PROFILE_VERSION; wrong.channel = 0;
    for (int L = 0; L < NUM_LAYERS; L++) md_set_layer(&wrong, L, (uint8_t)L);  /* seed */
    for (int L = 0; L < NUM_LAYERS - 1; L++)                                    /* then inherit */
        for (int i = 0; i < NUM_FADERS; i++)
            wrong.ext[L].fader_channel[i] = wrong.fader_channel[i];             /* copies ch0 */
    assert(wrong.ext[6].fader_channel[0] == 0);   /* L8 collapsed to ch0 - the trap */
}

/* Feature 1: the new CC-value button type is enum value 7, one past BTN_CHORD.
 * Feature 1 REUSES the inert chord6 slot (no new struct field), so adding the type
 * must NOT change the profile size: pin sizeof at 1038 (v9, base64 1384, no wipe). */
static void t_ccval_enum_value(void)
{
    assert(BTN_CHORD == 6);
    assert(BTN_CC_VALUE == 7);
    assert(sizeof(struct profile) == 1038);   /* F1 adds no bytes to the profile */
}

int main(void)
{
    t_ccval_enum_value();
    t_v9_geometry();
    t_v9_golden_parity();
    t_v9_golden_is_load_bearing();
    t_v9_ccval_round_trip();
    t_v9_ccval_golden();
    t_v8_legacy_upconvert_golden();
    t_round_trip();
    t_v4_encoded_length();
    t_v3_keymap_round_trip();
    t_v4_shift_keymap_round_trip();
    t_v9_wire_len_and_roundtrip();
    t_v9_validate();
    t_ccval_validate();
    t_v5_extra_layers_round_trip();
    t_v6_ext_layers_round_trip();
    t_v9_chord_round_trip();
    t_v9_rejects_legacy_blob();
    t_v4_rejects_v3_blob();
    t_validate_good();
    t_validate_bad_version();
    t_validate_bad_channel();
    t_validate_bad_fader_cc();
    t_validate_bad_fader_curve();
    t_validate_bad_button_type();
    t_validate_bad_fader_channel();
    t_validate_bad_button_channel();
    t_validate_bad_button_value();
    t_validate_bad_ext_fields();
    t_from_b64_wrong_length();
    t_to_b64_outcap();
    t_from_b64_lying_len();
    t_v6_default_seed_layout();
    t_v8_default_seed_layout();
    t_opxy8_seed_cc_map();
    t_opxy8_per_track_channels_survive_inheritance();
    printf("all profile tests passed\n");
    return 0;
}
