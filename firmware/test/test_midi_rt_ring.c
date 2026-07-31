#include <assert.h>
#include <stdio.h>
#include "../src/midi_rt_ring.h"

// put A,B,C (normal) then put_rt X,Y; next yields X,Y,A,B,C then false.
static void test_priority_order(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    assert(midi_rt_put(&r, 'A'));
    assert(midi_rt_put(&r, 'B'));
    assert(midi_rt_put(&r, 'C'));
    assert(midi_rt_put_rt(&r, 'X'));
    assert(midi_rt_put_rt(&r, 'Y'));
    uint8_t b;
    assert(midi_rt_next(&r, &b) && b == 'X');   // real-time drains first
    assert(midi_rt_next(&r, &b) && b == 'Y');
    assert(midi_rt_next(&r, &b) && b == 'A');   // then normal, in order
    assert(midi_rt_next(&r, &b) && b == 'B');
    assert(midi_rt_next(&r, &b) && b == 'C');
    assert(!midi_rt_next(&r, &b));              // both empty
}

// real-time bytes preserve FIFO among themselves.
static void test_rt_fifo(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    assert(midi_rt_put_rt(&r, 0xF8));
    assert(midi_rt_put_rt(&r, 0xFA));
    assert(midi_rt_put_rt(&r, 0xFC));
    uint8_t b;
    assert(midi_rt_next(&r, &b) && b == 0xF8);
    assert(midi_rt_next(&r, &b) && b == 0xFA);
    assert(midi_rt_next(&r, &b) && b == 0xFC);
    assert(!midi_rt_next(&r, &b));
}

// normal bytes preserve FIFO among themselves.
static void test_normal_fifo(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    for (uint8_t i = 0; i < 10; i++) assert(midi_rt_put(&r, i));
    uint8_t b;
    for (uint8_t i = 0; i < 10; i++) {
        assert(midi_rt_next(&r, &b) && b == i);
    }
    assert(!midi_rt_next(&r, &b));
}

// filling the normal ring past capacity returns false and does not clobber.
static void test_normal_full_no_clobber(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    // capacity is size-1 = 511 (one slot kept open)
    for (uint16_t i = 0; i < 511; i++) assert(midi_rt_put(&r, (uint8_t)i));
    assert(!midi_rt_put(&r, 0xEE));   // full: refuse
    assert(!midi_rt_put(&r, 0xEF));   // still full
    // drain: original 511 bytes intact, in order, no clobber from refused puts
    uint8_t b;
    for (uint16_t i = 0; i < 511; i++) {
        assert(midi_rt_next(&r, &b) && b == (uint8_t)i);
    }
    assert(!midi_rt_next(&r, &b));
}

// filling the real-time ring past capacity returns false and does not clobber.
static void test_rt_full_no_clobber(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    for (uint8_t i = 0; i < 7; i++) assert(midi_rt_put_rt(&r, (uint8_t)(0xF8 + i)));
    assert(!midi_rt_put_rt(&r, 0xFF));   // full: refuse
    uint8_t b;
    for (uint8_t i = 0; i < 7; i++) {
        assert(midi_rt_next(&r, &b) && b == (uint8_t)(0xF8 + i));
    }
    assert(!midi_rt_next(&r, &b));
}

// init leaves it empty (next -> false).
static void test_init_empty(void) {
    struct midi_rt_ring r;
    midi_rt_ring_init(&r);
    uint8_t b;
    assert(!midi_rt_next(&r, &b));
}

int main(void) {
    test_priority_order();
    test_rt_fifo();
    test_normal_fifo();
    test_normal_full_no_clobber();
    test_rt_full_no_clobber();
    test_init_empty();
    printf("all midi_rt_ring tests passed\n");
    return 0;
}
