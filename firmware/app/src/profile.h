#ifndef PROFILE_H
#define PROFILE_H
#include <stdint.h>
#define PROFILE_VERSION 9
#define NUM_FADERS 4
#define NUM_BUTTONS 9
#define NUM_LAYERS 8                /* 8 selectable layers = 8 tracks (PLAY count-dial, taps 1..8) */
#define NUM_MODES 2                 /* MIDI bank + Keyboard bank (mode>profile>layer §0) */
#define NUM_BANK_PROFILES 8         /* profiles per mode (the •• cycle wraps these) */
#define NUM_PROFILES (NUM_MODES * NUM_BANK_PROFILES)   /* 16 total NVS slots */
enum fader_curve { CURVE_LINEAR=0, CURVE_LOG=1, CURVE_EXP=2 };
enum fader_role { FADER_ROLE_CC=0, FADER_ROLE_CHORD_DEPTH=1 };
/* Button behaviors. NOTE and CC_MOMENTARY are realized by the pure mapping
 * engine (it emits the MIDI directly). CC_TOGGLE (latching on/off), TRANSPORT,
 * and PROFILE_SWITCH are STATEFUL and realized by the firmware control loop,
 * which tracks their state and emits MIDI itself; the mapping engine emits
 * nothing for them. */
enum btn_type { BTN_NONE=0, BTN_NOTE=1, BTN_CC_TOGGLE=2, BTN_CC_MOMENTARY=3,
                BTN_TRANSPORT=4, BTN_PROFILE_SWITCH=5,
                BTN_CHORD=6, BTN_CC_VALUE=7 };
struct fader_map { uint8_t cc, min, max, curve, invert; };
struct button_map { uint8_t type, value; };
/* v5: per-layer parameter bank for the TWO appended layers (L3, L4). L1 lives
 * inline (fader/button + base button_key/button_mod); L2 lives in shift.* +
 * button_key_shift/button_mod_shift. The variable fields per layer are the fader
 * CCs + button values (MIDI) and the per-button key + mod (Keyboard). In v5
 * button TYPES, fader min/max/curve/invert, and per-control channels were shared
 * from L1 (parameter-pages model). 31 bytes/bank * 2 banks = 62, 118 + 62 = 180.
 * v6 keeps this struct BYTE-IDENTICAL so the 180-byte image stays the prefix; the
 * fields that make each layer fully independent are appended in struct layer_ext. */
struct layer_bank {
    uint8_t fader_cc[NUM_FADERS];      /* MIDI: this layer's fader CCs        */
    uint8_t button_value[NUM_BUTTONS]; /* MIDI: this layer's button values    */
    uint8_t button_key[NUM_BUTTONS];   /* Keyboard: HID usage id, 0 = unbound */
    uint8_t button_mod[NUM_BUTTONS];   /* Keyboard: HID modifier bitmask      */
};
/* v6: the per-layer fields that, in v5, were SHARED from L1. Appending one of
 * these for each of L2/L3/L4 makes every layer a COMPLETE independent control
 * set (full fader_map + full button type + per-control channels), so all 4 layer
 * tabs become identical full editors. 38 bytes/bank:
 *   fader_min[4] + fader_max[4] + fader_curve[4] + fader_invert[4]   = 16
 *   button_type[9]                                                   =  9
 *   fader_channel[4]                                                 =  4
 *   button_channel[9]                                                =  9
 * L1 already carries all of these inline (fader_map[4], button[].type,
 * fader_channel[4], button_channel[9]); L2/L3/L4 carry only the partial set in
 * v5 (cc/value/key/mod). So this tail supplies exactly the missing fields, in the
 * same per-control SoA order the rest of the struct uses. 38 * 3 = 114 appended;
 * 180 + 114 = 294. A v6 layer's full config = {fader_cc, button_value, button_key,
 * button_mod} from the v5 bank/inline fields + {min,max,curve,invert,type,
 * fader_channel,button_channel} from this layer_ext. */
