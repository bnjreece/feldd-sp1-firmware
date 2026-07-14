/*
 * protocol.c — JSON-lines config protocol: request line -> response line.
 *
 * Pure & freestanding: <stdint.h>, <string.h>, <stdio.h> (snprintf only; no
 * malloc). The flat-JSON parser is hand-rolled and bounds-safe: it never reads
 * past the NUL terminator of `line`, and every write goes through snprintf with
 * the caller's outcap (truncation -> -1, never an overrun).
 *
 * Sizes/versions are derived from sizeof(struct profile) / PROTO_VERSION /
 * PROFILE_VERSION — no magic wire numbers.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "protocol.h"
#include "led_override.h"

/* The caps array advertised in hello_r. Hardcoded for this task per spec. */
#define CAPS_JSON "[\"trs\",\"usbmidi\",\"shift\",\"led\",\"mon\"]"

/* ------------------------------------------------------------------ */
/* Minimal flat-JSON field extractor.                                  */
/*                                                                     */
/* These operate on a NUL-terminated `line`. They only understand flat */
/* objects (no nesting, no arrays in requests, no string escapes — the */
/* only string values are base64 / plain ASCII verb names, neither of  */
/* which contains a '"' or '\\'). All scanning stops at the NUL.       */
/* ------------------------------------------------------------------ */

/* Locate the value position for "key" in line. On success returns a pointer to
 * the first character AFTER the colon-and-whitespace (i.e. the start of the
 * value). Returns NULL if the key is absent. Bounds: never reads past NUL. */
static const char *find_value(const char *line, const char *key)
{
    size_t klen = strlen(key);
    const char *p = line;

    while (*p) {
        /* find an opening quote that starts a key token */
        if (*p != '"') { p++; continue; }
        const char *kstart = p + 1;
        /* match key chars */
        size_t i = 0;
        while (i < klen && kstart[i] && kstart[i] == key[i])
            i++;
        if (i == klen && kstart[klen] == '"') {
            /* matched "key"; expect a ':' (allow spaces) */
            const char *q = kstart + klen + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                return q;
            }
            /* "key" not followed by ':' — keep scanning */
            p = kstart;     /* advance past this quote */
            continue;
        }
        /* not our key — skip this quoted token entirely so we don't match a
         * key substring inside a value. Advance to the closing quote. */
        const char *q = kstart;
        while (*q && *q != '"') q++;
        if (*q == '"') q++;          /* past closing quote */
        else            return NULL; /* unterminated string -> give up */
        p = q;
    }
    return NULL;
}

/* Extract a string value for `key` into buf (NUL-terminated, capacity buflen).
 * Returns the copied length on success; -1 if key absent, value isn't a string,
 * the string is unterminated, or it doesn't fit in buf. Bounds-safe. */
static int json_str(const char *line, const char *key, char *buf, int buflen)
{
    const char *v = find_value(line, key);
    if (!v || *v != '"') return -1;
    v++;                                  /* first char of the string */
    int i = 0;
    while (*v && *v != '"') {
        if (i >= buflen - 1) return -1;   /* would overflow buf */
        buf[i++] = *v++;
    }
    if (*v != '"') return -1;             /* unterminated */
    buf[i] = '\0';
    return i;
}

/* Extract an unsigned-int value for `key` into *out.
 * Returns 0 on success, -1 if key absent or value isn't a plain decimal uint. */
static int json_uint(const char *line, const char *key, uint32_t *out)
{
    const char *v = find_value(line, key);
    if (!v) return -1;
    if (*v < '0' || *v > '9') return -1;  /* must start with a digit */
    uint32_t acc = 0;
    int any = 0;
    while (*v >= '0' && *v <= '9') {
        acc = acc * 10u + (uint32_t)(*v - '0');
        any = 1;
        v++;
    }
    if (!any) return -1;
    *out = acc;
    return 0;
}

/* Extract a bool value for `key` into *out (0/1).
 * Returns 0 on success, -1 if key absent or value isn't true/false. */
