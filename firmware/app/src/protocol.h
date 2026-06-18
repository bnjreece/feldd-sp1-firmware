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
    uint8_t (*get_active)(void);
    uint8_t profiles, faders, buttons;
    const char *fw;
    const char *uid;   /* hex hardware id (hwinfo) for getPorts matching; "" if none */
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
