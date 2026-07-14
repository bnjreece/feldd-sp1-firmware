#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mapping.h"

static struct midi_msg cap[8]; static int ncap;
static void sink(const struct midi_msg *m, void *ctx){ (void)ctx; cap[ncap++]=*m; }

/* v9: seed ALL appended layer banks (L3..L8 = layer[0..NUM_LAYERS-3]) distinct
 * from inline (L1) and shift (L2). Bases 40/60 keep layer[0]=L3 and layer[1]=L4
 * byte-identical to the pre-v9 seed so t_*_four_layers still pass. */
static void seed_extra_layers(struct profile *p) {
    for (int b = 0; b < NUM_LAYERS - 2; b++) {
        for (int i = 0; i < NUM_FADERS; i++)
            p->layer[b].fader_cc[i] = (uint8_t)(40 + b * 10 + i);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->layer[b].button_value[i] = (uint8_t)(60 + b * 10 + i);
            p->layer[b].button_key[i]   = (uint8_t)(1  + b * 10 + i);
            p->layer[b].button_mod[i]   = (uint8_t)(b + 1);
        }
    }
}
/* v6: seed the per-layer ext banks (L2/L3/L4) with values DISTINCT from L1 and
 * from each other, so a per-layer read test proves the engine pulls min/max/
 * curve/invert/type/channels from the ACTIVE layer's bank, not from L1. ext[0]=L2
 * (shift), ext[1]=L3 (layer[0]), ext[2]=L4 (layer[1]). */
static void seed_ext_banks(struct profile *p) {
    for (int L = 0; L < NUM_LAYERS - 1; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            p->ext[L].fader_min[i]     = (uint8_t)(L * 4 + i);          /* 0..11 */
            p->ext[L].fader_max[i]     = (uint8_t)(100 + L * 4 + i);    /* 100.. */
            p->ext[L].fader_curve[i]   = (uint8_t)((L + i) % 3);
            p->ext[L].fader_invert[i]  = (uint8_t)((L + i) % 2);
            p->ext[L].fader_channel[i] = (uint8_t)((L * 2 + i + 1) & 0x0F);
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->ext[L].button_type[i]    = (uint8_t)((L + i) % 6);
            p->ext[L].button_channel[i] = (uint8_t)((L * 3 + i + 1) & 0x0F);
        }
    }
}
static struct profile P(void){
    struct profile p; memset(&p,0,sizeof p);
    p.version=PROFILE_VERSION; p.channel=2;
    /* v2: per-control channels default to the profile channel (2) so these
     * fixtures exercise the uniform case; t_*_per_channel covers the split. */
    for(int i=0;i<NUM_FADERS;i++){ p.fader[i]=(struct fader_map){.cc=(uint8_t)(7+i),.min=0,.max=127,.curve=CURVE_LINEAR,.invert=0}; p.shift.fader_cc[i]=(uint8_t)(20+i); p.fader_channel[i]=2;}
    for(int i=0;i<NUM_BUTTONS;i++) p.button_channel[i]=2;
    p.button[0]=(struct button_map){.type=BTN_NOTE,.value=60};
    p.button[1]=(struct button_map){.type=BTN_CC_MOMENTARY,.value=64};
    seed_extra_layers(&p);
    seed_ext_banks(&p);
    return p;
}
static void t_fader_cc(void){
    struct profile p=P(); ncap=0;
    map_fader(&p,0,100,0,sink,0);
    assert(ncap==1); assert(cap[0].status==(0xB0|2)); assert(cap[0].d1==7); assert(cap[0].d2==100); assert(cap[0].len==3);
}
static void t_fader_invert(void){
    struct profile p=P(); p.fader[0].invert=1; ncap=0;
    map_fader(&p,0,0,0,sink,0);
    assert(ncap==1); assert(cap[0].d2==127);   /* inverted: 0 -> 127 */
}
static void t_fader_range(void){
    struct profile p=P(); p.fader[1].min=0; p.fader[1].max=63; ncap=0;
    map_fader(&p,1,127,0,sink,0);
    assert(cap[0].d2==63);   /* full-scale clamped to max */
}
static void t_fader_shift_bank(void){
    struct profile p=P(); ncap=0;
    map_fader(&p,0,50,1,sink,0);            /* layer 1 = shift bank */
    assert(cap[0].d1==20);   /* shift layer uses shift.fader_cc[0] */
}
/* v5: fader CC is selected per LAYER: 0->inline cc, 1->shift.fader_cc,
 * 2->layer[0].fader_cc (L3), 3->layer[1].fader_cc (L4). */
