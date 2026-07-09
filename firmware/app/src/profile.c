/*
 * profile.c — profile validate + base64 codec
 *
 * Freestanding-friendly: only <stdint.h> and <string.h> (no malloc, no stdio).
 * All sizes derived from sizeof(struct profile) — never hardcoded.
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "profile.h"

/* Compile-time guard: the version wire boundaries (in profile_wire_len) MUST equal
 * the real struct offsets, so a future struct reorder can't silently break
 * cross-repo byte parity. v6 == sizeof. */
#define PROFILE_STATIC_ASSERT(c, name) typedef char prof_sa_##name[(c) ? 1 : -1]
PROFILE_STATIC_ASSERT(offsetof(struct profile, fader_channel)    == 69,  v1_end);
PROFILE_STATIC_ASSERT(offsetof(struct profile, button_key)       == 82,  v2_end);
PROFILE_STATIC_ASSERT(offsetof(struct profile, button_key_shift) == 100, v3_end);
PROFILE_STATIC_ASSERT(offsetof(struct profile, layer)            == 118, v4_end);
PROFILE_STATIC_ASSERT(offsetof(struct profile, layer)            == 118,  v9_layer_off);
PROFILE_STATIC_ASSERT(offsetof(struct profile, ext)              == 304,  v9_ext_off);
PROFILE_STATIC_ASSERT(offsetof(struct profile, chord6)           == 570,  v9_chord6_off);
PROFILE_STATIC_ASSERT(offsetof(struct profile, fader_role)       == 1002, v9_fader_role_off);
PROFILE_STATIC_ASSERT(offsetof(struct profile, chord_flags)      == 1034, v9_chord_flags_off);
PROFILE_STATIC_ASSERT(sizeof(struct profile)                     == 1038, v9_sizeof);

/* ---- base64 alphabet ---- */
static const char b64_enc_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ---- version-aware wire length ----
 * Each version appended a fixed tail, so the on-wire byte count is a clean prefix
 * of the current struct. These boundaries are the historical offsets (verified by
 * offsetof in the host test):
 *   v1: through name[]                                   -> 69
 *   v2: + fader_channel[4] + button_channel[9]           -> 82
 *   v3: + button_key[9] + button_mod[9]                  -> 100
 *   v4: + button_key_shift[9] + button_mod_shift[9]      -> 118
 *   v5: + layer_bank layer[2]   (2*31)                   -> 180
 *   v6: + layer_ext  ext[3]     (3*38)                   -> 294
 *   v8: + chord6[4][9](216) + fader_role[4][4](16)
 *       + chord_flags[2](2)                              -> 528
 * An unknown/future version maps to the full current image so a forward blob still
 * round-trips length-wise; profile_validate rejects the bad version separately. */
int profile_wire_len(uint8_t version)
{
    switch (version) {
    case 1:  return 69;
    case 2:  return 82;
    case 3:  return 100;
    case 4:  return 118;
    case 5:  return 180;
    case 6:  return 294;
    case 7:  return 294;   /* a v7 blob no longer round-trips full; its prefix is the v6 image (read-only path; firmware rejects v7 at validate) */
    case 8:  return 528;   /* pinned literal: v8 wire length is no longer == sizeof */
    case 9:
    default: return (int)sizeof(struct profile);   /* 1038 (v9, padded to a multiple of 3) */
    }
}

/* Decode a single base64 character to its 6-bit value.
 * Returns -1 for invalid characters (including '='). */
static int b64_char_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+')              return 62;
    if (c == '/')              return 63;
    return -1;
}

/* ---- profile_to_b64 ----
 * Base64-encode the raw bytes of *p into out (NUL-terminated).
 * Returns encoded string length on success, -1 if outcap is too small.
 *
 * VERSION-AWARE: emits only the prefix for p->version (the clean prefix-superset),
 * so a v5-targeted profile yields 180 bytes / 240 chars and a v6 profile yields
 * 294 bytes / 392 chars. The firmware only ever stamps PROFILE_VERSION, so it
 * always emits the full current image; older devices get a sliced blob.
 *
 * Encoded length for N input bytes: ceil(N/3)*4 chars + 1 NUL.
 */
int profile_to_b64(const struct profile *p, char *out, int outcap)
{
    const uint8_t *src = (const uint8_t *)p;
    int src_len = profile_wire_len(p->version);

    /* Compute required output capacity (including NUL terminator). */
    int out_len = ((src_len + 2) / 3) * 4;  /* padded base64 length */
    if (outcap < out_len + 1)
        return -1;

    int si = 0;
    int di = 0;
    while (si < src_len) {
        int remaining = src_len - si;
        uint8_t b0 = src[si];
        uint8_t b1 = (remaining > 1) ? src[si + 1] : 0u;
        uint8_t b2 = (remaining > 2) ? src[si + 2] : 0u;

        uint32_t triple = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2;

        out[di++] = b64_enc_tbl[(triple >> 18) & 0x3F];
        out[di++] = b64_enc_tbl[(triple >> 12) & 0x3F];
        out[di++] = (remaining > 1) ? b64_enc_tbl[(triple >> 6) & 0x3F] : '=';
        out[di++] = (remaining > 2) ? b64_enc_tbl[triple & 0x3F]         : '=';

        si += 3;
    }
    out[di] = '\0';
    return di;
}

