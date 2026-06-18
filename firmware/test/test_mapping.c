#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mapping.h"

static struct midi_msg cap[8]; static int ncap;
static void sink(const struct midi_msg *m, void *ctx){ (void)ctx; cap[ncap++]=*m; }

static struct profile P(void){
    struct profile p; memset(&p,0,sizeof p);
    p.version=PROFILE_VERSION; p.channel=2;
    for(int i=0;i<NUM_FADERS;i++){ p.fader[i]=(struct fader_map){.cc=(uint8_t)(7+i),.min=0,.max=127,.curve=CURVE_LINEAR,.invert=0}; p.shift.fader_cc[i]=(uint8_t)(20+i);}
    p.button[0]=(struct button_map){.type=BTN_NOTE,.value=60};
    p.button[1]=(struct button_map){.type=BTN_CC_MOMENTARY,.value=64};
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
    map_fader(&p,0,50,1,sink,0);
    assert(cap[0].d1==20);   /* shift layer uses shift.fader_cc[0] */
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
int main(void){ t_fader_cc();t_fader_invert();t_fader_range();t_fader_shift_bank();
    t_button_note();t_button_cc_momentary();t_button_none_silent();
    t_fader_curve_log();t_fader_curve_exp();t_button_toggle_engine_silent();
    printf("all mapping tests passed\n"); return 0; }