static void t_fader_four_layers(void){
    struct profile p=P();
    ncap=0; map_fader(&p,0,50,0,sink,0); assert(cap[0].d1==7);    /* L1 inline cc 7+0 */
    ncap=0; map_fader(&p,0,50,1,sink,0); assert(cap[0].d1==20);   /* L2 shift 20+0 */
    ncap=0; map_fader(&p,0,50,2,sink,0); assert(cap[0].d1==40);   /* L3 layer[0] */
    ncap=0; map_fader(&p,0,50,3,sink,0); assert(cap[0].d1==50);   /* L4 layer[1] */
}
/* v5: button VALUE (MIDI) selected per layer for a CC_MOMENTARY button. v6: the
 * TYPE is now per-layer too, so force CC_MOMENTARY on every layer's bank to keep
 * this test focused purely on VALUE selection. */
static void t_button_four_layers(void){
    struct profile p=P();
    p.button[1]=(struct button_map){.type=BTN_CC_MOMENTARY,.value=64};
    p.shift.button_value[1]=65;   /* bind the shift button to a nonzero CC: P() zeroes it, and
                                   * the 0.27.3 CC0 guard silences value-0 buttons (unbound), so this
                                   * layer must carry a real value to exercise L2 VALUE selection. */
    for(int L=0;L<NUM_LAYERS-1;L++) p.ext[L].button_type[1]=BTN_CC_MOMENTARY;
    ncap=0; map_button(&p,1,1,0,sink,0); assert(cap[0].d1==64);   /* L1 inline */
    ncap=0; map_button(&p,1,1,1,sink,0); assert(cap[0].d1==p.shift.button_value[1]); /* L2 */
    ncap=0; map_button(&p,1,1,2,sink,0); assert(cap[0].d1==61);   /* L3 layer[0] */
    ncap=0; map_button(&p,1,1,3,sink,0); assert(cap[0].d1==71);   /* L4 layer[1] */
}
/* 0.27.3 (petercolombo's bug): a CC button left on value 0 is UNBOUND and must stay
 * SILENT, not fire CC0 (Bank Select) and collide with fader 0. Mirrors map_fader's
 * cc==0 guard. Loading the TX-6 profile + reloading old presets on 0.27.2 left upper-
 * layer PLAY buttons as cc_momentary CC0; they must emit nothing. */
static void t_button_cc0_unbound_silent(void){
    struct profile p=P();
    p.button[2]=(struct button_map){.type=BTN_CC_MOMENTARY,.value=0};   /* value 0 = unbound */
    ncap=0; map_button(&p,2,1,0,sink,0); assert(ncap==0);   /* press:   nothing */
    ncap=0; map_button(&p,2,0,0,sink,0); assert(ncap==0);   /* release: nothing */
    p.button[2].value=64;                                   /* sanity: a real CC on the same button fires */
    ncap=0; map_button(&p,2,1,0,sink,0); assert(ncap==1 && cap[0].d1==64);
}
/* v9: the four per-layer SELECT accessors resolve L5..L8 (layers 4..7) to
 * layer[layer-2] (layer[2..5]), NOT the L1 inline default. Before the widen a
 * layer>=4 fell to `default` and returned L1's CC/value/key/mod. */