struct layer_ext {
    uint8_t fader_min[NUM_FADERS];     /* this layer's fader minimums        */
    uint8_t fader_max[NUM_FADERS];     /* this layer's fader maximums        */
    uint8_t fader_curve[NUM_FADERS];   /* CURVE_LINEAR/LOG/EXP per fader     */
    uint8_t fader_invert[NUM_FADERS];  /* 0/1 per fader                      */
    uint8_t button_type[NUM_BUTTONS];  /* enum btn_type per button           */
    uint8_t fader_channel[NUM_FADERS]; /* 0..15 per fader                    */
    uint8_t button_channel[NUM_BUTTONS];/* 0..15 per button                  */
};
/* v8: the UNPACKED in-RAM chord definition (12 B). NOT stored in the profile any
 * more — the profile stores the packed chord6 (6 B) per button; chord6_unpack()
 * produces one of these on the press edge for chord_engine. 5-note explicit cap on
 * STORED notes (chord6); this in-RAM form keeps notes[6] so the resolve layer and
 * MAX_CHORD=8 ranges/depth are unchanged. */
struct chord_def {
    uint8_t mode;          /* 0=explicit, 1=range, 2=root+quality                   */
    uint8_t count;         /* explicit notes live, 0..6                             */
    uint8_t notes[6];      /* explicit MIDI note numbers                            */
    uint8_t root;          /* root for root+quality                                 */
    uint8_t quality;       /* quality enum, 0=none/explicit-only (depth-ineligible) */
    uint8_t range_start;   /* range first note                                      */
    uint8_t range_count;   /* 0 = not a range; >0 fires start..start+count-1        */
};                         /* = 12 bytes                                            */
/* v8: the PACKED 6-byte stored chord (spec §2). hdr byte packs mode (bits 7..5) +
 * explicit count (bits 4..0, 0..5); 5 payload bytes are mode-specific. This is what
 * lives in the profile (NVS + wire); chord6_unpack() expands it to a struct chord_def. */
