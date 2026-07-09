#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cc_value.h"

/* pack writes the pinned order [0, on, off, sub, 0, 0]. */
static void t_pack_order(void){
    struct chord6 c; memset(&c, 0xAB, sizeof c);
    cc_value_pack(2, 9, 45, &c);
    assert(c.b[0]==0 && c.b[1]==9 && c.b[2]==45 && c.b[3]==2 && c.b[4]==0 && c.b[5]==0);
}
/* unpack reads b[3]=sub, b[1]=on, b[2]=off; returns 0. */
static void t_unpack_fields(void){
    struct chord6 c = { .b = { 0, 12, 34, 1, 0, 0 } };
    uint8_t sub=99,on=99,off=99;
    assert(cc_value_unpack(&c,&sub,&on,&off)==0);
    assert(sub==1 && on==12 && off==34);
}
static void t_unpack_null(void){
    uint8_t sub,on,off;
    assert(cc_value_unpack(0,&sub,&on,&off)==-1);
}
/* pack -> unpack round-trips across all three sub-modes. */
static void t_round_trip(void){
    for (uint8_t sub=0; sub<=2; sub++){
        struct chord6 c; cc_value_pack(sub, 7, 100, &c);
        uint8_t s,o,f; assert(cc_value_unpack(&c,&s,&o,&f)==0);
        assert(s==sub && o==7 && f==100);
    }
}
/* set-on-press (sub 0): send `on` on press, NOTHING on release; value stays. */
static void t_decide_set_on_press(void){
    struct btn_toggle t; btn_toggle_init(&t);
    uint8_t d2=0xFF;
    assert(cc_value_decide(0, /*pressed*/1, 9, 45, &t, 0, 3, &d2)==1 && d2==9);
    assert(cc_value_decide(0, /*pressed*/0, 9, 45, &t, 0, 3, &d2)==0);   /* no release send */
}
/* momentary (sub 1): send `on` on press, `off` on release (both edges emit). */
static void t_decide_momentary(void){
    struct btn_toggle t; btn_toggle_init(&t);
    uint8_t d2=0;
    assert(cc_value_decide(1, 1, 9, 45, &t, 0, 3, &d2)==1 && d2==9);
    assert(cc_value_decide(1, 0, 9, 45, &t, 0, 3, &d2)==1 && d2==45);
}
/* toggle (sub 2): press-edge only; flips latch; first press sends `on`. */
static void t_decide_toggle(void){
    struct btn_toggle t; btn_toggle_init(&t);
    uint8_t d2=0;
    assert(cc_value_decide(2, 1, 9, 45, &t, 2, 4, &d2)==1 && d2==9);   /* 1st press -> on  */
    assert(cc_value_decide(2, 0, 9, 45, &t, 2, 4, &d2)==0);            /* release: nothing */
    assert(cc_value_decide(2, 1, 9, 45, &t, 2, 4, &d2)==1 && d2==45);  /* 2nd press -> off */
    assert(cc_value_decide(2, 1, 9, 45, &t, 2, 4, &d2)==1 && d2==9);   /* 3rd press -> on  */
}
/* device-blind latch: FIRST press after btn_toggle_reset_all sends `on` (value A),
   documenting the swallowed-no-op-if-already-A behavior (spec Section 2). */
static void t_decide_toggle_first_press_after_reset(void){
    struct btn_toggle t; btn_toggle_init(&t);
    uint8_t d2=0;
    (void)cc_value_decide(2, 1, 9, 45, &t, 1, 5, &d2);   /* -> on */
    (void)cc_value_decide(2, 1, 9, 45, &t, 1, 5, &d2);   /* -> off */
    btn_toggle_reset_all(&t);
    assert(cc_value_decide(2, 1, 9, 45, &t, 1, 5, &d2)==1 && d2==9);  /* back to A after reset */
}
int main(void){ t_pack_order(); t_unpack_fields(); t_unpack_null(); t_round_trip();
    t_decide_set_on_press(); t_decide_momentary(); t_decide_toggle();
    t_decide_toggle_first_press_after_reset();
    printf("all cc_value tests passed\n"); return 0; }
