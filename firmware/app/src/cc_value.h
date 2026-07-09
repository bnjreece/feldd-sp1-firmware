#ifndef CC_VALUE_H
#define CC_VALUE_H
#include <stdint.h>
#include "profile.h"      /* struct chord6 */
#include "btn_toggle.h"   /* struct btn_toggle (used by cc_value_decide) */

/* Pack {sub_mode,on,off} into the reused chord6 slot: [0, on, off, sub, 0, 0].
 * The b[0]=0 + b[4]=b[5]=0 pinning makes the slot decode as an EMPTY chord to any
 * reader that does not understand BTN_CC_VALUE (chord6_is_empty -> true). */
void cc_value_pack(uint8_t sub, uint8_t on, uint8_t off, struct chord6 *out);

/* Read a cc_value slot: *on=b[1], *off=b[2], *sub=b[3]. Returns 0, or -1 if in==NULL. */
int cc_value_unpack(const struct chord6 *in, uint8_t *sub, uint8_t *on, uint8_t *off);

/* Decide the CC value to send for one button edge. sub: 0=set-on-press,
 * 1=momentary, 2=toggle. For toggle, flips t->bit[layer][idx] and uses the NEW
 * state (device-blind: first press after btn_toggle_reset_all sends `on`). Writes
 * *d2 with the value to send. Returns 1 if a MIDI message should be sent, else 0
 * (set-on-press release, toggle release). Momentary emits on BOTH edges. */
int cc_value_decide(uint8_t sub, int pressed, uint8_t on, uint8_t off,
                    struct btn_toggle *t, int layer, int idx, uint8_t *d2);

#endif
