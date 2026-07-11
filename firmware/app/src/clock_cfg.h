#ifndef CLOCK_CFG_H
#define CLOCK_CFG_H
#include <stdint.h>
/* clock_cfg - per-profile MIDI-clock config codec. Packs into 2 bytes (the v9
 * profile pad bytes). out[0] carries enable (bit7), tap-tempo button index
 * (bits 3..6) and BPM-fader index (bits 0..2); out[1] carries default BPM.
 * tap_button 0..8 or CLOCK_TAP_NONE (no button); bpm_fader 0..3 or
 * CLOCK_FADER_NONE (no fader). Pure; host-testable. */

#define CLOCK_TAP_NONE   0x0Fu   /* no tap-tempo button assigned */
#define CLOCK_FADER_NONE 0x07u   /* no BPM fader assigned */

struct clock_cfg {
    uint8_t enable;       /* 0/1 */
    uint8_t tap_button;   /* 0..8 or CLOCK_TAP_NONE */
    uint8_t bpm_fader;    /* 0..3 or CLOCK_FADER_NONE */
    uint8_t default_bpm;  /* 40..240 */
};

void clock_cfg_pack(const struct clock_cfg *c, uint8_t out[2]);
void clock_cfg_unpack(const uint8_t in[2], struct clock_cfg *c);

#endif
