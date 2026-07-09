#include <assert.h>
#include <stdio.h>
#include "panic.h"

/* panic_fill writes CC 120 / 121 / 123 (val 0) on every channel 0..15, in order. */
static void t_panic_fill(void)
{
    struct midi_msg m[48];
    assert(panic_fill(m, 48) == 48);
    assert(panic_fill(m, 47) == 0);   /* too small -> nothing written */
    for (int ch = 0; ch < 16; ch++) {
        for (int i = 0; i < 3; i++) {
            struct midi_msg *x = &m[ch * 3 + i];
            assert(x->status == (0xB0 | ch));
            assert(x->d2 == 0 && x->len == 3);
        }
        assert(m[ch * 3 + 0].d1 == 120);   /* all sound off */
        assert(m[ch * 3 + 1].d1 == 121);   /* reset all controllers */
        assert(m[ch * 3 + 2].d1 == 123);   /* all notes off */
    }
}
int main(void){ t_panic_fill(); printf("all panic tests passed\n"); return 0; }
