#ifndef PROTOCOL_H
#define PROTOCOL_H
#include "profile.h"
#define PROTO_VERSION 1
struct proto_store {
    int (*read)(uint8_t n, struct profile *out);      /* 0 ok, nonzero NVS_FAIL */
    int (*write)(uint8_t n, const struct profile *in); /* 0 ok, nonzero NVS_FAIL */
    int (*set_active)(uint8_t n);                      /* 0 ok, nonzero NVS_FAIL */
    int (*reset)(uint8_t n);                           /* 0 ok, nonzero NVS_FAIL: reseed slot n to default */
    int (*reset_all)(void);                            /* 0 ok, nonzero NVS_FAIL: reseed all slots to default */
    uint8_t (*get_active)(void);                       /* WITHIN-bank index (0..bank_profiles-1) of the current mode */
    uint8_t (*get_mode)(void);                         /* device mode: 0 MIDI, 1 KEYBOARD */
    int     (*set_mode)(uint8_t m);                    /* 0 ok; nonzero NVS_FAIL */
    uint8_t (*get_playrole)(void);                     /* PLAY role: 0 shift, 1 assignable */
    int     (*set_playrole)(uint8_t v);                /* 0 ok; nonzero NVS_FAIL */
    uint8_t (*get_midithru)(void);                     /* MIDI thru USB->TRS: 0 off, 1 on */
    int     (*set_midithru)(uint8_t v);                /* 0 ok; nonzero NVS_FAIL */
    uint8_t (*get_trsmode)(void);                      /* TRS jack role: 0 MIDI, 1 trigger, 2 sync */
    int     (*set_trsmode)(uint8_t v);                 /* 0 ok; nonzero NVS_FAIL */
    uint8_t (*get_trsdiv)(void);                       /* SYNC divider: clock ticks per pulse, 1..24 */
    uint8_t (*get_trshw)(void);                        /* pulse path: 1 hardware (TIMER+PPI), 0 k_timer fallback */
    uint8_t (*get_trslive)(void);                      /* LIVE module mode, vs the stored one — must agree */
    uint32_t (*get_trsfires)(void);                    /* pulses asserted since boot */
    uint8_t (*get_trsbusy)(void);                      /* 1 = a pulse is still high (stuck = no falling edge) */
    uint8_t (*get_trschan)(void);                      /* match channel 0..15, 16 = omni */
    int     (*set_trschan)(uint8_t v);
    uint8_t (*get_trswidth)(void);                     /* pulse width, 100 us units */
    int     (*set_trswidth)(uint8_t v);
    uint8_t (*get_trsring)(void);                      /* ring current source in pulse modes */
    int     (*set_trsring)(uint8_t v);
    uint8_t (*get_trsinv)(void);                       /* 0 = idle low/pulse high, 1 = inverted */
    int     (*set_trsinv)(uint8_t v);
    uint8_t (*get_trspin)(void);
    uint8_t (*get_trsfault)(void);                     /* 1 = the pad did not follow us on mode entry */                       /* the pin's ACTUAL level — must idle 0 in trigger mode */
    void    (*trspulse)(void);                         /* fire one pulse, bypassing note + clock */
    int     (*set_trsdiv)(uint8_t v);                  /* 0 ok; nonzero NVS_FAIL */
    /* Two distinct index axes (§0 mode-scoped banks). read/write/reset address a
     * GLOBAL slot 0..profiles-1 (both banks); setactive/get_active address a
     * WITHIN-bank index 0..bank_profiles-1 (the current mode's bank). Validating
     * setactive against `profiles` (=16) would let a host setactive n=8..15 pass
     * the gate, then get -EINVAL from a within-only set_active and be mislabeled
     * NVS_FAIL, so setactive is bounded by `bank_profiles` instead. */
    uint8_t profiles;          /* GLOBAL slot count (read/write/reset bound) = NUM_PROFILES */
    uint8_t bank_profiles;     /* WITHIN-bank profile count (setactive bound) = NUM_BANK_PROFILES */
    uint8_t faders, buttons;
    const char *fw;
    const char *uid;   /* hex hardware id (hwinfo) for getPorts matching; "" if none */
#ifdef CONFIG_FELDD_BT_PROVISION
    /* Q5 P0 stage-1 READ-ONLY SS probe hook. Wired by config_cdc.c to
     * bt_provision_probe_ss() in the FELDD_BT_PROVISION build only; NULL/absent otherwise.
     * Keeps protocol.c pure (no hardware include) — the verb just calls this pointer.
     * Fills ss_out[64] + *gate_out (enum bt_gate_result); returns enum bt_provision_status
     * (0 == OK). #ifdef'd so the shipped/host protocol layer is byte-unchanged. */
    int (*bt_ss_probe)(uint8_t ss_out[64], int *gate_out);
    /* Q5 P0 stage-2 DS-WRITE provisioning hook. Wired by config_cdc.c to bt_provision_run()
     * in the FELDD_BT_PROVISION build only; NULL/absent otherwise. Runs the power gate +
     * the read+gate+DS-write+verify+SS-preserve+launch sequence; fills *gate_out (enum
     * bt_gate_result) and returns an enum bt_provision_flash_status (0 == OK, includes
     * POWER_DEFER, GATE_REFUSED, the DS-write codes, SS_CHANGED, LAUNCH_FAIL). #ifdef'd so
     * the shipped/host protocol layer is byte-unchanged. */
    int (*bt_provision)(int *gate_out);
#endif
};
/* Side-channel result the caller may inspect after a successful dispatch.
   Currently carries the monitor on/off state parsed from a "monset" request,
   so the CDC binding sets its live-monitor flag from a real parse instead of
   string-matching the response. Pass NULL if you don't care. */
struct proto_result {
    uint8_t mon_set;   /* 1 iff this line was a successful monset */
    uint8_t mon_on;    /* when mon_set: 1 = monitoring on, 0 = off */
};

/* Handle one request line; write one response line (NUL-terminated, no newline)
   into out[0..outcap-1]. Returns response length, or -1 on outcap overflow.
   If res is non-NULL it is zeroed, then populated for verbs that expose a
   side-channel result (currently only monset). */
int proto_handle(const struct proto_store *s, const char *line,
                 char *out, int outcap, struct proto_result *res);
#endif
