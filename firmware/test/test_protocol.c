/*
 * test_protocol.c — host TDD for the JSON-lines config protocol (proto_handle).
 *
 * Backs proto_handle with an in-RAM mock store (struct profile arr[8] + active
 * index + flag/fail counters). Pure host build; no Zephyr, no hardware.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "protocol.h"

/* ---------- mock store ---------- */
/* §0 mode-scoped banks: 16 GLOBAL slots (read/write/reset axis), 8 per bank.
 * get/set_active operate on the WITHIN-bank index (0..7) of the current mode;
 * read/write/reset address a GLOBAL slot (0..15). */
#define MOCK_PROFILES 16
#define MOCK_BANK 8
static struct profile g_arr[MOCK_PROFILES];
static uint8_t        g_mode;                 /* current device mode 0/1 */
static uint8_t        g_active_within[2];     /* per-mode within-bank active */
static int            g_fail_write;   /* if set, store->write returns this */
static int            g_fail_read;    /* if set, store->read  returns this */
static int            g_fail_reset;   /* if set, store->reset / reset_all return this */
static int            g_reset_n;      /* last n passed to store->reset (-1 = none) */
static int            g_reset_calls;  /* number of store->reset invocations */
static int            g_resetall_calls; /* number of store->reset_all invocations */

static int mock_read(uint8_t n, struct profile *out)
{
    if (g_fail_read) return g_fail_read;
    if (n >= MOCK_PROFILES) return 1;
    memcpy(out, &g_arr[n], sizeof *out);
    return 0;
}
static int mock_write(uint8_t n, const struct profile *in)
{
    if (g_fail_write) return g_fail_write;
    if (n >= MOCK_PROFILES) return 1;
    memcpy(&g_arr[n], in, sizeof *in);
    return 0;
}
static int mock_set_active(uint8_t within)   /* within 0..7 of the current mode */
{
    if (within >= MOCK_BANK) return 1;
    g_active_within[g_mode] = within;
    return 0;
}
static uint8_t mock_get_active(void) { return g_active_within[g_mode]; }   /* WITHIN */
static int mock_reset(uint8_t n)
{
    if (g_fail_reset) return g_fail_reset;
    if (n >= MOCK_PROFILES) return 1;
    g_reset_n = (int)n;
    g_reset_calls++;
    memset(&g_arr[n], 0, sizeof g_arr[n]);   /* simulate reseed to a known state */
    g_arr[n].version = PROFILE_VERSION;
    return 0;
}
static int mock_reset_all(void)
{
    if (g_fail_reset) return g_fail_reset;
    g_resetall_calls++;
    for (int i = 0; i < MOCK_PROFILES; i++) {
        memset(&g_arr[i], 0, sizeof g_arr[i]);
        g_arr[i].version = PROFILE_VERSION;
    }
    return 0;
}
static uint8_t mock_get_mode(void) { return g_mode; }
static int mock_set_mode(uint8_t m)
{
    if (m > 1) return 1;
    g_mode = m;
    return 0;
}

static struct proto_store make_store(void)
{
    struct proto_store s;
    s.read = mock_read;
    s.write = mock_write;
    s.set_active = mock_set_active;
    s.get_active = mock_get_active;
    s.reset = mock_reset;
    s.reset_all = mock_reset_all;
    s.get_mode = mock_get_mode;
    s.set_mode = mock_set_mode;
    s.profiles = MOCK_PROFILES;        /* 16 global slots (read/write/reset axis) */
    s.bank_profiles = MOCK_BANK;       /* 8 within-bank slots (setactive axis) */
    s.faders = NUM_FADERS;
    s.buttons = NUM_BUTTONS;
    s.fw = "1.2.3";
    s.uid = "0011223344556677";
    return s;
}

static void reset_store(void)
{
    memset(g_arr, 0, sizeof g_arr);
    g_mode = 0;
    g_active_within[0] = 0;
    g_active_within[1] = 0;
    g_fail_write = 0;
    g_fail_read = 0;
    g_fail_reset = 0;
    g_reset_n = -1;
    g_reset_calls = 0;
    g_resetall_calls = 0;
}

