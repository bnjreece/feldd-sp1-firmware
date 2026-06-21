#ifndef TRANSPORT_H
#define TRANSPORT_H
/* Pure transport decision for BTN_TRANSPORT buttons. The gear feldd targets
 * (OP-XY, Dirtywave M8, Roland Aira, TP-7) follows MIDI SYSTEM REAL-TIME
 * transport, not MMC SysEx, so a transport press becomes a single-byte real-time
 * status: Start 0xFA, Continue 0xFB, Stop 0xFC.
 *
 * The button's `value` selects the behavior; *playing tracks the current run
 * state (the control loop owns the storage, this helper updates it):
 *   1 -> Start    (0xFA), *playing = 1
 *   2 -> Stop     (0xFC), *playing = 0
 *   3 -> Continue (0xFB), *playing = 1
 *   0 -> play/stop TOGGLE: if *playing -> Stop (0xFC), *playing = 0;
 *                          else        -> Start (0xFA), *playing = 1
 * Returns the real-time status byte to emit (feed straight to midi1_event /
 * trs_send as a len-1 message). Pure; host-testable. */
#include <stdint.h>
uint8_t transport_rt(unsigned char value, int *playing);
#endif