struct chord6 { uint8_t b[6]; };  /* = 6 bytes */
#define CHORD6_MODE_EXPLICIT 0
#define CHORD6_MODE_RANGE    1
#define CHORD6_MODE_QUALITY  2
#define CHORD6_MAX_EXPLICIT  5    /* stored hand-picked note cap (NVS-driven) */
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
    /* v5: the two ADDITIONAL layers (L3, L4) beyond the inline-L1 / shift-L2 banks.
     * layer[0] = profile layer index 2 (the 3rd page), layer[1] = index 3. The
     * mapping engine selects the active page by gesture_layer() (0..3): 0->inline,
     * 1->shift.*, 2->layer[0], 3->layer[1]. */
    struct layer_bank layer[NUM_LAYERS - 2];   /* L3, L4 */
    /* --- end of the v5 image (offset 180). Everything below is the v6 tail. --- */
    /* v6: per-layer completion banks for L2, L3, L4 so EVERY layer is a complete
     * independent control set (all 4 layer tabs become identical full editors).
     * L1 already stores these fields inline (fader_map[4], button[].type,
     * fader_channel[4], button_channel[9]); v5 shared them from L1 for L2/L3/L4.
     * Order: ext[0] = L2 (the shift bank), ext[1] = L3 (layer[0]), ext[2] = L4
     * (layer[1]). Appended after the v5 tail so the byte image is a clean
     * prefix-superset, exactly like v2/v3/v4/v5 appended. 38 B/bank * 3 = 114;
     * 180 + 114 = 294. A v5 blob reaching v6 firmware is rejected by the version
     * check; a v5 blob decoded by the v6 codec fills these from L1 (so the upgrade
     * keeps v5's "inherit L1" behavior). */
    struct layer_ext ext[NUM_LAYERS - 1];      /* L2, L3, L4 */
    /* --- end of the v6 image (offset 294). Everything below is the v8 tail. --- */
    /* v8: one INLINE packed chord per (layer, button). chord6 is DIRECT-indexed by
     * layer 0..3 (NOT ext-style layer-1) - L1 has its own chord home. A button is a
     * chord when its button_type (in the v6 part) == BTN_CHORD; the chord6 slot for a
     * non-chord button is inert/zero. Replaces v7's button_chord_ix + shared
     * chord_table (the "8 distinct chords / profile" limit). 216 B.
     * fader_role[L][f]: 0=cc, 1=chord_depth (carried from v7). 16 B.
     * chord_flags[0]=chord velocity (default 100); [1] reserved. 2 B.
     * 216 + 16 + 2 = 234-byte tail; 294 + 234 = 528. A v7 blob reaching v8 firmware is
     * rejected by the version check; a v6/older blob decoded by v8 fills these at
     * "no chords" (every chord6 zero, every fader_role cc, velocity 100). */
    /* Feature 1 (v9, NO format change): a BTN_CC_VALUE button REUSES this same
     * per-(layer,button) chord6 slot to store its {sub_mode,on_value,off_value}:
     *   b[0]=0 (reserved), b[1]=on_value, b[2]=off_value,
     *   b[3]=sub_mode (0=set-on-press, 1=momentary, 2=toggle),
     *   b[4]=b[5]=0 (reserved for a future field, kept forkless).
     * INVARIANT: chord6[layer][idx] is read as a CHORD only when the button's
     * type == BTN_CHORD; a BTN_CC_VALUE slot is NEVER fed to the chord engine
     * (route_midi_button branches on the type before touching chord6). The b[0]=0
     * + b[4]=b[5]=0 pinning also makes the slot decode as an EMPTY chord to any
     * reader that does not understand BTN_CC_VALUE. */
    struct chord6 chord6[NUM_LAYERS][NUM_BUTTONS];     /* 4*9*6 = 216 B; packed per button */
    uint8_t  fader_role[NUM_LAYERS][NUM_FADERS];       /* 4*4   = 16 B; 0=cc,1=chord_depth */
    uint8_t  chord_flags[4];                           /* 4 B: [0]=chord velocity, [1..3]=reserved. [2..3] pad struct to 1038 (multiple of 3) so base64 has no '=' padding (spec 2.1). */
} __attribute__((packed));

/* v9 byte layout (packed, all u8). v9 is the FIRST version to RESIZE the interior
 * arrays (NUM_LAYERS 4->8) rather than append a tail, so it is a HARD FORMAT BREAK,
 * not a prefix-superset of v8. The shared v8/v9 prefix ends at byte 179
 * (layer[0]/layer[1] sit at 118..179 in BOTH); divergence begins at byte 180
 * (v8 has ext[0] there, v9 has layer[2]).
 *   [0..117]     fixed prefix, byte-identical to v8
 *   [118..303]   layer[6]        (L3..L8 banks; was layer[2] @118..179)
 *   [304..569]   ext[7]          (L2..L8 completion; was ext[3] @180..293)
 *   [570..1001]  chord6[8][9]    (packed per (layer,button); was @294..509)
 *   [1002..1033] fader_role[8][4]
 *   [1034..1037] chord_flags[4]  ([0]=velocity, [1..3]=reserved pad)
 *   sizeof = 1038; base64 = ceil(1038/3)*4 = 1384 chars, no '=' padding.
 */

/* codec API */
int profile_validate(const struct profile *p);                        /* 0 ok, -1 bad */
int profile_to_b64(const struct profile *p, char *out, int outcap);   /* returns encoded len, or -1 */
int profile_from_b64(const char *b64, int len, struct profile *out);  /* 0 ok, -1 bad */
/* Wire byte count for a given profile version (the clean prefix length). v1..v6
 * are the historical append boundaries; an unknown/future version maps to the
 * full current image so a forward blob still round-trips length-wise (validate
 * rejects the version separately). */
int profile_wire_len(uint8_t version);

#endif