/* Build a fully-populated, VALID profile with distinct values everywhere. */
static struct profile make_full_profile(void)
{
    struct profile p;
    memset(&p, 0, sizeof p);
    p.version = PROFILE_VERSION;
    p.channel = 7;
    for (int i = 0; i < NUM_FADERS; i++) {
        p.fader[i].cc     = (uint8_t)(10 + i);
        p.fader[i].min    = (uint8_t)(i * 5);
        p.fader[i].max    = (uint8_t)(100 + i);
        p.fader[i].curve  = (uint8_t)(i % 3);
        p.fader[i].invert = (uint8_t)(i % 2);
        p.shift.fader_cc[i] = (uint8_t)(30 + i);
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p.button[i].type  = (uint8_t)(i % 6);
        p.button[i].value = (uint8_t)(40 + i);
        p.shift.button_value[i] = (uint8_t)(50 + i);
    }
    for (int i = 0; i < 16; i++)
        p.name[i] = (uint8_t)('A' + i);
    return p;
}

/* Tiny helper: extract the string value of "data":"..." from a response line.
 * Returns length copied into dst (NUL-terminated), or -1 if absent. */
static int extract_str_field(const char *line, const char *key, char *dst, int cap)
{
    /* find "key": */
    char pat[32];
    int kn = snprintf(pat, sizeof pat, "\"%s\":\"", key);
    if (kn < 0 || kn >= (int)sizeof pat) return -1;
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p += kn;                       /* now at first char of value */
    int i = 0;
    while (*p && *p != '"') {
        if (i >= cap - 1) return -1;
        dst[i++] = *p++;
    }
    if (*p != '"') return -1;
    dst[i] = '\0';
    return i;
}

/* ---------- tests ---------- */

/* 1. hello */
static void t_hello(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"hello\",\"i\":1}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"hello_r\""));
    assert(strstr(out, "\"i\":1"));
    assert(strstr(out, "\"ok\":true"));
    assert(strstr(out, "\"proto\":1"));
    assert(strstr(out, "\"profiles\":16"));   /* §0 mode-scoped banks: 2 x 8 */
    assert(strstr(out, "\"faders\":4"));
    assert(strstr(out, "\"buttons\":9"));
    assert(strstr(out, "\"active\":0"));
    /* pbytes must equal sizeof(struct profile) - derived, not magic. */
    char pb[32];
    snprintf(pb, sizeof pb, "\"pbytes\":%d", (int)sizeof(struct profile));
    assert(strstr(out, pb));
    /* pver must equal PROFILE_VERSION - derived, not magic. */
    char pv[32];
    snprintf(pv, sizeof pv, "\"pver\":%d", PROFILE_VERSION);
    assert(strstr(out, pv));
    assert(strstr(out, "\"caps\":"));
    assert(strstr(out, "\"trs\""));
    assert(strstr(out, "\"1.2.3\""));   /* fw echoed */
    assert(strstr(out, "\"uid\":\"0011223344556677\""));  /* hwinfo id echoed */
}

/* 2. read */
static void t_read(void)
{
    reset_store();
    g_arr[0] = make_full_profile();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"read\",\"i\":2,\"n\":0}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"read_r\""));
    assert(strstr(out, "\"n\":0"));
    assert(strstr(out, "\"i\":2"));
    char b64[256];
    int n = extract_str_field(out, "data", b64, (int)sizeof b64);
    assert(n > 0);
    struct profile got;
    int dr = profile_from_b64(b64, n, &got);
    assert(dr == 0);
    assert(memcmp(&got, &g_arr[0], sizeof got) == 0);
}

/* 3. write */
static void t_write(void)
{
    reset_store();
    struct proto_store s = make_store();
    struct profile p = make_full_profile();
    char b64[256];
    int bn = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(bn > 0);
    char line[512];
    snprintf(line, sizeof line, "{\"t\":\"write\",\"i\":3,\"n\":2,\"data\":\"%s\"}", b64);
    char out[512];
    int rc = proto_handle(&s, line, out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"write_r\""));
    assert(strstr(out, "\"n\":2"));
    assert(strstr(out, "\"i\":3"));
    assert(memcmp(&g_arr[2], &p, sizeof p) == 0);
}

/* 4. write bad data length */
static void t_write_bad_len(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"write\",\"i\":3,\"n\":2,\"data\":\"AAAA\"}",
                          out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_LEN\""));
    assert(strstr(out, "\"i\":3"));
    assert(strstr(out, "\"ok\":false"));
}