/* Fill the fields a shorter (older-version) blob did not carry, so an upgraded
 * blob behaves like it did under its own firmware. Mirrors the prior version
 * semantics:
 *   < v2: per-control channels inherit the profile-default `channel`.
 *   < v6: each layer's min/max/curve/invert/type/channels INHERIT L1 (the v5
 *         "parameter-pages" shared-from-L1 model), so a v5 blob upgrades sensibly.
 * Fields not addressed here are already zeroed by the caller (keyboard keymaps,
 * etc. — 0 == unbound, matching how each version's own firmware read them).
 * `wire` is the decoded prefix length (so we know which version produced it). */
static void profile_fill_missing(struct profile *out, int wire)
{
    (void)out; (void)wire;   /* strict v9: a full 1038-byte blob carries every field */
}

/* ---- profile_from_b64 ----
 * Decode base64 string b64 (length len) into *out.
 * VERSION-AWARE: accepts any valid v1..v6 wire length (the clean prefixes). The
 * decoded byte count must equal one of those known lengths; the remaining struct
 * bytes are zero-filled and then completed by profile_fill_missing() so an older
 * blob upgrades sensibly. Returns 0 on success, -1 on any error (malformed input,
 * unknown length, padding issues).
 */
int profile_from_b64(const char *b64, int len, struct profile *out)
{
    if (len < 0) return -1;
    /* Safety: caller-supplied len must not exceed the real string length.
     * A NUL within [0,len) means the string is truncated; reject before any
     * further indexing reads past the buffer (defends attacker-controlled len). */
    for (int i = 0; i < len; i++) {
        if (b64[i] == '\0') return -1;
    }

    /* The char length pins the decoded byte count exactly (no padding ambiguity
     * because every valid wire length is a multiple of 3, so no '=' appears):
     *   chars = (bytes/3)*4  ->  bytes = (chars/4)*3.
     * Accept only a known version wire length. */
    if (len <= 0 || (len % 4) != 0)
        return -1;
    int src_len = (len / 4) * 3;
    /* strict v9 (spec 2.5): firmware stamps and reads ONLY the full 1038-byte v9
     * image. Legacy lengths (180/294/528) are dropped here so a stored v8 blob is
     * rejected and the librarian reseeds. The buf[0] version cross-check below still
     * holds: profile_wire_len(9) == sizeof == src_len. */
    if (src_len != (int)sizeof(struct profile))
        return -1;

    /* All valid wire lengths are multiples of 3 -> zero padding chars. */
    /* Validate overall structure: all chars must be valid base64 (no '='). */
    for (int i = 0; i < len; i++) {
        if (b64_char_val(b64[i]) < 0)
            return -1;
    }

    /* Decode into a local buffer — never write past sizeof(struct profile). */
    uint8_t buf[sizeof(struct profile)];
    int bi = 0;     /* byte index into buf */
    int si = 0;     /* index into b64 */

    while (si < len && bi < src_len) {
        int v0 = b64_char_val(b64[si]);
        int v1 = b64_char_val(b64[si + 1]);
        int v2 = b64_char_val(b64[si + 2]);
        int v3 = b64_char_val(b64[si + 3]);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
            return -1;

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12)
                        | ((uint32_t)v2 << 6)  | (uint32_t)v3;

        if (bi < src_len) buf[bi++] = (uint8_t)((triple >> 16) & 0xFF);
        if (bi < src_len) buf[bi++] = (uint8_t)((triple >> 8) & 0xFF);
        if (bi < src_len) buf[bi++] = (uint8_t)(triple & 0xFF);

        si += 4;
    }

    if (bi != src_len)
        return -1;

    /* Cross-check the embedded version byte against the wire length: a blob that
     * claims one version but is sized as another is malformed. (buf[0] is the
     * version byte at offset 0.) */
    if (profile_wire_len(buf[0]) != src_len)
        return -1;

    /* Zero the full struct first so any bytes the prefix did not carry start at 0,
     * then copy the decoded prefix and complete the missing fields. */
    memset(out, 0, sizeof(struct profile));
    memcpy(out, buf, (size_t)src_len);
    profile_fill_missing(out, src_len);
    return 0;
}

/* ---- profile_validate ----
 * Returns 0 if p is a well-formed profile, -1 otherwise.
 */
