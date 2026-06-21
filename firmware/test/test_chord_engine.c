#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "chord_engine.h"

static struct chord_def mk(void){ struct chord_def c; memset(&c,0,sizeof c); return c; }

static void t_depth_bands(void){
    assert(chord_depth_from_cc(0)   == CHORD_DEPTH_TRIAD);
    assert(chord_depth_from_cc(25)  == CHORD_DEPTH_TRIAD);
    assert(chord_depth_from_cc(26)  == CHORD_DEPTH_7);
    assert(chord_depth_from_cc(51)  == CHORD_DEPTH_7);
    assert(chord_depth_from_cc(52)  == CHORD_DEPTH_9);
    assert(chord_depth_from_cc(76)  == CHORD_DEPTH_9);
    assert(chord_depth_from_cc(77)  == CHORD_DEPTH_11);
    assert(chord_depth_from_cc(102) == CHORD_DEPTH_11);
    assert(chord_depth_from_cc(103) == CHORD_DEPTH_13);
    assert(chord_depth_from_cc(127) == CHORD_DEPTH_13);
}
static void t_explicit(void){
    struct chord_def c = mk(); c.mode=0; c.count=3;
    c.notes[0]=60; c.notes[1]=64; c.notes[2]=67;
    uint8_t out[MAX_CHORD]; int n = chord_resolve(&c, CHORD_DEPTH_13, out);
    assert(n==3 && out[0]==60 && out[1]==64 && out[2]==67);   /* depth ignored */
}
static void t_range(void){
    struct chord_def c = mk(); c.mode=1; c.range_start=21; c.range_count=7;
    uint8_t out[MAX_CHORD]; int n = chord_resolve(&c, CHORD_DEPTH_13, out);
    assert(n==7);                                             /* 7 <= MAX_CHORD; profile_validate (Fix 2) already rejects range_count > 8, so a longer run never reaches here */
    for (int i=0;i<7;i++) assert(out[i]==21+i);               /* depth ignored */
}
static void t_quality_triads(void){
    uint8_t out[MAX_CHORD];
    struct chord_def c = mk(); c.mode=2; c.root=60;
    c.quality=1; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[0]==60 && out[1]==64 && out[2]==67); /* maj */
    c.quality=2; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[1]==63);                              /* min */
    c.quality=6; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[2]==66);                              /* dim */
    c.quality=7; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[2]==68);                              /* aug */
    c.quality=8; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[1]==62);                              /* sus2 */
    c.quality=9; assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3 && out[1]==65);                              /* sus4 */
}
static void t_depth_ladder(void){
    uint8_t out[MAX_CHORD]; struct chord_def c = mk(); c.mode=2; c.root=60; c.quality=5; /* min7 */
    assert(chord_resolve(&c,CHORD_DEPTH_TRIAD,out)==3);                 /* 60,63,67 */
    int n7 = chord_resolve(&c,CHORD_DEPTH_7,out);  assert(n7==4 && out[3]==70);   /* +b7 */
    int n9 = chord_resolve(&c,CHORD_DEPTH_9,out);  assert(n9==5 && out[4]==74);   /* +9 (root+14) */
    int n11= chord_resolve(&c,CHORD_DEPTH_11,out); assert(n11==6 && out[5]==77);  /* +11 (root+17) */
}
static void t_thirteenth_drop(void){
    uint8_t out[MAX_CHORD]; struct chord_def c = mk(); c.mode=2; c.root=60; c.quality=5; /* min7 */
    int n13 = chord_resolve(&c,CHORD_DEPTH_13,out);
    /* +13 = root,3rd,5th,7th,9th,13th (11th dropped) = 6 notes, none == root+17 */
    assert(n13==6);
    for (int i=0;i<n13;i++) assert(out[i] != 60+17);                   /* 11th dropped */
    assert(out[5]==60+21);                                             /* 13th present */
}
static void t_depth_gated_off(void){
    uint8_t out[MAX_CHORD]; struct chord_def c = mk(); c.mode=2; c.root=60;
    c.quality=6; assert(chord_resolve(&c,CHORD_DEPTH_13,out)==3);      /* dim: triad only */
    c.quality=8; assert(chord_resolve(&c,CHORD_DEPTH_13,out)==3);      /* sus2: triad only */
}
static void t_high_root_clamp(void){
    uint8_t out[MAX_CHORD]; struct chord_def c = mk(); c.mode=2; c.root=120; c.quality=4; /* maj7 */
    int n = chord_resolve(&c,CHORD_DEPTH_13,out);
    for (int i=0;i<n;i++) assert(out[i] <= 127);                       /* no note > 127 */
}
static void t_null(void){ uint8_t out[MAX_CHORD]; assert(chord_resolve(0,0,out)==0); }

