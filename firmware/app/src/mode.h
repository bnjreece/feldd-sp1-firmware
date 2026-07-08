#ifndef MODE_H
#define MODE_H
#include <stdint.h>

/* Pure, host-testable device-mode model for feldd phase 2b.
 *
 * Device mode is one global byte (persisted in NVS by the librarian, NOT in any
 * profile). An N-mode enum: MIDI and KEYBOARD ship; values 2,3 are reserved for
 * future "personalities" (the Midipal north star). As of the 0.9.1 LED redesign
 * the SIDE (PLAY-LED) row no longer renders a STATIC mode pattern: it permanently
 * shows the LAYER (one LED at gesture_layer()), and mode_led_pattern() is now the
 * source for a TEMPORARY MODE-FLASH that plays for ~1.2 s on a switch and then
 * yields back to the layer LED (side_led.c / side_led_pattern, armed via
 * mode_flash_ticks in main.c). There is no resting mode indicator anymore. The
 * toggle gesture (PLAY held + tap FWD/RWD) decision is mode_decide(); main.c owns
 * the gesture_disarm + librarian write — this unit is pure, modifier-agnostic
 * logic only. */

enum device_mode { MODE_MIDI = 0, MODE_KEYBOARD = 1 };

/* The 4 mode-indicator LED bits in a pattern byte (LED1..LED4 left-to-right).
 *
 * HARDWARE CONTRACT — these 4 bits render on the SIDE (PLAY-LED) row. As of the
 * 0.9.1 LED redesign they are the TEMPORARY MODE-FLASH pattern (no longer a static
 * resting indicator): on a mode switch the side row flashes this pattern for
 * ~1.2 s, then returns to the permanent LAYER LED. That layer indicator now spans
 * 8 layers: SOLID for layers 1-4 and BLINK for layers 5-8 at the same LED position
 * (layer & 3), see side_led_pattern. All four play LEDs
 * (SP1_PLAY_LED1..4 in sp1_board.h) are driven; all four are SYNTH-clock pins,
 * already driven always-on by 0.9.0, GPIO-only and Track1+4-DFU-recoverable (NOT a
 * brick). An integrator MUST map these bits onto the side row:
 *   MODE_LED1 -> SP1_PLAY_LED1 (P0.01), MODE_LED2 -> SP1_PLAY_LED2 (P1.12),
 *   MODE_LED3 -> SP1_PLAY_LED3 (P0.00), MODE_LED4 -> SP1_PLAY_LED4 (P1.13).
 * With these positions: MIDI = LEDs 1+4, KEYBOARD = LEDs 2+3 (spec §7.1).
 *
 * The -DFELDD_MODE_LED_SINGLE build-time fallback collapses the whole side
 * indicator (layer LED + mode-flash) onto a single side LED (SP1_LED1 = P1.13)
 * and does not drive the other three side pins. The TRACK row
 * (SP1_TRACK_LED1..4) is reserved for interaction (plain button-press feedback +
 * profile dial/peek + boot sweep + DFU), NOT the mode pattern. */
#define MODE_LED1 0x01u
#define MODE_LED2 0x02u
#define MODE_LED3 0x04u
#define MODE_LED4 0x08u

/* The ladder indices the mode gesture watches (buttons.c order). */
#define MODE_BTN_FWD 7   /* •• + FWD -> KEYBOARD */
#define MODE_BTN_RWD 8   /* •• + RWD -> MIDI     */

/* PLAY must be held at least this many scan ticks before a FWD/RWD edge is taken
 * as a mode flip (else it passes through to transport). Guards the OP-XY 8-track
 * collision (spec §4.3): a quick track-select PLAY tap coincident with a rocker
 * edge must NOT silently flip MIDI<->Keyboard. Value MATCHES the peek plateau
 * PEEK_HOLD_SCANS (gesture.h); main.c BUILD_ASSERTs the two are equal. */
#define MODE_FLIP_ARM_SCANS 75

struct mode_decision {
    uint8_t set;       /* 1 -> caller calls librarian_set_mode(which) */
    uint8_t which;     /* MODE_MIDI / MODE_KEYBOARD (valid iff set) */
    uint8_t consumed;  /* 1 -> caller MUST NOT map_button/usb_hid this edge, and
                        *      MUST gesture_disarm() the shift detector so the held
                        *      PLAY press can't complete a stray shift double-tap */
};

/* Decide what a single button edge means given the modifier state THIS tick.
 *   modifier_hold_ticks : scan ticks the modifier (PLAY) has been continuously
 *                         held this hold (0 = not held); a FWD/RWD PRESS is a mode
 *                         flip only once this reaches MODE_FLIP_ARM_SCANS. Below
 *                         the plateau the edge is passthrough (transport).
 *   idx                 : the logical button index of the edge (0..8).
 *   pressed             : 1 press edge, 0 release edge.
 * A FWD/RWD PRESS while the modifier is held PAST the arm plateau sets the mode
 * and consumes the edge. The matching RELEASE while armed is consumed (so it never
 * types/maps) but sets nothing. Anything else is passthrough (set=0, consumed=0).
 * This logic is modifier-agnostic: main.c supplies the PLAY hold-tick count. */
struct mode_decision mode_decide(int modifier_hold_ticks, int idx, int pressed);

/* The SIDE-row (PLAY-LED) on/off pattern for `mode` (an OR of MODE_LED* bits, each
 * mapping to SP1_PLAY_LED1..4 — see the HARDWARE CONTRACT above). Used as the
 * MODE-FLASH pattern source (0.9.1: a temporary flash on a switch, not a resting
 * indicator). MIDI = 1+4, KEYBOARD = 2+3, modes 2/3 reserved (1+2, 3+4);
 * out-of-range clamps to the MIDI pattern. Raw on/off — no PWM, no pulsing. */
uint8_t mode_led_pattern(int mode);

#endif /* MODE_H */