static void t_select_accessors_layers_5_8(void){
    struct profile p=P();
    for (int L = 4; L < NUM_LAYERS; L++) {
        const struct layer_bank *b = &p.layer[L-2];
        for (int i = 0; i < NUM_FADERS; i++)
            assert(profile_layer_fader_cc(&p,i,L)==b->fader_cc[i]);
        for (int i = 0; i < NUM_BUTTONS; i++) {
            assert(profile_layer_button_value(&p,i,L)==b->button_value[i]);
            assert(profile_layer_button_key(&p,i,L)==b->button_key[i]);
            assert(profile_layer_button_mod(&p,i,L)==b->button_mod[i]);
        }
    }
}
/* v6: the new per-layer ext accessors return the ACTIVE LAYER's value: layer 0
 * = L1 inline, 1 = ext[0] (L2), 2 = ext[1] (L3), 3 = ext[2] (L4). */
static void t_ext_accessors_per_layer(void){
    struct profile p=P();
    /* L1 (layer 0) reads the inline fader_map / button.type / per-control ch. */
    for(int i=0;i<NUM_FADERS;i++){
        assert(profile_layer_fader_min(&p,i,0)==p.fader[i].min);
        assert(profile_layer_fader_max(&p,i,0)==p.fader[i].max);
        assert(profile_layer_fader_curve(&p,i,0)==p.fader[i].curve);
        assert(profile_layer_fader_invert(&p,i,0)==p.fader[i].invert);
        assert(profile_layer_fader_channel(&p,i,0)==p.fader_channel[i]);
    }
    for(int i=0;i<NUM_BUTTONS;i++){
        assert(profile_layer_button_type(&p,i,0)==p.button[i].type);
        assert(profile_layer_button_channel(&p,i,0)==p.button_channel[i]);
    }
    /* L2/L3/L4 (layers 1..3) read ext[layer-1], NOT L1. */
    for(int L=1;L<NUM_LAYERS;L++){
        const struct layer_ext *e=&p.ext[L-1];
        for(int i=0;i<NUM_FADERS;i++){
            assert(profile_layer_fader_min(&p,i,L)==e->fader_min[i]);
            assert(profile_layer_fader_max(&p,i,L)==e->fader_max[i]);
            assert(profile_layer_fader_curve(&p,i,L)==e->fader_curve[i]);
            assert(profile_layer_fader_invert(&p,i,L)==e->fader_invert[i]);
            assert(profile_layer_fader_channel(&p,i,L)==e->fader_channel[i]);
        }
        for(int i=0;i<NUM_BUTTONS;i++){
            assert(profile_layer_button_type(&p,i,L)==e->button_type[i]);
            assert(profile_layer_button_channel(&p,i,L)==e->button_channel[i]);
        }
    }
}
/* v6: a full-scale fader on each layer clamps to THAT layer's max (not L1's). */
static void t_fader_per_layer_range(void){
    struct profile p=P();
    /* L1 max is 127; give each ext layer a distinct, lower max. */
    p.fader[0].min=0; p.fader[0].max=127; p.fader[0].curve=CURVE_LINEAR; p.fader[0].invert=0;
    p.ext[0].fader_min[0]=0; p.ext[0].fader_max[0]=63; p.ext[0].fader_curve[0]=CURVE_LINEAR; p.ext[0].fader_invert[0]=0;
    p.ext[1].fader_min[0]=0; p.ext[1].fader_max[0]=31; p.ext[1].fader_curve[0]=CURVE_LINEAR; p.ext[1].fader_invert[0]=0;
    p.ext[2].fader_min[0]=0; p.ext[2].fader_max[0]=15; p.ext[2].fader_curve[0]=CURVE_LINEAR; p.ext[2].fader_invert[0]=0;
    ncap=0; map_fader(&p,0,127,0,sink,0); assert(cap[0].d2==127);  /* L1 */
    ncap=0; map_fader(&p,0,127,1,sink,0); assert(cap[0].d2==63);   /* L2 ext[0] */
    ncap=0; map_fader(&p,0,127,2,sink,0); assert(cap[0].d2==31);   /* L3 ext[1] */
    ncap=0; map_fader(&p,0,127,3,sink,0); assert(cap[0].d2==15);   /* L4 ext[2] */
}
/* v6: fader curve is read per-layer. L1 linear, L3 log -> different mapped value
 * for the same input. */