/* 5. write bad version: valid-length b64 whose decoded version != PROFILE_VERSION */
static void t_write_bad_version(void)
{
    reset_store();
    struct proto_store s = make_store();
    struct profile p = make_full_profile();
    p.version = PROFILE_VERSION + 1;   /* still encodes to the right length */
    char b64[256];
    int bn = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(bn > 0);
    char line[512];
    snprintf(line, sizeof line, "{\"t\":\"write\",\"i\":8,\"n\":1,\"data\":\"%s\"}", b64);
    char out[512];
    int rc = proto_handle(&s, line, out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_VERSION\""));
    assert(strstr(out, "\"i\":8"));
    /* store must NOT have been written */
    struct profile zero; memset(&zero, 0, sizeof zero);
    assert(memcmp(&g_arr[1], &zero, sizeof zero) == 0);
}

/* 6. setactive then getactive */
static void t_setactive_getactive(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"setactive\",\"i\":4,\"n\":3}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"setactive_r\""));
    assert(strstr(out, "\"active\":3"));
    assert(strstr(out, "\"i\":4"));
    assert(g_active_within[0] == 3);   /* mode 0 (MIDI) within-bank active */

    rc = proto_handle(&s, "{\"t\":\"getactive\",\"i\":5}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"getactive_r\""));
    assert(strstr(out, "\"active\":3"));
    assert(strstr(out, "\"i\":5"));
}

/* 6b. setactive is bounded by the WITHIN-bank axis (bank_profiles), NOT the
 * global slot count (profiles). §0 mode-scoped banks: read/write/reset address a
 * GLOBAL slot 0..profiles-1 (both banks), but setactive selects a WITHIN-bank
 * index 0..bank_profiles-1 of the CURRENT mode. A two-bank store (profiles=16,
 * bank_profiles=8) must:
 *   - REJECT setactive n=8..15 as BAD_INDEX (a within index can't exceed 7),
 *     NOT pass the gate and surface set_active's -EINVAL as a misleading NVS_FAIL,
 *   - still ACCEPT read/write of the upper bank (global n=8..15), proving the two
 *     axes are reconciled rather than collapsed onto one bound. */
static void t_setactive_within_vs_global_bounds(void)
{
    reset_store();
    struct proto_store s = make_store();
    s.profiles = 16;          /* two banks of 8 (global slots 0..15) */
    s.bank_profiles = 8;      /* the current mode's bank (within 0..7) */
    char out[512];

    /* within edge: n=7 is the last valid within index -> OK. */
    int rc = proto_handle(&s, "{\"t\":\"setactive\",\"i\":1,\"n\":7}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"setactive_r\""));
    assert(strstr(out, "\"active\":7"));

    /* n=8 is bank 1's BASE, a valid GLOBAL slot but NOT a valid within index.
     * It must be a clean BAD_INDEX, never a BAD_INDEX-bypass -> NVS_FAIL. */
    rc = proto_handle(&s, "{\"t\":\"setactive\",\"i\":2,\"n\":8}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_INDEX\""));
    assert(!strstr(out, "NVS_FAIL"));   /* the whole point: not mislabeled */

    /* n=15 (top of bank 1) is likewise rejected on the within axis. */
    rc = proto_handle(&s, "{\"t\":\"setactive\",\"i\":3,\"n\":15}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"code\":\"BAD_INDEX\""));

    /* But the upper bank IS addressable on the GLOBAL axis: read/write n=15 work.
     * (mock_read/write bound on MOCK_PROFILES=8, so use the real range here by
     * checking that the protocol gate itself admits n=15 rather than BAD_INDEX.)
     * Reading global 8..15 against this 16-wide protocol bound must pass the gate
     * and reach the store, where the single-bank mock returns NVS_FAIL, proving
     * the protocol no longer rejects the upper bank at the index gate. */
    rc = proto_handle(&s, "{\"t\":\"read\",\"i\":4,\"n\":15}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(!strstr(out, "BAD_INDEX"));  /* global gate admits 15 */
}

/* 7. read bad index */
static void t_read_bad_index(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"read\",\"i\":9,\"n\":99}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_INDEX\""));
    assert(strstr(out, "\"i\":9"));
}