static int json_bool(const char *line, const char *key, int *out)
{
    const char *v = find_value(line, key);
    if (!v) return -1;
    if (strncmp(v, "true", 4) == 0)  { *out = 1; return 0; }
    if (strncmp(v, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Response emitters. Each writes via snprintf and returns the length, */
/* or -1 if it would not fit (snprintf truncation == overflow).        */
/* ------------------------------------------------------------------ */

static int emit(char *out, int outcap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, (size_t)(outcap > 0 ? outcap : 0), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= outcap) return -1;   /* truncated / error -> overflow */
    return n;
}

static int emit_err(char *out, int outcap, uint32_t id,
                    const char *code, const char *msg)
{
    return emit(out, outcap,
                "{\"t\":\"err\",\"i\":%u,\"ok\":false,\"code\":\"%s\",\"msg\":\"%s\"}",
                id, code, msg);
}

/* ------------------------------------------------------------------ */
/* Dispatch.                                                           */
/* ------------------------------------------------------------------ */

int proto_handle(const struct proto_store *s, const char *line,
                 char *out, int outcap, struct proto_result *res)
{
    char verb[24];

    if (res) {
        res->mon_set = 0;
        res->mon_on  = 0;
    }

    /* Extract the verb. Absent/unparseable -> BAD_JSON with i:0. */
    if (json_str(line, "t", verb, (int)sizeof verb) < 0)
        return emit_err(out, outcap, 0, "BAD_JSON", "bad json");

    /* Extract request id; default 0 if absent. */
    uint32_t id = 0;
    json_uint(line, "i", &id);   /* ignore failure -> stays 0 */

    /* ---- hello ---- */
    if (strcmp(verb, "hello") == 0) {
        return emit(out, outcap,
            "{\"t\":\"hello_r\",\"i\":%u,\"ok\":true,\"proto\":%d,\"pver\":%d,"
            "\"fw\":\"%s\",\"profiles\":%u,\"active\":%u,\"faders\":%u,\"buttons\":%u,"
            "\"caps\":" CAPS_JSON ",\"pbytes\":%d,\"uid\":\"%s\"}",
            id, PROTO_VERSION, PROFILE_VERSION, s->fw,
            (unsigned)s->profiles, (unsigned)s->get_active(),
            (unsigned)s->faders, (unsigned)s->buttons,
            (int)sizeof(struct profile),
            s->uid ? s->uid : "");
    }

    /* ---- read ---- */
    if (strcmp(verb, "read") == 0) {
        uint32_t n;
        if (json_uint(line, "n", &n) < 0 || n >= s->profiles)
            return emit_err(out, outcap, id, "BAD_INDEX", "bad index");
        struct profile p;
        if (s->read((uint8_t)n, &p) != 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "nvs read failed");
        char b64[((sizeof(struct profile) + 2) / 3) * 4 + 1];
        if (profile_to_b64(&p, b64, (int)sizeof b64) < 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "encode failed");
        return emit(out, outcap,
            "{\"t\":\"read_r\",\"i\":%u,\"ok\":true,\"n\":%u,\"data\":\"%s\"}",
            id, (unsigned)n, b64);
    }

    /* ---- write ---- */
    if (strcmp(verb, "write") == 0) {
        uint32_t n;
        if (json_uint(line, "n", &n) < 0 || n >= s->profiles)
            return emit_err(out, outcap, id, "BAD_INDEX", "bad index");
        /* data b64: cap at the exact valid length (+1 NUL). Anything longer is
         * by definition wrong-length -> BAD_LEN, and json_str returns -1. */
        char b64[((sizeof(struct profile) + 2) / 3) * 4 + 1];
        int blen = json_str(line, "data", b64, (int)sizeof b64);
        if (blen < 0)
            return emit_err(out, outcap, id, "BAD_LEN", "bad data length");
        struct profile p;
        if (profile_from_b64(b64, blen, &p) != 0)
            return emit_err(out, outcap, id, "BAD_LEN", "bad data length");
        if (profile_validate(&p) != 0)
            return emit_err(out, outcap, id, "BAD_VERSION", "bad profile version");
        if (s->write((uint8_t)n, &p) != 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "nvs write failed");
        return emit(out, outcap,
            "{\"t\":\"write_r\",\"i\":%u,\"ok\":true,\"n\":%u}",
            id, (unsigned)n);
    }

    /* ---- setactive ---- */
    /* n is a WITHIN-bank index (0..bank_profiles-1), NOT a global slot: it selects
     * one of the CURRENT mode's profiles (the •• cycle's axis). Bound it against
     * bank_profiles (=8), not profiles (=16); otherwise n=8..15 would pass the
     * gate and set_active would reject it with -EINVAL, surfaced as a misleading
     * NVS_FAIL. The other bank is reachable by flipping mode, not by setactive. */
    if (strcmp(verb, "setactive") == 0) {
        uint32_t n;
        if (json_uint(line, "n", &n) < 0 || n >= s->bank_profiles)
            return emit_err(out, outcap, id, "BAD_INDEX", "bad index");
        if (s->set_active((uint8_t)n) != 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "nvs set_active failed");
        return emit(out, outcap,
            "{\"t\":\"setactive_r\",\"i\":%u,\"ok\":true,\"active\":%u}",
            id, (unsigned)n);
    }

    /* ---- getactive ---- */
    if (strcmp(verb, "getactive") == 0) {
        return emit(out, outcap,
            "{\"t\":\"getactive_r\",\"i\":%u,\"ok\":true,\"active\":%u}",
            id, (unsigned)s->get_active());
    }

    /* ---- reset ---- */
    if (strcmp(verb, "reset") == 0) {
        uint32_t n;
        if (json_uint(line, "n", &n) < 0 || n >= s->profiles)
            return emit_err(out, outcap, id, "BAD_INDEX", "bad index");
        if (s->reset((uint8_t)n) != 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "nvs reset failed");
        return emit(out, outcap,
            "{\"t\":\"reset_r\",\"i\":%u,\"ok\":true,\"n\":%u}",
            id, (unsigned)n);
    }

    /* ---- resetall ---- */
    if (strcmp(verb, "resetall") == 0) {
        if (s->reset_all() != 0)
            return emit_err(out, outcap, id, "NVS_FAIL", "nvs reset_all failed");
        return emit(out, outcap,
            "{\"t\":\"resetall_r\",\"i\":%u,\"ok\":true}",
            id);
    }

    /* ---- monset ---- */
    if (strcmp(verb, "monset") == 0) {
        int on;
        if (json_bool(line, "on", &on) < 0)
            return emit_err(out, outcap, id, "BAD_JSON", "missing on flag");
        if (res) {
            res->mon_set = 1;
            res->mon_on  = (uint8_t)(on ? 1 : 0);
        }
        return emit(out, outcap,
            "{\"t\":\"monset_r\",\"i\":%u,\"ok\":true,\"on\":%s}",
            id, on ? "true" : "false");
    }

    /* ---- mode ---- */
    /* {"t":"mode"} reads, {"t":"mode","v":0|1} sets the global device mode. */
    if (strcmp(verb, "mode") == 0) {
        uint32_t v;
        if (json_uint(line, "v", &v) == 0) {
            if (v > 1)
                return emit_err(out, outcap, id, "BAD_VALUE", "mode must be 0 or 1");
            if (s->set_mode((uint8_t)v) != 0)
                return emit_err(out, outcap, id, "NVS_FAIL", "nvs set_mode failed");
        }
        return emit(out, outcap,
            "{\"t\":\"mode_r\",\"i\":%u,\"ok\":true,\"v\":%u}",
            id, (unsigned)s->get_mode());
    }

    /* ---- playrole ---- */
    /* {"t":"playrole"} reads; {"t":"playrole","v":0|1} sets PLAY's role. */
    if (strcmp(verb, "playrole") == 0) {
        uint32_t v;
        if (json_uint(line, "v", &v) == 0) {
            if (v > 1)
                return emit_err(out, outcap, id, "BAD_VALUE", "playrole must be 0 or 1");
            if (s->set_playrole((uint8_t)v) != 0)
                return emit_err(out, outcap, id, "NVS_FAIL", "nvs set_playrole failed");
        }
        return emit(out, outcap,
            "{\"t\":\"playrole_r\",\"i\":%u,\"ok\":true,\"v\":%u}",
            id, (unsigned)s->get_playrole());
    }

    /* ---- list ---- */
    /* list_r{mode,active,profiles:[{n,bank,name,ver}]}. We read each slot's
     * profile and emit only its GLOBAL index, bank, name, and version (NOT the
     * base64 blob) so the librarian can render both banks' names without decoding
     * 16 full profiles. §0 mode-scoped banks: ALL profiles (0..NUM_PROFILES-1) are
     * returned, each tagged with bank = n / NUM_BANK_PROFILES (0=MIDI, 1=Keyboard),
     * plus the current `mode` and the within-mode `active` at the top so the web
     * shows both banks together and highlights the active one.
     *
     * Budget (WORST case — must NOT under-count the sanitizer's expansion):
     *   name[16] is user-supplied and NOT constrained by profile_validate, so a
     *   host can write all 16 bytes as '"' (or '\\'); the sanitizer below prepends
     *   a backslash for EACH, so name[16] emits up to 32 wire bytes, not 16.
     *   Per entry "{\"n\":NN,\"bank\":B,\"name\":\"<<=32>>\",\"ver\":VVV}" + comma:
     *     fixed structural chars (incl. comma, n=2, bank=1, ver=3 digits) = 38
     *     + escaped name (16 -> 32)                                       = 32
     *     = 70 bytes / entry.  16 entries = 1120.
     *   Header "{\"t\":\"list_r\",\"i\":<<=10>>,\"mode\":M,\"active\":A,\"profiles\":["
     *   is <= 61 (i is host-supplied %u, up to 10 digits); footer "]}" = 2.
     *   Worst total = 1120 + 61 + 2 = 1183 (+ NUL = 1184). config_cdc.c sizes
     *   g_resp to 1280 (>= 1184) to hold this. The prior 768 UNDER-budgeted (it
     *   counted name[16] as 16 wire bytes), so the worst-case list returned -1 and
     *   the host could not enumerate profiles — exactly the under-budgeting class
     *   the earlier "the plan's original 256 underbudgeted the list verb" warned of.
     *   The emit() path is snprintf-bounded: if the array ever overruns outcap,
     *   proto_handle returns -1 and the caller reports OVERFLOW rather than
     *   truncating (no corruption). version is a single byte (0..255 -> <=3
     *   digits). */
    if (strcmp(verb, "list") == 0) {
        int off = emit(out, outcap,
            "{\"t\":\"list_r\",\"i\":%u,\"mode\":%u,\"active\":%u,\"profiles\":[",
            id, (unsigned)s->get_mode(), (unsigned)s->get_active());
        if (off < 0)
            return -1;
        for (unsigned n = 0; n < s->profiles; n++) {
            struct profile p;
            if (s->read((uint8_t)n, &p) != 0)
                return emit_err(out, outcap, id, "NVS_FAIL", "nvs read failed");
            /* name[16] is raw, NUL-terminated-or-not, and NOT validated, so it
             * may contain bytes that corrupt the newline-delimited JSON line
             * protocol (0x0A/0x0D, '"', '\\', or other controls). Sanitize on
             * emit: stop at the first NUL, drop any control byte (< 0x20) so no
             * raw newline/CR/control reaches the wire, and JSON-escape '"' and
             * '\\'. Worst case 16 bytes each escaped to 2 -> 32 + NUL. */
            char name[33];
            int j = 0;
            for (int k = 0; k < 16 && p.name[k]; k++) {
                uint8_t c = p.name[k];
                if (c < 0x20)
                    continue;                 /* drop control bytes */
                if (c == '"' || c == '\\')
                    name[j++] = '\\';          /* escape */
                name[j++] = (char)c;
            }
            name[j] = '\0';
            unsigned bank = n / NUM_BANK_PROFILES;   /* 0..7 -> 0, 8..15 -> 1 */
            int w = emit(out + off, outcap - off,
                "%s{\"n\":%u,\"bank\":%u,\"name\":\"%s\",\"ver\":%u}",
                n ? "," : "", n, bank, name, (unsigned)p.version);
            if (w < 0)
                return -1;
            off += w;
        }
        int w = emit(out + off, outcap - off, "]}");
        if (w < 0)
            return -1;
        return off + w;
    }

    /* ---- led ---- */
    /* host-LED override for the Claude Code console (feldd-cc): drive the 8 LEDs
     * from the host. {"ix":N,"on":B} = one LED; {"mask":M} = all 8 (bit i = LED i);
     * {"release":true} = hand the LEDs back to the normal render.
     * ix 0-3 = SP1_TRACK_LED1..4, ix 4-7 = SP1_PLAY_LED1..4. Calls the global
     * host_led_* directly (pure state module, no proto_store plumbing). */
    if (strcmp(verb, "led") == 0) {
        int rel = 0, on;
        uint32_t mask, ix;
        if (json_bool(line, "release", &rel) == 0 && rel) {
            host_led_release();
        } else if (json_uint(line, "mask", &mask) == 0) {
            if (mask > 255)
                return emit_err(out, outcap, id, "BAD_VALUE", "mask 0..255");
            host_led_mask((uint8_t)mask);
        } else if (json_uint(line, "ix", &ix) == 0) {
            if (ix > 7)
                return emit_err(out, outcap, id, "BAD_INDEX", "ix 0..7");
            if (json_bool(line, "on", &on) < 0)
                return emit_err(out, outcap, id, "BAD_JSON", "missing on flag");
            host_led_set((uint8_t)ix, on != 0);
        } else {
            return emit_err(out, outcap, id, "BAD_JSON", "need ix+on, mask, or release");
        }
        return emit(out, outcap,
            "{\"t\":\"led_r\",\"i\":%u,\"ok\":true,\"active\":%s,\"mask\":%u}",
            id, host_led_active() ? "true" : "false", (unsigned)host_led_get_mask());
    }

#ifdef CONFIG_FELDD_BT_PROVISION
    /* ---- bt_ss_probe (Q5 P0 stage 1, READ-ONLY) ---- */
    /* {"t":"bt_ss_probe"} -> drive the onboard CYW20706 into download mode, READ its 64-byte
     * Static Section (NO flash write, ever), run the template gate, and report. The hardware
     * work is done by s->bt_ss_probe (wired to bt_provision_probe_ss in config_cdc.c); this
     * layer only formats the JSON reply, so protocol.c stays hardware-free + host-testable.
     * status 0 == OK; ss is 128 hex chars (the 64 SS bytes). */
    if (strcmp(verb, "bt_ss_probe") == 0) {
        if (!s->bt_ss_probe)
            return emit_err(out, outcap, id, "NO_PROVISION", "ss probe unavailable");
        uint8_t ss[64] = {0};
        int gate = -1;
        int st = s->bt_ss_probe(ss, &gate);
        char hex[129];
        static const char H[] = "0123456789abcdef";
        for (int k = 0; k < 64; k++) {
            hex[2 * k]     = H[(ss[k] >> 4) & 0x0F];
            hex[2 * k + 1] = H[ss[k] & 0x0F];
        }
        hex[128] = '\0';
        return emit(out, outcap,
            "{\"t\":\"bt_ss_probe_r\",\"i\":%u,\"ok\":%s,\"status\":%d,\"gate\":%d,\"ss\":\"%s\"}",
            id, (st == 0) ? "true" : "false", st, gate, hex);
    }

    /* ---- bt_provision (Q5 P0 stage 2, DS FLASH WRITE) ---- */
    /* {"t":"bt_provision"} -> power-gate, then (only on GATE_OK + the ds_base assert) DS-only
     * write our BLE-MIDI app to the compile-time CYBT_DS_BASE_ADDR, verify, prove the SS is
     * untouched, cold-boot the new app. The hardware work + all safety gating is done by
     * s->bt_provision (wired to bt_provision_run in config_cdc.c); this layer only formats
     * the JSON reply, so protocol.c stays hardware-free + host-testable. status 0 == OK. */
    if (strcmp(verb, "bt_provision") == 0) {
        if (!s->bt_provision)
            return emit_err(out, outcap, id, "NO_PROVISION", "provision unavailable");
        int gate = -1;
        int st = s->bt_provision(&gate);
        return emit(out, outcap,
            "{\"t\":\"bt_provision_r\",\"i\":%u,\"ok\":%s,\"status\":%d,\"gate\":%d}",
            id, (st == 0) ? "true" : "false", st, gate);
    }
#endif

    /* ---- unknown verb ---- */
    return emit_err(out, outcap, id, "BAD_VERB", "unknown verb");
}
