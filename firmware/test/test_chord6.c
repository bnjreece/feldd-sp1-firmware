#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "profile.h"
#include "chord6.h"

static struct chord_def mk(void){ struct chord_def c; memset(&c,0,sizeof c); return c; }

/* explicit C-E-G round-trips; count + first notes preserved */
static void t_explicit(void){
    struct chord_def c = mk(); c.mode=0; c.count=3;
    c.notes[0]=60; c.notes[1]=64; c.notes[2]=67;
    struct chord6 p; chord6_pack(&c,&p);
    /* hdr = mode 0 << 5 | count 3 = 0x03; payload = 60,64,67,0,0 */
    assert(p.b[0]==0x03);
    assert(p.b[1]==60 && p.b[2]==64 && p.b[3]==67 && p.b[4]==0 && p.b[5]==0);
    struct chord_def d; chord6_unpack(&p,&d);
    assert(d.mode==0 && d.count==3 && d.quality==0 && d.range_count==0);
    assert(d.notes[0]==60 && d.notes[1]==64 && d.notes[2]==67);
    assert(!chord6_is_empty(&p));
}

/* explicit set of 7 notes is CLAMPED to 5 stored (spec §0.2 hand-picked cap) */
static void t_explicit_clamp_to_5(void){
    struct chord_def c = mk(); c.mode=0; c.count=6;   /* in-RAM allows 6 */
    for (int i=0;i<6;i++) c.notes[i]=(uint8_t)(50+i);
    struct chord6 p; chord6_pack(&c,&p);
    assert((p.b[0] & 0x1F) == 5);                    /* count clamped to 5 */
    assert((p.b[0] >> 5)   == 0);                    /* mode explicit */
    assert(p.b[1]==50 && p.b[5]==54);                /* first 5 stored, 6th dropped */
    struct chord_def d; chord6_unpack(&p,&d);
    assert(d.count==5 && d.notes[4]==54);
}

/* range 21..27 (start 21, count 7) round-trips */
static void t_range(void){
    struct chord_def c = mk(); c.mode=1; c.range_start=21; c.range_count=7;
    struct chord6 p; chord6_pack(&c,&p);
    assert((p.b[0]>>5)==1 && (p.b[0]&0x1F)==0);      /* mode range, count 0 */
    assert(p.b[1]==21 && p.b[2]==7 && p.b[3]==0);
    struct chord_def d; chord6_unpack(&p,&d);
    assert(d.mode==1 && d.range_start==21 && d.range_count==7 && d.count==0 && d.quality==0);
    assert(!chord6_is_empty(&p));
}

/* root+quality Cmin7 (root 48, quality 5) round-trips */
static void t_quality(void){
    struct chord_def c = mk(); c.mode=2; c.root=48; c.quality=5;
    struct chord6 p; chord6_pack(&c,&p);
    assert((p.b[0]>>5)==2 && (p.b[0]&0x1F)==0);
    assert(p.b[1]==48 && p.b[2]==5 && p.b[3]==0);
    struct chord_def d; chord6_unpack(&p,&d);
    assert(d.mode==2 && d.root==48 && d.quality==5 && d.range_count==0 && d.count==0);
    assert(!chord6_is_empty(&p));
}

/* a zeroed chord6 is empty (no notes, no range, no quality) */
static void t_empty(void){
    struct chord6 p; memset(&p,0,sizeof p);
    assert(chord6_is_empty(&p));
    struct chord_def d; chord6_unpack(&p,&d);
    assert(d.count==0 && d.range_count==0 && d.quality==0);
}

int main(void){
    t_explicit(); t_explicit_clamp_to_5(); t_range(); t_quality(); t_empty();
    printf("all chord6 tests passed\n");
    return 0;
}