/* 8. monset */
static void t_monset(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"monset\",\"i\":6,\"on\":true}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"monset_r\""));
    assert(strstr(out, "\"on\":true"));
    assert(strstr(out, "\"i\":6"));

    /* false echoes false */
    rc = proto_handle(&s, "{\"t\":\"monset\",\"i\":7,\"on\":false}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"monset_r\""));
    assert(strstr(out, "\"on\":false"));
}

/* 8b. monset reports the parsed on/off flag via the result out-param,
 * so config_cdc no longer has to strstr-sniff the response line. */
static void t_monset_result_flag(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    struct proto_result res;

    /* on:true -> res.mon_set true, res.mon_on true */
    memset(&res, 0xEE, sizeof res);
    int rc = proto_handle(&s, "{\"t\":\"monset\",\"i\":6,\"on\":true}",
                          out, (int)sizeof out, &res);
    assert(rc > 0);
    assert(res.mon_set == 1);
    assert(res.mon_on == 1);

    /* on:false -> res.mon_set true, res.mon_on false */
    memset(&res, 0xEE, sizeof res);
    rc = proto_handle(&s, "{\"t\":\"monset\",\"i\":7,\"on\":false}",
                      out, (int)sizeof out, &res);
    assert(rc > 0);
    assert(res.mon_set == 1);
    assert(res.mon_on == 0);

    /* a NON-monset verb must leave mon_set clear so the caller ignores it */
    memset(&res, 0xEE, sizeof res);
    rc = proto_handle(&s, "{\"t\":\"getactive\",\"i\":8}",
                      out, (int)sizeof out, &res);
    assert(rc > 0);
    assert(res.mon_set == 0);

    /* a malformed monset (missing on) is an err and must NOT set mon_set */
    memset(&res, 0xEE, sizeof res);
    rc = proto_handle(&s, "{\"t\":\"monset\",\"i\":9}",
                      out, (int)sizeof out, &res);
    assert(rc > 0);
    assert(strstr(out, "\"code\":\"BAD_JSON\""));
    assert(res.mon_set == 0);
}

/* 8c. list returns ALL 16 entries (both banks), each tagged with its bank, +
 * a top-level mode + a within-mode active. The web shows both banks together;
 * bank 0 = MIDI (global 0..7), bank 1 = Keyboard (global 8..15). Names are
 * decoded straight from profile.name[16] (NOT base64). The whole 16-entry
 * response must fit the firmware's g_resp buffer (now 1280; see
 * t_list_worst_case_fits_g_resp for the all-escaped worst case). */
static void t_list_all_banks(void)
{
    reset_store();
    static const char *nm0 = "MIDI-A";   /* global 0 -> bank 0 */
    static const char *nm8 = "KB-A";     /* global 8 -> bank 1 */
    struct profile p0 = make_full_profile();
    memset(p0.name, 0, sizeof p0.name); memcpy(p0.name, nm0, strlen(nm0));
    g_arr[0] = p0;
    struct profile p8 = make_full_profile();
    memset(p8.name, 0, sizeof p8.name); memcpy(p8.name, nm8, strlen(nm8));
    g_arr[8] = p8;
    g_mode = 1;                /* current = Keyboard bank */
    g_active_within[1] = 2;    /* KB active = within 2 (global 10) */

    struct proto_store s = make_store();
    char out[768];
    int rc = proto_handle(&s, "{\"t\":\"list\",\"i\":5}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"list_r\""));
    assert(strstr(out, "\"i\":5"));
    assert(strstr(out, "\"mode\":1"));           /* current mode reported */
    assert(strstr(out, "\"active\":2"));          /* WITHIN index of the current mode */
    assert(strstr(out, "\"name\":\"MIDI-A\""));
    assert(strstr(out, "\"name\":\"KB-A\""));
    assert(strstr(out, "\"n\":0,\"bank\":0"));    /* global 0 is bank 0 */
    assert(strstr(out, "\"n\":8,\"bank\":1"));    /* global 8 is bank 1 */
    /* version field per slot, derived from profile.version */
    char vpat[24];
    snprintf(vpat, sizeof vpat, "\"ver\":%d", PROFILE_VERSION);
    assert(strstr(out, vpat));
    /* a zeroed slot decodes to an empty name, not garbage past the NUL */
    assert(strstr(out, "\"name\":\"\""));
    /* the full 16-entry list must fit (proto_handle returns -1 if it would not). */
    assert(rc < (int)sizeof out);
}

