#ifndef PROFILE_H
#define PROFILE_H
#include <stdint.h>
#define PROFILE_VERSION 1
#define NUM_FADERS 4
#define NUM_BUTTONS 9
enum fader_curve { CURVE_LINEAR=0, CURVE_LOG=1, CURVE_EXP=2 };
/* Button behaviors. NOTE and CC_MOMENTARY are realized by the pure mapping
 * engine (it emits the MIDI directly). CC_TOGGLE (latching on/off), TRANSPORT,
 * and PROFILE_SWITCH are STATEFUL and realized by the firmware control loop,
 * which tracks their state and emits MIDI itself; the mapping engine emits
 * nothing for them. */
enum btn_type { BTN_NONE=0, BTN_NOTE=1, BTN_CC_TOGGLE=2, BTN_CC_MOMENTARY=3,
                BTN_TRANSPORT=4, BTN_PROFILE_SWITCH=5 };
struct fader_map { uint8_t cc, min, max, curve, invert; };
struct button_map { uint8_t type, value; };
struct profile {
    uint8_t version;
    uint8_t channel;                       /* 0..15 */
    struct fader_map  fader[NUM_FADERS];
    struct button_map button[NUM_BUTTONS];
    struct { uint8_t fader_cc[NUM_FADERS]; uint8_t button_value[NUM_BUTTONS]; } shift;
    uint8_t name[16];
} __attribute__((packed));

/* codec API */
int profile_validate(const struct profile *p);                        /* 0 ok, -1 bad */
int profile_to_b64(const struct profile *p, char *out, int outcap);   /* returns encoded len, or -1 */
int profile_from_b64(const char *b64, int len, struct profile *out);  /* 0 ok, -1 bad */

#endif
