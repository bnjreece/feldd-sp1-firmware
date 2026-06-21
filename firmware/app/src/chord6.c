/*
 * chord6.c — pack/unpack the v8 stored 6-byte chord (spec §2).
 *
 * The profile stores chord6 (6 B) per (layer, button); chord_engine consumes the
 * unpacked struct chord_def (12 B). These two functions are the ONLY translation
 * point. Freestanding-friendly: only <stdint.h> + <string.h>.
 */
#include <stdint.h>
#include <string.h>
#include "chord6.h"

void chord6_pack(const struct chord_def *in, struct chord6 *out)
{
    memset(out, 0, sizeof *out);
    if (in == 0) return;
    uint8_t mode = in->mode & 0x07;
    if (mode == CHORD6_MODE_EXPLICIT) {
        uint8_t cnt = in->count;
        if (cnt > CHORD6_MAX_EXPLICIT) cnt = CHORD6_MAX_EXPLICIT;   /* 5-note stored cap */
        out->b[0] = (uint8_t)((mode << 5) | (cnt & 0x1F));
        for (int i = 0; i < cnt; i++) out->b[1 + i] = in->notes[i];
    } else if (mode == CHORD6_MODE_RANGE) {
        out->b[0] = (uint8_t)(mode << 5);                          /* count 0 */
        out->b[1] = in->range_start;
        out->b[2] = in->range_count;
    } else { /* CHORD6_MODE_QUALITY */
        out->b[0] = (uint8_t)(mode << 5);                          /* count 0 */
        out->b[1] = in->root;
        out->b[2] = in->quality;
    }
}

void chord6_unpack(const struct chord6 *in, struct chord_def *out)
{
    memset(out, 0, sizeof *out);
    if (in == 0) return;
    uint8_t mode  = (uint8_t)((in->b[0] >> 5) & 0x07);
    uint8_t count = (uint8_t)(in->b[0] & 0x1F);
    out->mode = mode;
    if (mode == CHORD6_MODE_EXPLICIT) {
        if (count > CHORD6_MAX_EXPLICIT) count = CHORD6_MAX_EXPLICIT;
        out->count = count;
        for (int i = 0; i < count; i++) out->notes[i] = in->b[1 + i];
    } else if (mode == CHORD6_MODE_RANGE) {
        out->range_start = in->b[1];
        out->range_count = in->b[2];
    } else { /* CHORD6_MODE_QUALITY */
        out->root    = in->b[1];
        out->quality = in->b[2];
    }
}

int chord6_is_empty(const struct chord6 *c)
{
    if (c == 0) return 1;
    uint8_t mode  = (uint8_t)((c->b[0] >> 5) & 0x07);
    uint8_t count = (uint8_t)(c->b[0] & 0x1F);
    if (mode == CHORD6_MODE_EXPLICIT) return count == 0;
    if (mode == CHORD6_MODE_RANGE)    return c->b[2] == 0;   /* range_count == 0 */
    return c->b[2] == 0;                                     /* quality == 0 */
}
