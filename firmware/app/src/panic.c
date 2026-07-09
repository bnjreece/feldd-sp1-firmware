#include "panic.h"

int panic_fill(struct midi_msg *out, int cap)
{
    if (cap < 48) return 0;
    static const unsigned char ccs[3] = { 120, 121, 123 };
    int n = 0;
    for (int ch = 0; ch < 16; ch++) {
        for (int i = 0; i < 3; i++) {
            out[n].status = (unsigned char)(0xB0 | (ch & 0x0F));
            out[n].d1     = ccs[i];
            out[n].d2     = 0;
            out[n].len    = 3;
            n++;
        }
    }
    return n;
}