static void t_fader_per_layer_curve(void){
    struct profile p=P();
    p.fader[0].min=0; p.fader[0].max=127; p.fader[0].curve=CURVE_LINEAR; p.fader[0].invert=0;
    /* ext[1] = L3: log curve, full range, no invert. cc 64 -> 64*64/127 = 32. */
    p.ext[1].fader_min[0]=0; p.ext[1].fader_max[0]=127; p.ext[1].fader_curve[0]=CURVE_LOG; p.ext[1].fader_invert[0]=0;
    ncap=0; map_fader(&p,0,64,0,sink,0); assert(cap[0].d2==64);   /* L1 linear */
    ncap=0; map_fader(&p,0,64,2,sink,0); assert(cap[0].d2==32);   /* L3 log */
}
/* v6: fader invert is read per-layer. */
static void t_fader_per_layer_invert(void){
    struct profile p=P();
    p.fader[0].min=0; p.fader[0].max=127; p.fader[0].curve=CURVE_LINEAR; p.fader[0].invert=0;
    /* ext[2] = L4: inverted, full range, linear. cc 0 -> 127. */
    p.ext[2].fader_min[0]=0; p.ext[2].fader_max[0]=127; p.ext[2].fader_curve[0]=CURVE_LINEAR; p.ext[2].fader_invert[0]=1;
    ncap=0; map_fader(&p,0,0,0,sink,0); assert(cap[0].d2==0);     /* L1 not inverted */
    ncap=0; map_fader(&p,0,0,3,sink,0); assert(cap[0].d2==127);   /* L4 inverted */
}
/* v6: each fader rides ITS LAYER's channel (ext bank), not L1's. */
static void t_fader_per_layer_channel(void){
    struct profile p=P();
    p.fader_channel[0]=2;             /* L1 */
    p.ext[0].fader_channel[0]=5;      /* L2 */
    p.ext[1].fader_channel[0]=9;      /* L3 */
    p.ext[2].fader_channel[0]=12;     /* L4 */
    ncap=0; map_fader(&p,0,100,0,sink,0); assert(cap[0].status==(0xB0|2));
    ncap=0; map_fader(&p,0,100,1,sink,0); assert(cap[0].status==(0xB0|5));
    ncap=0; map_fader(&p,0,100,2,sink,0); assert(cap[0].status==(0xB0|9));
    ncap=0; map_fader(&p,0,100,3,sink,0); assert(cap[0].status==(0xB0|12));
}
/* v9: the misroute trap. A fader on L5 (layer 4) must ride L5's OWN channel
 * (ext[3]), not silently fall back to L1's. Before the ext-accessor widen,
 * layer 4 failed the <=3 guard and emitted on L1's channel. The CC (d1) comes
 * from the M1-widened select accessor (layer[2]); the channel from ext[3]. */
static void t_fader_layer5_rides_own_channel(void){
    struct profile p=P();
    p.fader_channel[0]=2;                 /* L1 */
    p.ext[3].fader_channel[0]=10;         /* L5 = ext[3] */
    p.ext[3].fader_min[0]=0;   p.ext[3].fader_max[0]=127;
    p.ext[3].fader_curve[0]=CURVE_LINEAR; p.ext[3].fader_invert[0]=0;
    ncap=0; map_fader(&p,0,100,4,sink,0);
    assert(cap[0].status==(uint8_t)(0xB0|10));       /* rides ch10, NOT ch2 */
    assert(cap[0].d1==p.layer[2].fader_cc[0]);       /* L5 CC = layer[2].fader_cc[0] (60) */
    assert(cap[0].d2==100);                          /* full range, linear */
}
/* v6: button TYPE is read per-layer. Same button is NOTE on L1, CC_MOMENTARY on
 * L2, and NONE (silent) on L3. */
