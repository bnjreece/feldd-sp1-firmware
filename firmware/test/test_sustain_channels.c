#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mapping.h"

/* An empty profile: every fader and button unbound (CC 0, BTN_NONE), channel 0. */
static struct profile blank(void)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;
    return p;
}

static void t_none(void)
{
    struct profile p = blank();
    assert(profile_sustain_channels(&p) == 0);
    assert(profile_sustain_channels(0) == 0);
}

/* L1 fader 2 on CC64, channel 7 (wire 6) -- the step-0 bench profile's F2. */
static void t_fader_l1(void)
{
    struct profile p = blank();
    p.fader[1].cc = 64;
    p.fader_channel[1] = 6;
    assert(profile_sustain_channels(&p) == (1u << 6));
}

/* A fader whose ROLE is chord depth does not send its CC, so it is not sustain. */
static void t_fader_role_depth_ignored(void)
{
    struct profile p = blank();
    p.fader[0].cc = 64;
    p.fader_role[0][0] = FADER_ROLE_CHORD_DEPTH;
    assert(profile_sustain_channels(&p) == 0);
}

/* Any layer counts: L2 (shift) and L5 (layer[2]) on two other channels. */
static void t_other_layers(void)
{
    struct profile p = blank();
    p.shift.fader_cc[3] = 64;
    p.ext[0].fader_channel[3] = 9;           /* L2's channels live in ext[0] */
    p.layer[2].fader_cc[0] = 64;
    p.ext[3].fader_channel[0] = 15;          /* L5 = ext[3] */
    assert(profile_sustain_channels(&p) == ((1u << 9) | (1u << 15)));
}

/* CC buttons count when their value is 64; notes and chords on 64 do not. */
static void t_buttons(void)
{
    struct profile p = blank();
    p.button[1].type = BTN_CC_TOGGLE;    p.button[1].value = 64; p.button_channel[1] = 0;
    p.button[2].type = BTN_CC_MOMENTARY; p.button[2].value = 64; p.button_channel[2] = 3;
    p.button[3].type = BTN_CC_VALUE;     p.button[3].value = 64; p.button_channel[3] = 4;
    p.button[4].type = BTN_NOTE;         p.button[4].value = 64; p.button_channel[4] = 5;
    p.button[5].type = BTN_CC_TOGGLE;    p.button[5].value = 65; p.button_channel[5] = 8;
    assert(profile_sustain_channels(&p) == ((1u << 0) | (1u << 3) | (1u << 4)));
}

/* A sustain fader whose channel byte carries high bits still maps to its nibble. */
static void t_channel_nibble(void)
{
    struct profile p = blank();
    p.fader[0].cc = 64;
    p.fader_channel[0] = 0x1A;
    assert(profile_sustain_channels(&p) == (1u << 0x0A));
}

int main(void)
{
    t_none();
    t_fader_l1();
    t_fader_role_depth_ignored();
    t_other_layers();
    t_buttons();
    t_channel_nibble();
    printf("test_sustain_channels: OK\n");
    return 0;
}