/* 8c'. read/write reach into bank 1 (global idx 8..15) regardless of current
 * mode — the editor can write either bank without first flipping mode. */
static void t_read_write_other_bank(void)
{
    reset_store();
    struct proto_store s = make_store();
    struct profile p = make_full_profile();
    char b64[256];
    int bn = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(bn > 0);
    char line[512];
    snprintf(line, sizeof line, "{\"t\":\"write\",\"i\":3,\"n\":15,\"data\":\"%s\"}", b64);
    char out[512];
    int rc = proto_handle(&s, line, out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"write_r\""));
    assert(strstr(out, "\"n\":15"));
    assert(memcmp(&g_arr[15], &p, sizeof p) == 0);   /* wrote the KB bank's last slot */
}

/* 8d. list with a HOSTILE profile name: raw name[16] bytes that include a
 * newline (0x0A), a carriage return (0x0D), a double-quote (0x22), a backslash
 * (0x5C) and another control byte. The emit side must sanitize: NO raw control
 * byte (< 0x20) may appear in the output line, and '"'/'\\' must be escaped so
 * the result is a single valid JSON line (the newline-delimited line protocol
 * is otherwise corrupted by an embedded 0x0A). */
static void t_list_hostile_name(void)
{
    reset_store();
    struct profile p = make_full_profile();
    /* name bytes: 'a', LF, '"', '\\', CR, 0x07 (BEL), 'b' then NUL-padded */
    memset(p.name, 0, sizeof p.name);
    p.name[0] = 'a';
    p.name[1] = 0x0A;   /* newline - would break the line protocol */
    p.name[2] = 0x22;   /* '"'    - would break the JSON string */
    p.name[3] = 0x5C;   /* '\\'   - would break the JSON string */
    p.name[4] = 0x0D;   /* CR */
    p.name[5] = 0x07;   /* BEL control */
    p.name[6] = 'b';
    g_arr[0] = p;
    mock_set_active(0);

    struct proto_store s = make_store();
    char out[768];   /* 16-entry banked list_r */
    int rc = proto_handle(&s, "{\"t\":\"list\",\"i\":5}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"list_r\""));

    /* NO raw control byte (< 0x20) anywhere in the emitted line */
    for (int i = 0; i < rc; i++)
        assert((unsigned char)out[i] >= 0x20);

    /* the quote and backslash must appear ESCAPED, not raw */
    assert(strstr(out, "\\\""));   /* an escaped double-quote */
    assert(strstr(out, "\\\\"));   /* an escaped backslash */

    /* the surviving printable chars 'a' and 'b' must still be present in the
     * sanitized name */
    assert(strstr(out, "\"name\":\"a"));
    assert(strstr(out, "b\""));
}

/* 8e. WORST-CASE list_r MUST fit the firmware's g_resp buffer. Regression guard
 * for the under-budgeting bug: the sanitizer prepends a backslash for EVERY '"'
 * and '\\', so a name[16] of all '"' emits 32 wire bytes, not 16. profile_validate
 * does NOT constrain name[] content, so a host can legally write all 16 slots
 * with 16-char all-'"' names via `write`. The OLD 768-byte budget (which counted
 * name[16] as 16 wire bytes) OVERFLOWED on this legal input: proto_handle returned
 * -1 and the host could not enumerate profiles at all. This pins the response to
 * fit the real firmware g_resp size. Keep this constant in lockstep with
 * config_cdc.c::g_resp. */