static void t_button_per_layer_type(void){
    struct profile p=P();
    p.button[2]=(struct button_map){.type=BTN_NOTE,.value=50};   /* L1 inline value */
    p.button_channel[2]=0;
    p.ext[0].button_type[2]=BTN_CC_MOMENTARY; p.ext[0].button_channel[2]=0;
    p.shift.button_value[2]=51;
    p.ext[1].button_type[2]=BTN_NONE;
    /* L1: NOTE on -> 0x90 */
    ncap=0; map_button(&p,2,1,0,sink,0); assert(ncap==1); assert(cap[0].status==(0x90|0)); assert(cap[0].d1==50);
    /* L2: CC_MOMENTARY -> 0xB0, value from shift bank */
    ncap=0; map_button(&p,2,1,1,sink,0); assert(ncap==1); assert(cap[0].status==(0xB0|0)); assert(cap[0].d1==51); assert(cap[0].d2==127);
    /* L3: NONE -> silent */
    ncap=0; map_button(&p,2,1,2,sink,0); assert(ncap==0);
}
/* v6: button rides ITS LAYER's channel (ext bank), not L1's. */
static void t_button_per_layer_channel(void){
    struct profile p=P();
    p.button[1]=(struct button_map){.type=BTN_CC_MOMENTARY,.value=64};
    p.button_channel[1]=2;            /* L1 */
    p.shift.button_value[1]=64;       /* bind the shift button (P() zeroes it); 0.27.3 guard silences value-0 */
    /* keep the type CC_MOMENTARY on every layer so the channel is the only var */
    p.ext[0].button_type[1]=BTN_CC_MOMENTARY; p.ext[0].button_channel[1]=4;
    p.ext[1].button_type[1]=BTN_CC_MOMENTARY; p.ext[1].button_channel[1]=7;
    p.ext[2].button_type[1]=BTN_CC_MOMENTARY; p.ext[2].button_channel[1]=11;
    ncap=0; map_button(&p,1,1,0,sink,0); assert(cap[0].status==(0xB0|2));
    ncap=0; map_button(&p,1,1,1,sink,0); assert(cap[0].status==(0xB0|4));
    ncap=0; map_button(&p,1,1,2,sink,0); assert(cap[0].status==(0xB0|7));
    ncap=0; map_button(&p,1,1,3,sink,0); assert(cap[0].status==(0xB0|11));
}
static void t_button_note(void){
    struct profile p=P(); ncap=0;
    map_button(&p,0,1,0,sink,0);     /* press -> note on */
    assert(cap[0].status==(0x90|2)); assert(cap[0].d1==60); assert(cap[0].d2==127);
    ncap=0; map_button(&p,0,0,0,sink,0);   /* release -> note off */
    assert(cap[0].status==(0x80|2)); assert(cap[0].d1==60);
}
static void t_button_cc_momentary(void){
    struct profile p=P(); ncap=0;
    map_button(&p,1,1,0,sink,0);
    assert(cap[0].status==(0xB0|2)); assert(cap[0].d1==64); assert(cap[0].d2==127);
    ncap=0; map_button(&p,1,0,0,sink,0);
    assert(cap[0].d2==0);
}
static void t_button_none_silent(void){
    struct profile p=P(); p.button[2].type=BTN_NONE; ncap=0;
    map_button(&p,2,1,0,sink,0); assert(ncap==0);
}
static void t_fader_curve_log(void){
    /* cc_val=64, CURVE_LOG: v=64*64/127=32, range 0..127 -> d2=32 */
    struct profile p=P(); p.fader[0].curve=CURVE_LOG; ncap=0;
    map_fader(&p,0,64,0,sink,0);
    assert(ncap==1); assert(cap[0].d2==32);
}
static void t_fader_curve_exp(void){
    /* cc_val=64, CURVE_EXP: v=127-((63*63)/127)=127-31=96, range 0..127 -> d2=96 */
    struct profile p=P(); p.fader[0].curve=CURVE_EXP; ncap=0;
    map_fader(&p,0,64,0,sink,0);
    assert(ncap==1); assert(cap[0].d2==96);
}
static void t_button_toggle_engine_silent(void){
    /* BTN_CC_TOGGLE is stateful; pure engine emits nothing on press or release */
    struct profile p=P(); p.button[2].type=BTN_CC_TOGGLE; ncap=0;
    map_button(&p,2,1,0,sink,0);
    map_button(&p,2,0,0,sink,0);
    assert(ncap==0);
}
static void t_fader_per_channel(void){
    /* v2: each fader rides its OWN channel, not the profile-wide one (mixer use). */
    struct profile p=P(); p.fader_channel[0]=0; p.fader_channel[1]=5; ncap=0;
    map_fader(&p,0,100,0,sink,0); assert(cap[0].status==(0xB0|0));
    ncap=0; map_fader(&p,1,100,0,sink,0); assert(cap[0].status==(0xB0|5));
}
static void t_button_per_channel(void){
    /* v2: each button rides its OWN channel. */
    struct profile p=P(); p.button_channel[0]=3; p.button_channel[1]=9; ncap=0;
    map_button(&p,0,1,0,sink,0); assert(cap[0].status==(0x90|3));   /* note on ch3 */
    ncap=0; map_button(&p,1,1,0,sink,0); assert(cap[0].status==(0xB0|9)); /* CC ch9 */
}
static void t_chord_accessor(void){
    struct profile p; memset(&p,0,sizeof p); p.version=PROFILE_VERSION;
    /* L0 b2 = explicit C-E-G; L1 b3 = the same; proves DIRECT layer indexing +
     * unpack into a caller buffer. */
    p.chord6[0][2] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };
    p.chord6[1][3] = (struct chord6){ .b = { 0x03, 60, 64, 67, 0, 0 } };
    struct chord_def out;
    assert(profile_layer_chord(&p, 2, 0, &out) != NULL);
    assert(out.count==3 && out.notes[0]==60);
    assert(profile_layer_chord(&p, 3, 1, &out) != NULL);
    assert(out.notes[2]==67);
    /* empty slot -> NULL (button 0 L0, button 2 L1) */
    assert(profile_layer_chord(&p, 0, 0, &out) == NULL);
    assert(profile_layer_chord(&p, 2, 1, &out) == NULL);
    /* out-of-range index guarded -> NULL */
    assert(profile_layer_chord(&p, 99, 0, &out) == NULL);
}
static void t_fader_role_accessor(void){
    struct profile p; memset(&p,0,sizeof p); p.version=PROFILE_VERSION;
    p.fader_role[0][2]=1;   /* L0 fader 2 = chord_depth */
    p.fader_role[3][0]=1;   /* L3 fader 0 = chord_depth */
    assert(profile_layer_fader_role(&p,2,0)==FADER_ROLE_CHORD_DEPTH);
    assert(profile_layer_fader_role(&p,0,0)==FADER_ROLE_CC);
    assert(profile_layer_fader_role(&p,0,3)==FADER_ROLE_CHORD_DEPTH);
    assert(profile_layer_fader_role(&p,9,0)==FADER_ROLE_CC);   /* OOB -> cc */
}
/* Feature 1: profile_layer_ccval decodes the reused chord6 slot and bounds-guards
   idx/layer exactly like profile_layer_chord (mapping.c). */
