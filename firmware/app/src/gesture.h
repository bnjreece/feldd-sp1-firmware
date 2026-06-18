#ifndef GESTURE_H
#define GESTURE_H
#include <stdint.h>

/* Pure, host-testable double-tap-to-latch detector for the SP-1 shift trigger.
 *
 * Stepped exactly once per main-loop scan tick (~8 ms). A PLAY press edge that
 * lands within DOUBLE_TAP_WINDOW_SCANS of the previous PLAY press toggles the
 * shift latch on/off. Time is expressed in scan ticks (NOT wall-clock) so the
 * same translation unit links into both the firmware and the host test, exactly
 * like control_logic.c / buttons.c's pure core.
 *
 * This is the TEMPORARY shift-trigger gesture: it intentionally lives on PLAY
 * (button index 0), not the overloaded •• button, so it does not collide with
 * the •• tap(next-profile)/hold(power-off) gestures. It is expected to move to
 * its own gesture after bench testing. */

/* Two PLAY presses within this many scan ticks count as a double-tap. The loop
 * runs ~8 ms/tick, so 45 ticks ~= 360 ms. Must stay comfortably above the
 * ~24 ms (3-tick) ladder debounce floor (buttons.c) so a real tap-release-tap
 * can be resolved. Bench-tunable once the Play plateau is calibrated. */
#define DOUBLE_TAP_WINDOW_SCANS 45

/* gesture_step() return codes. */
#define GESTURE_NONE          0
#define GESTURE_LATCH_TOGGLED 1

typedef struct {
    uint8_t  latched;     /* current shift-latch state (0/1) */
    uint8_t  armed;       /* a first PLAY press is waiting for a possible second */
    uint16_t since_press; /* scan ticks since the last PLAY press (saturating) */
} gesture_t;

void gesture_init(gesture_t *g);

/* Advance one scan tick. play_press is 1 on the tick a PLAY press edge was seen,
 * else 0. Returns GESTURE_LATCH_TOGGLED on the tick a double-tap completes a
 * toggle, else GESTURE_NONE. */
int gesture_step(gesture_t *g, int play_press);

/* Current latch state (0/1) — feed this as the `shift` arg into the mapping. */
int gesture_latched(const gesture_t *g);

#endif /* GESTURE_H */
