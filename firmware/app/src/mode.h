#ifndef MODE_H
#define MODE_H
#include <stdint.h>

/* Pure, host-testable device-mode model for feldd phase 2b.
 *
 * Device mode is one global byte (persisted in NVS by the librarian, NOT in any
 * profile). An N-mode enum: MIDI, KEYBOARD, and LOOPER ship; value 3 remains
 * reserved for a future personality. As of the 0.9.1 LED redesign
 * the SIDE (PLAY-LED) row no longer renders a STATIC mode pattern: it permanently
 * shows the LAYER (one LED at gesture_layer()), and mode_led_pattern() is now the
 * source for a TEMPORARY MODE-FLASH that plays for ~1.2 s on a switch and then
 * yields back to the layer LED (side_led.c / side_led_pattern, armed via
 * mode_flash_ticks in main.c). There is no resting mode indicator anymore.
 * Feature 4 retired the mode_decide hold-plateau gesture: the mode flip is now a
 * genuine dot-dot+T4 combo toggle (mode_toggle + combo_dispatch below), so this
 * unit is the pure mode enum + LED table + the combo engine. */

enum device_mode { MODE_MIDI = 0, MODE_KEYBOARD = 1, MODE_LOOPER = 2 };

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

/* The SIDE-row (PLAY-LED) on/off pattern for `mode` (an OR of MODE_LED* bits, each
 * mapping to SP1_PLAY_LED1..4 — see the HARDWARE CONTRACT above). Used as the
 * MODE-FLASH pattern source (0.9.1: a temporary flash on a switch, not a resting
 * indicator). MIDI = 1+4, KEYBOARD = 2+3, LOOPER = 1+2, mode 3 reserved (3+4);
 * out-of-range clamps to the MIDI pattern. Raw on/off — no PWM, no pulsing. */
uint8_t mode_led_pattern(int mode);

/* ---- Feature 4: dot-dot (••) Fn-modifier combo engine ----
 * While •• is held (func_down), a PRESS on the combo ladder buttons carries an
 * action: T4 cycles MODE, Vol+/- cycle the profile, FWD/RWD step
 * the layer. The press is consumed (latched per index) and, whatever the crossing
 * ordering, the matching RELEASE is swallowed IFF that index's latch bit is set -
 * never gated on the instantaneous func_down at the release edge (spec (h)1). Any
 * consumed press signals reset_hold so main.c voids the •• power-off hold (clean-
 * hold gate, spec (b)4). This retires mode_decide's press logic; the toggle is a
 * genuine flip fired once on the press so Keyboard stays reachable and cannot
 * double-fire. */
enum combo_action { COMBO_NONE=0, COMBO_MODE_TOGGLE=1, COMBO_PROFILE_NEXT=2,
                    COMBO_PROFILE_PREV=3, COMBO_LAYER_NEXT=4, COMBO_LAYER_PREV=5,
                    COMBO_BATTERY=6, COMBO_BRIGHTNESS=7, COMBO_PANIC=8 };
#define COMBO_BTN_T1 1
#define COMBO_BTN_T2 2
#define COMBO_BTN_T3 3
#define COMBO_BTN_T4 4
#define COMBO_BTN_VOLUP 5
#define COMBO_BTN_VOLDN 6
#define COMBO_BTN_FWD 7
#define COMBO_BTN_RWD 8
typedef struct { uint16_t latched; } combo_latch_t;   /* bit i = idx i consumed-as-combo */
struct combo_decision { uint8_t action; uint8_t consumed; uint8_t reset_hold; };
void combo_latch_init(combo_latch_t *l);
struct combo_decision combo_dispatch(combo_latch_t *l, int func_down, int idx, int pressed);
uint8_t mode_toggle(uint8_t cur);

#endif /* MODE_H */