#define G_RESP_SIZE 1280   /* mirror of config_cdc.c g_resp[1280] */
static void t_list_worst_case_fits_g_resp(void)
{
    reset_store();
    /* Fill ALL 16 slots with the worst case the wire can carry: a full 16-char
     * name of all '"' (each escapes to 2 -> 32 wire bytes) + a 3-digit version. */
    for (int i = 0; i < MOCK_PROFILES; i++) {
        struct profile p = make_full_profile();
        p.version = 255;                       /* 3-digit ver -> widest "ver" */
        memset(p.name, '"', sizeof p.name);    /* 16 quotes, NO NUL -> full 16 */
        g_arr[i] = p;
    }

    struct proto_store s = make_store();
    /* Size EXACTLY like the firmware's g_resp so this asserts the real budget. */
    char resp[G_RESP_SIZE];
    int rc = proto_handle(&s, "{\"t\":\"list\",\"i\":4294967295}",
                          resp, (int)sizeof resp, NULL);
    /* Must NOT overflow: a positive length, fully within the buffer. */
    assert(rc > 0);
    assert(rc < (int)sizeof resp);
    assert(strstr(resp, "\"t\":\"list_r\""));
    /* All 16 banked entries are present (last global slot 15 in bank 1). */
    assert(strstr(resp, "\"n\":15,\"bank\":1"));
    /* Every '"' in a name is escaped (a `\"` appears in the name field). */
    assert(strstr(resp, "\\\""));

    /* Prove the OLD budget was genuinely too small: the SAME worst case into a
     * 768-byte buffer overflows (proto_handle returns -1), so the bump is load-
     * bearing, not cosmetic. */
    char resp_old[768];
    int rc_old = proto_handle(&s, "{\"t\":\"list\",\"i\":4294967295}",
                              resp_old, (int)sizeof resp_old, NULL);
    assert(rc_old == -1);
}

/* 9. bad json */
static void t_bad_json(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "not json", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_JSON\""));
    assert(strstr(out, "\"i\":0"));
}

/* 10. unknown verb */
static void t_unknown_verb(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"frobnicate\",\"i\":7}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_VERB\""));
    assert(strstr(out, "\"i\":7"));
}

/* 11. store NVS_FAIL on write */
static void t_nvs_fail_write(void)
{
    reset_store();
    g_fail_write = 1;
    struct proto_store s = make_store();
    struct profile p = make_full_profile();
    char b64[256];
    int bn = profile_to_b64(&p, b64, (int)sizeof b64);
    assert(bn > 0);
    char line[512];
    snprintf(line, sizeof line, "{\"t\":\"write\",\"i\":11,\"n\":0,\"data\":\"%s\"}", b64);
    char out[512];
    int rc = proto_handle(&s, line, out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"NVS_FAIL\""));
    assert(strstr(out, "\"i\":11"));
}

/* 11b. store NVS_FAIL on read */
static void t_nvs_fail_read(void)
{
    reset_store();
    g_fail_read = 1;
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"read\",\"i\":12,\"n\":0}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"NVS_FAIL\""));
    assert(strstr(out, "\"i\":12"));
}

/* ---------- adversarial / bounds-safety tests ---------- */

/* 12. outcap too small for a VALID response → -1 (no overrun). */
static void t_outcap_overflow(void)
{
    reset_store();
    g_arr[0] = make_full_profile();
    struct proto_store s = make_store();
    char out[256];   /* a read_r with the full base64 profile will not fit in 64 */
    int rc = proto_handle(&s, "{\"t\":\"read\",\"i\":2,\"n\":0}", out, 64, NULL);
    assert(rc == -1);
    /* also a tiny outcap for hello must not overrun */
    rc = proto_handle(&s, "{\"t\":\"hello\",\"i\":1}", out, 8, NULL);
    assert(rc == -1);
    (void)out;
}

/* 13. hostile input: huge "data" value, unterminated brace, no closing quote.
 * Must not crash / overrun; must return an err or -1. */
static void t_hostile_input(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[256];

    /* huge data value far longer than any internal buffer */
    static char big[4096];
    int o = 0;
    o += snprintf(big + o, sizeof big - o, "{\"t\":\"write\",\"i\":13,\"n\":0,\"data\":\"");
    while (o < (int)sizeof big - 8) big[o++] = 'A';
    big[o++] = '"';
    big[o++] = '}';
    big[o] = '\0';
    int rc = proto_handle(&s, big, out, (int)sizeof out, NULL);
    /* either a structured err (fits) or -1 (didn't fit) — never a crash. */
    assert(rc == -1 || strstr(out, "\"t\":\"err\""));

    /* unterminated brace */
    rc = proto_handle(&s, "{", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));

    /* no closing quote on the verb */
    rc = proto_handle(&s, "{\"t\":\"hel", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));

    /* empty line */
    rc = proto_handle(&s, "", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));

    /* 4KB line into a small outcap → -1, no overrun */
    char small[32];
    rc = proto_handle(&s, big, small, (int)sizeof small, NULL);
    assert(rc == -1);
}

