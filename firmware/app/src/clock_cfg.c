// clock_cfg.c - per-profile MIDI-clock config <-> 2 v9 profile pad bytes.
#include "clock_cfg.h"

void clock_cfg_pack(const struct clock_cfg *c, uint8_t out[2])
{
    out[0] = (uint8_t)((c->enable ? 0x80u : 0u)
                       | ((c->tap_button & 0x0Fu) << 3)
                       | (c->bpm_fader & 0x07u));
    out[1] = c->default_bpm;
}

void clock_cfg_unpack(const uint8_t in[2], struct clock_cfg *c)
{
    c->enable      = (uint8_t)((in[0] >> 7) & 0x01u);
    c->tap_button  = (uint8_t)((in[0] >> 3) & 0x0Fu);
    c->bpm_fader   = (uint8_t)(in[0] & 0x07u);
    c->default_bpm = in[1];
}
