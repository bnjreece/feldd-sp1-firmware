/*
 * profile.c — profile validate + base64 codec
 *
 * Freestanding-friendly: only <stdint.h> and <string.h> (no malloc, no stdio).
 * All sizes derived from sizeof(struct profile) — never hardcoded.
 */

#include <stdint.h>
#include <string.h>
#include "profile.h"

/* ---- base64 alphabet ---- */
static const char b64_enc_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
 * Encoded length for N input bytes: ceil(N/3)*4 chars + 1 NUL.
 */
int profile_to_b64(const struct profile *p, char *out, int outcap)
{
    const uint8_t *src = (const uint8_t *)p;
    int src_len = (int)sizeof(struct profile);

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

/* ---- profile_from_b64 ----
 * Decode base64 string b64 (length len) into *out.
 * The decoded byte count MUST equal sizeof(struct profile); otherwise returns -1.
 * Returns 0 on success, -1 on any error (malformed input, wrong length, padding issues).
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

    int src_len = (int)sizeof(struct profile);

    /* A valid base64 encoding of src_len bytes is exactly ((src_len+2)/3)*4 chars. */
    int expected_b64_len = ((src_len + 2) / 3) * 4;
    if (len != expected_b64_len)
        return -1;

    /* We know exactly how many padding '=' chars to expect:
     * padding = (3 - src_len % 3) % 3  */
    int pad = (3 - (src_len % 3)) % 3;

    /* Validate overall structure: last 'pad' chars must be '=',
     * all others must be valid base64 chars. */
    for (int i = 0; i < len; i++) {
        if (i >= len - pad) {
            if (b64[i] != '=')
                return -1;
        } else {
            if (b64_char_val(b64[i]) < 0)
                return -1;
        }
    }

    /* Decode into a local buffer — never write past sizeof(struct profile). */
    uint8_t buf[sizeof(struct profile)];
    int bi = 0;     /* byte index into buf */
    int si = 0;     /* index into b64 */

    while (si < len && bi < src_len) {
        int v0 = b64_char_val(b64[si]);
        int v1 = b64_char_val(b64[si + 1]);
        int v2 = (b64[si + 2] == '=') ? 0 : b64_char_val(b64[si + 2]);
        int v3 = (b64[si + 3] == '=') ? 0 : b64_char_val(b64[si + 3]);

        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0)
            return -1;

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12)
                        | ((uint32_t)v2 << 6)  | (uint32_t)v3;

        if (bi < src_len) buf[bi++] = (uint8_t)((triple >> 16) & 0xFF);
        if (bi < src_len && b64[si + 2] != '=') buf[bi++] = (uint8_t)((triple >> 8) & 0xFF);
        if (bi < src_len && b64[si + 3] != '=') buf[bi++] = (uint8_t)(triple & 0xFF);

        si += 4;
    }

    if (bi != src_len)
        return -1;

    memcpy(out, buf, sizeof(struct profile));
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
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (p->button[i].type > BTN_PROFILE_SWITCH) return -1;
        if (p->button_channel[i] > 15)              return -1; /* v2 per-button ch */
    }
    /* v3 button_key[i] / button_mod[i] AND v4 button_key_shift[i] /
     * button_mod_shift[i]: every u8 is a legal HID usage / modifier bitmask
     * (0 = unbound), so no bounds guard is needed. The version == PROFILE_VERSION
     * check above already rejects any older blob reaching v4 firmware. */
    return 0;
}