/* 14. reset one slot */
static void t_reset(void)
{
    reset_store();
    g_arr[2] = make_full_profile();   /* make slot 2 distinctly non-default */
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"reset\",\"i\":14,\"n\":2}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"reset_r\""));
    assert(strstr(out, "\"n\":2"));
    assert(strstr(out, "\"i\":14"));
    assert(strstr(out, "\"ok\":true"));
    /* the store's reset must have fired exactly once for slot 2 */
    assert(g_reset_calls == 1);
    assert(g_reset_n == 2);
    assert(g_resetall_calls == 0);
}

/* 15. reset bad index */
static void t_reset_bad_index(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"reset\",\"i\":15,\"n\":99}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"BAD_INDEX\""));
    assert(strstr(out, "\"i\":15"));
    assert(g_reset_calls == 0);   /* store untouched on a bad index */
}

/* 16. reset NVS_FAIL */
static void t_reset_nvs_fail(void)
{
    reset_store();
    g_fail_reset = 1;
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"reset\",\"i\":16,\"n\":0}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"NVS_FAIL\""));
    assert(strstr(out, "\"i\":16"));
}

/* 17. resetall */
static void t_resetall(void)
{
    reset_store();
    for (int i = 0; i < MOCK_PROFILES; i++)
        g_arr[i] = make_full_profile();   /* every slot distinctly non-default */
    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"resetall\",\"i\":17}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"resetall_r\""));
    assert(strstr(out, "\"i\":17"));
    assert(strstr(out, "\"ok\":true"));
    assert(g_resetall_calls == 1);
    assert(g_reset_calls == 0);
    /* NVS_FAIL path: reset_all returns nonzero -> err */
    reset_store();
    g_fail_reset = 1;
    s = make_store();
    rc = proto_handle(&s, "{\"t\":\"resetall\",\"i\":18}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"err\""));
    assert(strstr(out, "\"code\":\"NVS_FAIL\""));
    assert(strstr(out, "\"i\":18"));
}

/* 18. mode get/set/bad-value */
static void t_mode_get_set(void)
{
    reset_store();
    struct proto_store s = make_store();
    char out[512];

    /* get defaults to 0 */
    int n = proto_handle(&s, "{\"t\":\"mode\",\"i\":1}", out, sizeof out, NULL);
    assert(n > 0);
    assert(strstr(out, "\"t\":\"mode_r\"") && strstr(out, "\"v\":0"));

    /* set to 1 */
    n = proto_handle(&s, "{\"t\":\"mode\",\"v\":1,\"i\":2}", out, sizeof out, NULL);
    assert(n > 0);
    assert(strstr(out, "\"t\":\"mode_r\"") && strstr(out, "\"v\":1"));

    /* get now reflects 1 */
    n = proto_handle(&s, "{\"t\":\"mode\",\"i\":3}", out, sizeof out, NULL);
    assert(strstr(out, "\"v\":1"));

    /* bad value -> err */
    n = proto_handle(&s, "{\"t\":\"mode\",\"v\":2,\"i\":4}", out, sizeof out, NULL);
    assert(strstr(out, "\"t\":\"err\"") && strstr(out, "BAD_VALUE"));
}

int main(void)
{
    t_hello();
    t_read();
    t_write();
    t_write_bad_len();
    t_write_bad_version();
    t_setactive_getactive();
    t_setactive_within_vs_global_bounds();
    t_read_bad_index();
    t_monset();
    t_monset_result_flag();
    t_list_all_banks();
    t_read_write_other_bank();
    t_list_hostile_name();
    t_list_worst_case_fits_g_resp();
    t_bad_json();
    t_unknown_verb();
    t_nvs_fail_write();
    t_nvs_fail_read();
    t_outcap_overflow();
    t_hostile_input();
    t_reset();
    t_reset_bad_index();
    t_reset_nvs_fail();
    t_resetall();
    t_mode_get_set();
    printf("all protocol tests passed\n");
    return 0;
}