static void t_ccval_accessor(void){
    struct profile p; memset(&p,0,sizeof p); p.version=PROFILE_VERSION;
    /* L0 button 3 = set-on-press on_value 9; L3 button 4 = toggle 9<->45 */
    p.chord6[0][3] = (struct chord6){ .b = { 0, 9, 45, 0, 0, 0 } };
    p.chord6[3][4] = (struct chord6){ .b = { 0, 9, 45, 2, 0, 0 } };
    uint8_t sub,on,off;
    assert(profile_layer_ccval(&p,3,0,&sub,&on,&off)==0);
    assert(sub==0 && on==9 && off==45);
    assert(profile_layer_ccval(&p,4,3,&sub,&on,&off)==0);
    assert(sub==2 && on==9 && off==45);
    /* out-of-range idx/layer guarded -> -1 (no write past the array) */
    assert(profile_layer_ccval(&p,99,0,&sub,&on,&off)==-1);
    assert(profile_layer_ccval(&p,0,NUM_LAYERS,&sub,&on,&off)==-1);
    assert(profile_layer_ccval(&p,-1,0,&sub,&on,&off)==-1);
}
/* profile_shift_target: reads chord_flags[1]. Legacy 0 or an out-of-range value
 * (>= NUM_LAYERS) resolves to the default 1 (UI L2); explicit 1..7 are honored. */
