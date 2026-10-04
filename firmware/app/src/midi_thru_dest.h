#ifndef MIDI_THRU_DEST_H
#define MIDI_THRU_DEST_H
#include <stdint.h>

/* Where a thru'd USB-in channel-voice message should go.
 *
 * WHY A MASK AND NOT TWO CALLS. midi_out_thru() is invoked from the USB class
 * OUT completion, once per event in a buffer that can hold sixteen. Deciding
 * destinations once per BUFFER and passing the answer down keeps the per-message
 * path a bit test, and -- more importantly -- keeps the two sinks in ONE call so
 * they cannot drift apart as the file is edited later.
 *
 * WHY THIS IS ITS OWN PURE HEADER. The interesting part is not the send, it is
 * the policy: two independent switches, and the empty case that must add no work
 * on the default path (both off, which is feldd's shipped state -- it is a
 * control surface, not a MIDI sink). That is worth testing on a host, and the
 * Zephyr I/O around it is not.
 *
 * NOT USB. A thru'd message never goes back out USB: the stream came FROM the
 * USB host, and echoing it would loop. BLE is a genuinely different transport
 * with no MIDI-in path in feldd, so there is no loop to close -- the one real
 * caveat is a host connected over BOTH links at once, which is exactly why the
 * two switches are independent rather than one.
 */
#define MIDI_THRU_DEST_NONE 0x00u
#define MIDI_THRU_DEST_TRS  0x01u   /* the 3.5 mm jack, via the TRS ring */
#define MIDI_THRU_DEST_BLE  0x02u   /* the BLE-MIDI link, when a host is subscribed */

/* Compose the destination mask from the two persisted switches. Any nonzero
 * input is treated as on, so a caller may pass a raw NVS byte without first
 * normalising it -- the librarian range-checks on write, but a store written by
 * some future firmware should not be able to turn a 2 into "neither". */
static inline uint8_t midi_thru_dest(int trs_on, int ble_on)
{
    return (uint8_t)((trs_on ? MIDI_THRU_DEST_TRS : 0u) |
                     (ble_on ? MIDI_THRU_DEST_BLE : 0u));
}

#endif /* MIDI_THRU_DEST_H */
