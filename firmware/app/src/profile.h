#ifndef PROFILE_H
#define PROFILE_H
#include <stdint.h>
#define PROFILE_VERSION 4
#define NUM_FADERS 4
#define NUM_BUTTONS 9
#define NUM_MODES 2                 /* MIDI bank + Keyboard bank (mode>profile>layer §0) */
#define NUM_BANK_PROFILES 8         /* profiles per mode (the •• cycle wraps these) */
#define NUM_PROFILES (NUM_MODES * NUM_BANK_PROFILES)   /* 16 total NVS slots */
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
    uint8_t channel;                       /* 0..15 — profile default ("set all") */
    struct fader_map  fader[NUM_FADERS];
    struct button_map button[NUM_BUTTONS];
    struct { uint8_t fader_cc[NUM_FADERS]; uint8_t button_value[NUM_BUTTONS]; } shift;
    uint8_t name[16];
    /* v2: per-control MIDI channel (0..15). The mapping engine sends each fader
     * and button on ITS own channel, so one profile can drive multiple tracks at
     * once — e.g. 4 faders -> 4 track volumes on channels 1..4 (a real mixer).
     * `channel` above stays as the UI default ("set all") + the v1 fallback. The
     * firmware only ever speaks v2; the web import layer fills these from
     * `channel` when reading a legacy v1 profile. */
    uint8_t fader_channel[NUM_FADERS];     /* 0..15 each */
    uint8_t button_channel[NUM_BUTTONS];   /* 0..15 each */
    /* v3: per-button USB-HID keyboard binding (Keyboard mode). button_key[i] is
     * a HID usage id (0=unbound, 0x04='a', 0x28=Enter, 0x2C=Space); button_mod[i]
     * is a HID modifier bitmask (bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGUI).
     * Appended after the v2 layout so the byte image is a clean superset, exactly
     * like v2 appended channels after v1. Faders type nothing — buttons only. */
    uint8_t button_key[NUM_BUTTONS];       /* HID usage id, 0 = unbound (base) */
    uint8_t button_mod[NUM_BUTTONS];       /* HID modifier bitmask     (base) */
    /* v4: SHIFT-LAYER keyboard binding (Keyboard mode, double-tap-PLAY bank).
     * Same semantics as button_key/button_mod but selected when the PLAY shift
     * latch is engaged — so Keyboard mode gets a second keymap exactly like the
     * MIDI shift bank. Appended after the v3 tail so the byte image stays a clean
     * superset, exactly like v2/v3 appended. A v3 blob reaching v4 firmware is
     * rejected by the version check; a v3 blob decoded by the v4 web codec reads
     * these as 0 (unbound). */
    uint8_t button_key_shift[NUM_BUTTONS]; /* HID usage id, 0 = unbound */
    uint8_t button_mod_shift[NUM_BUTTONS]; /* HID modifier bitmask      */
} __attribute__((packed));

/* codec API */
int profile_validate(const struct profile *p);                        /* 0 ok, -1 bad */
int profile_to_b64(const struct profile *p, char *out, int outcap);   /* returns encoded len, or -1 */
int profile_from_b64(const char *b64, int len, struct profile *out);  /* 0 ok, -1 bad */

#endif
