#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "profile.h"

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
    return p;
}

/* ---- round-trip ---- */
static void t_round_trip(void)
{
    struct profile orig = make_full_profile();
    /* Base64 of the packed profile (v4 = 118 bytes -> ceil(118/3)*4 = 40*4 = 160
     * chars + NUL). Buffer derived from sizeof, never hardcoded. */
    char b64[176];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof(b64));
    assert(enc_len > 0);

    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    int rc = profile_from_b64(b64, enc_len, &decoded);
    assert(rc == 0);
    assert(memcmp(&orig, &decoded, sizeof(struct profile)) == 0);
}

/* ---- v4: encoded length is exactly 160 chars (ceil(118/3)*4) ---- */
static void t_v4_encoded_length(void)
{
    struct profile p = make_full_profile();
    char b64[176];
    int enc_len = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(enc_len == 160);
    assert((int)sizeof(struct profile) == 118);
}

/* ---- v3: button_key/button_mod survive a round-trip ---- */
static void t_v3_keymap_round_trip(void)
{
    struct profile orig = make_full_profile();
    char b64[176];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 160);
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
    char b64[176];
    int enc_len = profile_to_b64(&orig, b64, (int)sizeof b64);
    assert(enc_len == 160);
    struct profile decoded;
    memset(&decoded, 0, sizeof decoded);
    assert(profile_from_b64(b64, enc_len, &decoded) == 0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        assert(decoded.button_key_shift[i] == (uint8_t)(0x20 + i));
        assert(decoded.button_mod_shift[i] == (uint8_t)((i + 8) & 0x0F));
    }
}

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

/* ---- validate: rejects button.type > 5 ---- */
static void t_validate_bad_button_type(void)
{
    struct profile p = make_full_profile();
    p.button[3].type = 6;
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

int main(void)
{
    t_round_trip();
    t_v4_encoded_length();
    t_v3_keymap_round_trip();
    t_v4_shift_keymap_round_trip();
    t_v4_rejects_v3_blob();
    t_validate_good();
    t_validate_bad_version();
    t_validate_bad_channel();
    t_validate_bad_fader_cc();
    t_validate_bad_fader_curve();
    t_validate_bad_button_type();
    t_validate_bad_fader_channel();
    t_validate_bad_button_channel();
    t_from_b64_wrong_length();
    t_to_b64_outcap();
    t_from_b64_lying_len();
    printf("all profile tests passed\n");
    return 0;
}