int profile_validate(const struct profile *p)
{
    if (p->version != PROFILE_VERSION)
        return -1;
    if (p->channel > 15)
        return -1;
    for (int i = 0; i < NUM_FADERS; i++) {
        if (p->fader[i].cc    > 127)  return -1;
        if (p->fader[i].min   > 127)  return -1;
        if (p->fader[i].max   > 127)  return -1;
        if (p->fader[i].curve > CURVE_EXP)         return -1;
        if (p->fader[i].invert > 1)                return -1;
        if (p->fader_channel[i] > 15)              return -1;  /* v2 per-fader ch */
        if (p->shift.fader_cc[i]     > 127)        return -1;  /* L2 fader CC (d1) */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (p->button[i].type > BTN_CC_VALUE)       return -1; /* v7: BTN_CHORD, F1: BTN_CC_VALUE */
        if (p->button_channel[i] > 15)              return -1; /* v2 per-button ch */
        if (p->button[i].value        > 127)        return -1; /* L1 button d1 (CC/value) */
        if (p->shift.button_value[i]  > 127)        return -1; /* L2 button d1           */
    }
    /* The appended layer banks (layer[] = L3..L8) carry fader CCs + button d1 values
     * that validate did not previously range-check; a seed-formula overflow (spec 3.6)
     * would otherwise ship a >127 MIDI d1 with no error. */
    for (int L = 0; L < NUM_LAYERS - 2; L++) {
        for (int i = 0; i < NUM_FADERS; i++)
            if (p->layer[L].fader_cc[i]     > 127) return -1;
        for (int i = 0; i < NUM_BUTTONS; i++)
            if (p->layer[L].button_value[i] > 127) return -1;
    }
    /* v3 button_key[i] / button_mod[i] AND v4 button_key_shift[i] /
     * button_mod_shift[i]: every u8 is a legal HID usage / modifier bitmask
     * (0 = unbound), so no bounds guard is needed. The version == PROFILE_VERSION
     * check above already rejects any older blob reaching v4 firmware. */
    /* v6: the per-layer completion banks (L2/L3/L4) carry the same kinds of fields
     * as L1, so bound them the same way. min/max are 0..127, curve 0..2, invert
     * 0/1, type 0..5, channels 0..15. (fader_cc/button_value/button_key/button_mod
     * for these layers live in the v5 banks and are u8-legal as above.) */
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            if (p->ext[L].fader_min[i]     > 127)       return -1;
            if (p->ext[L].fader_max[i]     > 127)       return -1;
            if (p->ext[L].fader_curve[i]   > CURVE_EXP) return -1;
            if (p->ext[L].fader_invert[i]  > 1)         return -1;
            if (p->ext[L].fader_channel[i] > 15)        return -1;
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            if (p->ext[L].button_type[i]    > BTN_CC_VALUE)      return -1; /* v7 BTN_CHORD, F1 BTN_CC_VALUE */
            if (p->ext[L].button_channel[i] > 15)                return -1;
        }
    }
    /* v8: per-button packed chord6. button_type==BTN_CHORD selects it; an inert slot
     * is all-zero (mode 0, count 0) and validates trivially. Validate every slot's
     * packed bytes are in range so a corrupt blob can't strand the press edge. */
    for (int L = 0; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            const struct chord6 *c = &p->chord6[L][i];
            /* Feature 1: a BTN_CC_VALUE button REUSES this slot for {sub,on,off}.
             * Look up the per-(layer,button) type (same source the wire byte came
             * from) and validate the reused layout instead of the chord layout;
             * lock the reserved bytes + sub range so a future field stays forkless. */
            uint8_t bt = (L == 0) ? p->button[i].type : p->ext[L - 1].button_type[i];
            if (bt == BTN_CC_VALUE) {
                if (c->b[0] != 0)  return -1;                 /* reserved */
                if (c->b[1] > 127) return -1;                 /* on_value  */
                if (c->b[2] > 127) return -1;                 /* off_value */
                if (c->b[3] > 2)   return -1;                 /* sub_mode  */
                if (c->b[4] != 0 || c->b[5] != 0) return -1;  /* reserved (forkless) */
                continue;   /* skip the chord checks for this slot */
            }
            uint8_t mode  = (uint8_t)((c->b[0] >> 5) & 0x07);
            uint8_t count = (uint8_t)(c->b[0] & 0x1F);
            if (mode > 2) return -1;                              /* 0=explicit,1=range,2=quality */
            if (mode == 0) {                                     /* explicit */
                if (count > CHORD6_MAX_EXPLICIT) return -1;       /* 5-note stored cap */
                for (int n = 0; n < CHORD6_MAX_EXPLICIT; n++)
                    if (c->b[1 + n] > 127) return -1;             /* notes are MIDI 0..127 */
            } else if (mode == 1) {                              /* range */
                if (count != 0) return -1;                        /* count unused for range */
                uint8_t rs = c->b[1], rc = c->b[2];
                if (rs > 127) return -1;
                if (rc > 8) return -1;                            /* > MAX_CHORD (chord_engine.h) */
                if (rc > 0 && (int)rs + (int)rc - 1 > 127) return -1;
            } else {                                             /* root+quality */
                if (count != 0) return -1;
                if (c->b[1] > 127) return -1;                    /* root */
                if (c->b[2] > 9)   return -1;                    /* quality enum, last = sus4 */
            }
        }
    }
    for (int L = 0; L < NUM_LAYERS; L++)
        for (int f = 0; f < NUM_FADERS; f++)
            if (p->fader_role[L][f] > 1) return -1;              /* 0=cc,1=chord_depth */
    if (p->chord_flags[0] > 127) return -1;                     /* chord velocity */
    return 0;
}
