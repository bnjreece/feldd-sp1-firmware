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
#define MOCK_PROFILES 8
static struct profile g_arr[MOCK_PROFILES];
static uint8_t        g_active;
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
static int mock_set_active(uint8_t n)
{
    if (n >= MOCK_PROFILES) return 1;
    g_active = n;
    return 0;
}
static uint8_t mock_get_active(void) { return g_active; }
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

static struct proto_store make_store(void)
{
    struct proto_store s;
    s.read = mock_read;
    s.write = mock_write;
    s.set_active = mock_set_active;
    s.get_active = mock_get_active;
    s.reset = mock_reset;
    s.reset_all = mock_reset_all;
    s.profiles = MOCK_PROFILES;
    s.faders = NUM_FADERS;
    s.buttons = NUM_BUTTONS;
    s.fw = "1.2.3";
    s.uid = "0011223344556677";
    return s;
}

static void reset_store(void)
{
    memset(g_arr, 0, sizeof g_arr);
    g_active = 0;
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
    assert(strstr(out, "\"profiles\":8"));
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
    assert(g_active == 3);

    rc = proto_handle(&s, "{\"t\":\"getactive\",\"i\":5}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"getactive_r\""));
    assert(strstr(out, "\"active\":3"));
    assert(strstr(out, "\"i\":5"));
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

/* 8c. list -> list_r with one {n,name,ver} entry per slot, plus active.
 * Names are decoded straight from profile.name[16] (NOT base64), and the
 * whole response must fit the firmware's 256-byte g_resp buffer. */
static void t_list(void)
{
    reset_store();
    /* Give three slots distinct, length-varied names; leave the rest zeroed
     * (empty name -> "", version 0). */
    static const char *nm[3] = { "OP-XY mix", "Logic", "X" };
    for (int k = 0; k < 3; k++) {
        struct profile p = make_full_profile();
        memset(p.name, 0, sizeof p.name);
        memcpy(p.name, nm[k], strlen(nm[k]));
        g_arr[k] = p;                 /* version == PROFILE_VERSION */
    }
    mock_set_active(2);

    struct proto_store s = make_store();
    char out[512];
    int rc = proto_handle(&s, "{\"t\":\"list\",\"i\":5}", out, (int)sizeof out, NULL);
    assert(rc > 0);
    assert(strstr(out, "\"t\":\"list_r\""));
    assert(strstr(out, "\"i\":5"));
    assert(strstr(out, "\"active\":2"));
    /* each populated slot's name + index appears */
    assert(strstr(out, "\"n\":0"));
    assert(strstr(out, "\"name\":\"OP-XY mix\""));
    assert(strstr(out, "\"name\":\"Logic\""));
    assert(strstr(out, "\"name\":\"X\""));
    /* version field per slot, derived from profile.version */
    char vpat[24];
    snprintf(vpat, sizeof vpat, "\"ver\":%d", PROFILE_VERSION);
    assert(strstr(out, vpat));
    /* a zeroed slot decodes to an empty name, not garbage past the NUL */
    assert(strstr(out, "\"name\":\"\""));

    /* CRITICAL: the full 8-slot list must fit the real firmware response
     * buffer (config_cdc.c g_resp, sized to hold the worst-case list:
     * 8 * {"n":N,"name":"<=16>","ver":VVV} ~= 405B, so 512). proto_handle
     * returns -1 if it would not fit. (The plan's original 256 underbudgeted
     * the list verb: even 3 short names + 5 empty slots is 267B; g_resp was
     * bumped to 512 to match.) */
    char small[512];
    rc = proto_handle(&s, "{\"t\":\"list\",\"i\":5}", small, (int)sizeof small, NULL);
    assert(rc > 0);
    assert(rc < (int)sizeof small);
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
    char out[512];
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
    char out[256];   /* a read_r with 92-char base64 will not fit */
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

int main(void)
{
    t_hello();
    t_read();
    t_write();
    t_write_bad_len();
    t_write_bad_version();
    t_setactive_getactive();
    t_read_bad_index();
    t_monset();
    t_monset_result_flag();
    t_list();
    t_list_hostile_name();
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
    printf("all protocol tests passed\n");
    return 0;
}