static int is_off(uint8_t s){ return (s & 0xF0) == 0x80; }
static void t_tx_ring_cap(void){
    struct chord_tx_ring r; chord_tx_init(&r);
    /* push the worst case: 12 Note-Offs then 12 Note-Ons = 24 messages. */
    for (int i=0;i<12;i++) assert(chord_tx_push(&r, 0x80, (uint8_t)(40+i), 0));
    for (int i=0;i<12;i++) assert(chord_tx_push(&r, 0x90, (uint8_t)(60+i), 100));
    assert(r.count == 24);
    struct chord_tx_msg out[CHORD_TX_BUDGET];
    int total=0, drained_offs_first=1, seen_on=0;
    while (r.count > 0) {
        int n = chord_tx_drain(&r, out);
        assert(n >= 1 && n <= CHORD_TX_BUDGET);
        for (int i=0;i<n;i++){
            if (!is_off(out[i].status)) seen_on=1;
            else if (seen_on) drained_offs_first=0;   /* an off after an on -> order broke */
        }
        total += n;
    }
    assert(total == 24);                 /* nothing lost */
    assert(drained_offs_first == 1);     /* all 12 offs drained before any on */
    assert(r.count == 0);
    /* 24 msgs / 8 per tick = 3 drains -> empties in 3 ticks. */
}
static void t_tx_ring_full(void){
    struct chord_tx_ring r; chord_tx_init(&r);
    for (int i=0;i<CHORD_TX_RING;i++) assert(chord_tx_push(&r,0x90,(uint8_t)i,1));
    assert(chord_tx_push(&r,0x90,99,1) == 0);   /* full -> rejected, not corrupt */
    assert(r.count == CHORD_TX_RING);
}
/* Fix 6: chord_flush_all() purges the ring (via chord_tx_init) so a queued On/Off
 * for a now-stale map cannot drain AFTER the flush's direct Offs and re-strand a
 * note. main.c's chord_flush_all is build-verified-only, so prove the underlying
 * operation here: a populated ring, after chord_tx_init, is empty and drains
 * NOTHING (no stale Note-On survives). */
static void t_tx_ring_flush_purges(void){
    struct chord_tx_ring r; chord_tx_init(&r);
    /* queue some chord Note-Ons (the stale-map press) + an Off. */
    for (int i=0;i<5;i++) assert(chord_tx_push(&r, 0x90, (uint8_t)(60+i), 100));
    assert(chord_tx_push(&r, 0x80, 60, 0));
    assert(r.count == 6);
    chord_tx_init(&r);                 /* the flush's purge step */
    assert(r.count == 0);              /* ring emptied */
    struct chord_tx_msg out[CHORD_TX_BUDGET];
    assert(chord_tx_drain(&r, out) == 0);   /* nothing drains afterward - no stale On */
}

int main(void){
    t_depth_bands(); t_explicit(); t_range(); t_quality_triads();
    t_depth_ladder(); t_thirteenth_drop(); t_depth_gated_off();
    t_high_root_clamp(); t_null();
    t_tx_ring_cap(); t_tx_ring_full(); t_tx_ring_flush_purges();
    printf("all chord_engine tests passed\n");
    return 0;
}
