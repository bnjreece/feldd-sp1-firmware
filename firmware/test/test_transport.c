#include <assert.h>
#include <stdio.h>
#include "../src/transport.h"

/* System real-time status bytes the gear responds to. */
#define RT_START    0xFA
#define RT_CONTINUE 0xFB
#define RT_STOP     0xFC

static void t_explicit_start(void) {
    int playing = 0;
    /* value 1 = explicit START, regardless of prior state */
    assert(transport_rt(1, &playing) == RT_START);
    assert(playing == 1);
    /* still START even if already playing */
    assert(transport_rt(1, &playing) == RT_START);
    assert(playing == 1);
}

static void t_explicit_stop(void) {
    int playing = 1;
    /* value 2 = explicit STOP, regardless of prior state */
    assert(transport_rt(2, &playing) == RT_STOP);
    assert(playing == 0);
    assert(transport_rt(2, &playing) == RT_STOP);
    assert(playing == 0);
}

static void t_explicit_continue(void) {
    int playing = 0;
    /* value 3 = explicit CONTINUE -> playing */
    assert(transport_rt(3, &playing) == RT_CONTINUE);
    assert(playing == 1);
}

static void t_toggle_from_stopped(void) {
    int playing = 0;
    /* value 0 = play/stop TOGGLE: stopped -> START + playing */
    assert(transport_rt(0, &playing) == RT_START);
    assert(playing == 1);
    /* toggle again: playing -> STOP + stopped */
    assert(transport_rt(0, &playing) == RT_STOP);
    assert(playing == 0);
    /* and back: stopped -> START */
    assert(transport_rt(0, &playing) == RT_START);
    assert(playing == 1);
}

int main(void) {
    t_explicit_start();
    t_explicit_stop();
    t_explicit_continue();
    t_toggle_from_stopped();
    printf("all transport tests passed\n");
    return 0;
}
