#include "gesture.h"

void gesture_init(gesture_t *g) {
    g->latched = 0;
    g->armed = 0;
    g->since_press = 0;
}

int gesture_step(gesture_t *g, int play_press) {
    /* advance the inter-tap timer first (saturate so it can't wrap) */
    if (g->since_press < 0xFFFF) {
        g->since_press++;
    }
    /* a pending first tap expires once the window passes with no second tap */
    if (g->armed && g->since_press > DOUBLE_TAP_WINDOW_SCANS) {
        g->armed = 0;
    }

    if (!play_press) {
        return GESTURE_NONE;
    }

    /* a PLAY press landed this tick */
    int toggled = 0;
    if (g->armed && g->since_press <= DOUBLE_TAP_WINDOW_SCANS) {
        /* second tap inside the window -> toggle the shift latch */
        g->latched = g->latched ? 0 : 1;
        g->armed = 0;
        toggled = 1;
    } else {
        /* first tap (or too-late tap) -> arm and wait for a possible second */
        g->armed = 1;
    }
    g->since_press = 0;
    return toggled ? GESTURE_LATCH_TOGGLED : GESTURE_NONE;
}

int gesture_latched(const gesture_t *g) {
    return g->latched ? 1 : 0;
}
