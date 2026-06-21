#include "chord_engine.h"

int chord_depth_from_cc(int cc)
{
    if (cc <  26) return CHORD_DEPTH_TRIAD;
    if (cc <  52) return CHORD_DEPTH_7;
    if (cc <  77) return CHORD_DEPTH_9;
    if (cc < 103) return CHORD_DEPTH_11;
    return CHORD_DEPTH_13;
}

/* Triad offsets per quality enum (0=none has no triad). */
static const int8_t TRIAD[10][3] = {
    {0,0,0},   /* 0 none/explicit-only (unused) */
    {0,4,7},   /* 1 maj  */
    {0,3,7},   /* 2 min  */
    {0,4,7},   /* 3 dom7 */
    {0,4,7},   /* 4 maj7 */
    {0,3,7},   /* 5 min7 */
    {0,3,6},   /* 6 dim  */
    {0,4,8},   /* 7 aug  */
    {0,2,7},   /* 8 sus2 */
    {0,5,7},   /* 9 sus4 */
};
/* The quality's own 7th offset (used at the +7th band). */
static const int8_t SEVENTH[10] = {
    0, 11, 10, 10, 11, 10, 0, 0, 0, 0   /* maj=+11, min/dom7/min7=+10, maj7=+11 */
};
/* Depth-eligible = qualities 1..5 only. */
static int depth_eligible(int q){ return q >= 1 && q <= 5; }

/* Append `off` (a semitone offset from root) as a note <=127, if not already
 * present; returns the new count. Drops the note if root+off > 127. */
static int add_off(uint8_t out[MAX_CHORD], int n, int root, int off)
{
    int note = root + off;
    if (note > 127 || n >= MAX_CHORD) return n;
    for (int i = 0; i < n; i++) if (out[i] == (uint8_t)note) return n;  /* de-dup */
    out[n++] = (uint8_t)note;
    return n;
}

/* Remove a note at `root+off` if present, compacting. Returns the new count. */
static int remove_off(uint8_t out[MAX_CHORD], int n, int root, int off)
{
    int note = root + off;
    for (int i = 0; i < n; i++) {
        if (out[i] == (uint8_t)note) {
            for (int j = i; j < n - 1; j++) out[j] = out[j+1];
            return n - 1;
        }
    }
    return n;
}

/* Sort ascending in place (tiny n, insertion sort) so out is low-to-high. */
static void sort_asc(uint8_t out[MAX_CHORD], int n)
{
    for (int i = 1; i < n; i++) {
        uint8_t v = out[i]; int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j+1] = out[j]; j--; }
        out[j+1] = v;
    }
}

static int resolve_quality(const struct chord_def *c, int depth, uint8_t out[MAX_CHORD])
{
    int q = c->quality, root = c->root, n = 0;
    /* triad always */
    for (int i = 0; i < 3; i++) n = add_off(out, n, root, TRIAD[q][i]);
    if (!depth_eligible(q) || depth == CHORD_DEPTH_TRIAD) { sort_asc(out, n); return n; }
    /* +7th: append the quality's 7th. */
    n = add_off(out, n, root, SEVENTH[q]);
    if (depth >= CHORD_DEPTH_9)  n = add_off(out, n, root, 14);   /* +9  */
    if (depth >= CHORD_DEPTH_11) n = add_off(out, n, root, 17);   /* +11 */
    if (depth >= CHORD_DEPTH_13) {
        n = add_off(out, n, root, 21);                            /* +13 */
        /* 13th-drop: cap at 6 notes by dropping the 11th (+17) first, then 5th (+7). */
        if (n > 6) n = remove_off(out, n, root, 17);
        if (n > 6) n = remove_off(out, n, root, 7);
    }
    sort_asc(out, n);
    return n;
}

int chord_resolve(const struct chord_def *c, int depth, uint8_t out[MAX_CHORD])
{
    if (c == 0) return 0;
    if (c->range_count > 0) {                       /* range: start..start+count-1 */
        int n = 0;
        for (int i = 0; i < c->range_count && n < MAX_CHORD; i++) {
            int note = c->range_start + i;
            if (note > 127) break;
            out[n++] = (uint8_t)note;
        }
        return n;                                   /* already ascending */
    }
    if (c->quality != 0) return resolve_quality(c, depth, out);
    /* explicit set */
    int n = 0;
    for (int i = 0; i < c->count && n < MAX_CHORD; i++) out[n++] = c->notes[i];
    return n;
}

void chord_tx_init(struct chord_tx_ring *r){ r->head=r->tail=r->count=0; }

int chord_tx_push(struct chord_tx_ring *r, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (r->count >= CHORD_TX_RING) return 0;
    r->buf[r->tail].status = status; r->buf[r->tail].d1 = d1; r->buf[r->tail].d2 = d2;
    r->tail = (uint8_t)((r->tail + 1) % CHORD_TX_RING);
    r->count++;
    return 1;
}

static int ring_is_off(uint8_t s){ return (s & 0xF0) == 0x80; }

/* Pop up to CHORD_TX_BUDGET, Note-Offs first. Two passes over the live ring: pass
 * 1 collects offs, pass 2 fills the remaining budget with ons, each removed by
 * compacting the ring (small N, simple + correct over cleverness). */
int chord_tx_drain(struct chord_tx_ring *r, struct chord_tx_msg out[CHORD_TX_BUDGET])
{
    int n = 0;
    for (int pass = 0; pass < 2 && n < CHORD_TX_BUDGET; pass++) {
        int want_off = (pass == 0);
        int i = r->head, scanned = 0, total = r->count;
        while (scanned < total && n < CHORD_TX_BUDGET) {
            struct chord_tx_msg *m = &r->buf[i];
            if (ring_is_off(m->status) == want_off) {
                out[n++] = *m;
                /* remove element at index i by shifting the logical ring down. */
                int j = i;
                while (j != r->tail) {
                    int k = (j + 1) % CHORD_TX_RING;
                    if (k == r->tail) break;
                    r->buf[j] = r->buf[k];
                    j = k;
                }
                r->tail = (uint8_t)((r->tail + CHORD_TX_RING - 1) % CHORD_TX_RING);
                r->count--;
                total--;
                /* do not advance i: the next element shifted into this slot. */
            } else {
                i = (i + 1) % CHORD_TX_RING;
            }
            scanned++;
        }
    }
    return n;
}