static void t_profile_shift_target(void){
    struct profile p; memset(&p, 0, sizeof p);
    assert(profile_shift_target(&p) == 1);       /* legacy 0 -> default 1 */
    p.chord_flags[1] = 1; assert(profile_shift_target(&p) == 1);
    p.chord_flags[1] = 3; assert(profile_shift_target(&p) == 3);   /* explicit honored */
    p.chord_flags[1] = NUM_LAYERS - 1; assert(profile_shift_target(&p) == NUM_LAYERS - 1); /* 7 max */
    p.chord_flags[1] = NUM_LAYERS; assert(profile_shift_target(&p) == 1);   /* >= NUM_LAYERS -> 1 */
    p.chord_flags[1] = 200; assert(profile_shift_target(&p) == 1);          /* far out of range */
}
/* effective_layer: play_mode 0 (shift) + play_held -> the resolved shift_target;
 * otherwise the engaged gesture layer. Assignable mode (play_mode 1) NEVER shifts. */
static void t_effective_layer_shift_vs_assignable(void){
    assert(effective_layer(0, 1, 3, 1) == 1);   /* shift held -> target 1 (L2) */
    assert(effective_layer(0, 0, 3, 1) == 3);   /* shift released -> engaged */
    assert(effective_layer(0, 1, 0, 1) == 1);   /* held from L1 -> target 1 */
    assert(effective_layer(1, 1, 3, 1) == 3);   /* assignable: no shift */
    assert(effective_layer(1, 0, 2, 1) == 2);   /* assignable, released */
    /* a non-1 target lands there while held (shift mode) */
    assert(effective_layer(0, 1, 3, 4) == 4);   /* shift held -> target 4 (L5) */
    assert(effective_layer(0, 0, 3, 4) == 3);   /* released -> engaged, target ignored */
    assert(effective_layer(1, 1, 0, 4) == 0);   /* assignable never shifts to target */
    /* resolved through profile_shift_target: legacy 0 lands on 1, explicit honored */
    struct profile p; memset(&p, 0, sizeof p);
    assert(effective_layer(0, 1, 3, profile_shift_target(&p)) == 1);   /* legacy 0 -> 1 */
    p.chord_flags[1] = 5;
    assert(effective_layer(0, 1, 0, profile_shift_target(&p)) == 5);   /* explicit 5 */
}

/* Feature A (0.23): the PLAY shift is never a no-op. On the target layer, holding
   PLAY flips to home (L1 = index 0); elsewhere it goes to the target; assignable
   (play_mode 1) never shifts. */
static void t_effective_layer_toggle(void)
{
    assert(effective_layer(0, 1, 3, 3) == 0);  /* on target -> home L1 */
    assert(effective_layer(0, 1, 0, 3) == 3);  /* on home -> target */
    assert(effective_layer(0, 1, 5, 3) == 3);  /* elsewhere -> target */
    assert(effective_layer(0, 0, 5, 3) == 5);  /* not held -> gesture layer */
    assert(effective_layer(1, 1, 3, 3) == 3);  /* assignable never shifts */
    assert(effective_layer(1, 1, 0, 3) == 0);
}

int main(void){ t_fader_cc();t_fader_invert();t_fader_range();t_fader_shift_bank();
    t_button_note();t_button_cc_momentary();t_button_none_silent();
    t_fader_curve_log();t_fader_curve_exp();t_button_toggle_engine_silent();
    t_fader_per_channel();t_button_per_channel();
    t_fader_four_layers();t_button_four_layers();t_button_cc0_unbound_silent();t_select_accessors_layers_5_8();
    t_ext_accessors_per_layer();
    t_fader_per_layer_range();t_fader_per_layer_curve();
    t_fader_per_layer_invert();t_fader_per_layer_channel();t_fader_layer5_rides_own_channel();
    t_button_per_layer_type();t_button_per_layer_channel();
    t_chord_accessor();
    t_ccval_accessor();
    t_fader_role_accessor();
    t_profile_shift_target();
    t_effective_layer_shift_vs_assignable();
    t_effective_layer_toggle();
    printf("all mapping tests passed\n"); return 0; }
