// midi_rt_ring.c - two-tier priority byte queue for TRS MIDI TX. Pure; host-tested.
#include "midi_rt_ring.h"

#define RT_MASK 7    // rt[] is 8 slots, power of two
#define NB_MASK 127  // nb[] is 128 slots, power of two

void midi_rt_ring_init(struct midi_rt_ring *r)
{
    r->rt_h = r->rt_t = 0;
    r->nb_h = r->nb_t = 0;
}

bool midi_rt_put_rt(struct midi_rt_ring *r, uint8_t b)
{
    uint8_t nt = (uint8_t)((r->rt_t + 1) & RT_MASK);
    if (nt == r->rt_h) {
        return false;               // full: refuse without clobbering
    }
    r->rt[r->rt_t] = b;
    r->rt_t = nt;
    return true;
}

bool midi_rt_put(struct midi_rt_ring *r, uint8_t b)
{
    uint8_t nt = (uint8_t)((r->nb_t + 1) & NB_MASK);
    if (nt == r->nb_h) {
        return false;               // full: refuse without clobbering
    }
    r->nb[r->nb_t] = b;
    r->nb_t = nt;
    return true;
}

bool midi_rt_next(struct midi_rt_ring *r, uint8_t *out)
{
    if (r->rt_h != r->rt_t) {       // real-time ring first
        *out = r->rt[r->rt_h];
        r->rt_h = (uint8_t)((r->rt_h + 1) & RT_MASK);
        return true;
    }
    if (r->nb_h != r->nb_t) {        // then normal ring
        *out = r->nb[r->nb_h];
        r->nb_h = (uint8_t)((r->nb_h + 1) & NB_MASK);
        return true;
    }
    return false;                    // both empty
}
