#include <stddef.h>
#include <string.h>
#include "cc_value.h"

void cc_value_pack(uint8_t sub, uint8_t on, uint8_t off, struct chord6 *out)
{
    memset(out, 0, sizeof *out);
    out->b[1] = on;
    out->b[2] = off;
    out->b[3] = sub;   /* b[0], b[4], b[5] stay 0 */
}

int cc_value_unpack(const struct chord6 *in, uint8_t *sub, uint8_t *on, uint8_t *off)
{
    if (in == NULL) return -1;
    *sub = in->b[3];
    *on  = in->b[1];
    *off = in->b[2];
    return 0;
}

int cc_value_decide(uint8_t sub, int pressed, uint8_t on, uint8_t off,
                    struct btn_toggle *t, int layer, int idx, uint8_t *d2)
{
    if (sub == 2) {                     /* toggle: press-edge only, stateful */
        if (!pressed) return 0;
        *d2 = btn_toggle_flip(t, layer, idx) ? on : off;
        return 1;
    }
    if (sub == 1) {                     /* momentary: both edges */
        *d2 = pressed ? on : off;
        return 1;
    }
    if (!pressed) return 0;             /* set-on-press (sub 0 or any other): press only */
    *d2 = on;
    return 1;
}
