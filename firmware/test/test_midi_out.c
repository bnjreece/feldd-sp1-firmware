/*
 * Host test for the USB last-value-wins pending table in midi_out.c, focused on
 * the note-non-lossy eviction guarantee (the MED fix).
 *
 * midi_out.c is compiled with -DMIDI_OUT_HOST_TEST so its Zephyr I/O shell (TRS
 * uart, ring GPIO, real usbd_midi_send) is excluded. This file PROVIDES the
 * usb_try_send seam: it simulates a USB TX ring that is "full" (-ENOBUFS) or
 * "drained" (0), and records every (status,d1,d2) the firmware hands it so we
 * can prove that a queued NOTE OFF / NOTE ON is never silently dropped under
 * eviction, while CC (0xB0) remains last-value-wins.
 *
 * Key fact about the table: it is keyed by (status, d1). A NOTE ON and its
 * NOTE OFF on the same channel+note share d1 (the note number) but differ in
 * status high-nibble (0x90 vs 0x80), so they are DISTINCT keys and both occupy
 * slots — exactly the situation that can fill the table with un-droppable note
 * events.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "midi_out.h"

/* ---- usb_try_send seam: a fake USB TX ring the test controls --------------- */

#include <errno.h>

/* Realistic fake USB TX ring: it holds at most g_ring_cap in-flight messages.
 * Each usb_try_send first lets the host drain up to g_ring_drain messages (the
 * host polls continuously, so room frees up between our calls), then accepts the
 * new message if there is room, else returns -ENOBUFS. Anything accepted is
 * recorded as "delivered to the host".
 *
 *   g_ring_cap   = ring depth (messages)
 *   g_ring_drain = host-drained-per-call. 0 = host stalled (ring stays full);
 *                  >0 = host polling (a synchronous flush makes real progress).
 *   g_ring_used  = messages currently in-flight (not yet drained by host). */
static int g_ring_cap   = 1000;     /* effectively open unless a test shrinks it */
static int g_ring_drain = 1000;     /* host keeps up unless a test stalls it */
static int g_ring_used;

struct sent_rec { uint8_t status, d1, d2; };
static struct sent_rec g_sent[512];
static int             g_sent_n;

int usb_try_send(const struct midi_msg *m)
{
    /* host drains first (room frees between calls) */
    g_ring_used -= g_ring_drain;
    if (g_ring_used < 0) {
        g_ring_used = 0;
    }
    if (g_ring_used >= g_ring_cap) {
        return -ENOBUFS;            /* ring full -> backpressure */
    }
    g_ring_used++;                  /* one more in-flight */
    if (g_sent_n < (int)(sizeof(g_sent) / sizeof(g_sent[0]))) {
        g_sent[g_sent_n].status = m->status;
        g_sent[g_sent_n].d1     = m->d1;
        g_sent[g_sent_n].d2     = m->d2;
        g_sent_n++;
    }
    return 0;
}

/* Did we ever successfully send a message with this exact (status,d1,d2)? */
static int sent_contains(uint8_t status, uint8_t d1, uint8_t d2)
{
    for (int i = 0; i < g_sent_n; i++) {
        if (g_sent[i].status == status && g_sent[i].d1 == d1 &&
            g_sent[i].d2 == d2) {
            return 1;
        }
    }
    return 0;
}

static void reset(void)
{
    /* Drain table + ring + recorder between cases: open the ring wide, pump so
     * every queued entry lands and frees its slot, then zero the recorder. */
    g_ring_cap   = 1000;
    g_ring_drain = 1000;
    g_ring_used  = 0;
    midi_out_pump();
    g_sent_n    = 0;
    g_ring_used = 0;
}

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static struct midi_msg note_on(uint8_t ch, uint8_t note, uint8_t vel)
{
    struct midi_msg m = { (uint8_t)(0x90 | (ch & 0x0f)), note, vel, 3 };
    return m;
}
static struct midi_msg note_off(uint8_t ch, uint8_t note)
{
    struct midi_msg m = { (uint8_t)(0x80 | (ch & 0x0f)), note, 0, 3 };
    return m;
}
static struct midi_msg cc(uint8_t ch, uint8_t num, uint8_t val)
{
    struct midi_msg m = { (uint8_t)(0xB0 | (ch & 0x0f)), num, val, 3 };
    return m;
}

/* ---- the bug: a queued note must never be dropped by eviction -------------- */

/* PENDING_CAP is 16 in midi_out.c. Fill the table with 16 DISTINCT note keys
 * while the ring is full, then push one more note. Under the old oldest-wins
 * eviction the first queued note (a NOTE OFF) was evicted and, because the ring
 * was still full, never sent — a permanently stuck note. The fix must make that
 * note reach usb_try_send (via the forced flush / direct send) instead. */
static void test_full_of_notes_does_not_drop_a_note(void)
{
    reset();
    g_sent_n = 0;
    /* Ring fully stalled: cap 0 => every usb_try_send is -ENOBUFS, so the 16
     * sends below all queue into the pending table and nothing lands. */
    g_ring_cap = 0;
    g_ring_drain = 0;
    g_ring_used = 0;

    /* Slot 0: a NOTE OFF for ch0 note 60 — the one the old code would evict
     * first. It is the "release" of a sounding note: dropping it = stuck note. */
    struct midi_msg victim = note_off(0, 60);
    midi_out_send(&victim, NULL);

    /* Fill the remaining 15 slots with distinct NOTE ON keys (notes 61..75). */
    for (int n = 61; n <= 75; n++) {
        struct midi_msg m = note_on(0, (uint8_t)n, 100);
        midi_out_send(&m, NULL);
    }
    /* Table is now full of 16 un-droppable note events, ring still full. */
    CHECK(g_sent_n == 0);   /* nothing has landed yet (ring full) */

    /* The host now begins polling again (ring recovers). A 17th note arrives.
     * With the OLD code, even with the ring recovering, pending_upsert evicted
     * the oldest entry (the ch0/60 NOTE OFF) the instant the new note arrived,
     * losing it before any flush. The fix instead forces a synchronous flush
     * (the queued notes land, freeing slots) and parks the new note — no queued
     * note is ever evicted. */
    g_ring_cap   = 64;     /* ring has room again */
    g_ring_drain = 64;     /* host is polling: a synchronous flush drains it */
    struct midi_msg extra = note_on(0, 76, 100);
    midi_out_send(&extra, NULL);

    /* Land anything still queued. */
    midi_out_pump();

    CHECK(sent_contains(0x80, 60, 0));    /* the NOTE OFF survived -> no stuck note */
    /* And every note we queued is accounted for (none silently dropped). */
    for (int n = 61; n <= 76; n++) {
        CHECK(sent_contains(0x90, (uint8_t)n, 100));
    }
}

/* When the table is full but contains a CC, the CC (last-value-wins, harmless to
 * drop) must be the eviction victim, NOT a note. */
static void test_cc_is_evicted_before_a_note(void)
{
    reset();
    g_sent_n = 0;
    g_ring_cap = 0;        /* stalled: everything queues */
    g_ring_drain = 0;
    g_ring_used = 0;

    /* One CC (oldest), then 15 distinct notes -> table full (16). */
    struct midi_msg c = cc(0, 7, 64);          /* ch0 CC7 = volume */
    midi_out_send(&c, NULL);
    for (int n = 60; n <= 74; n++) {
        struct midi_msg m = note_on(0, (uint8_t)n, 90);
        midi_out_send(&m, NULL);
    }

    /* A new note arrives while the ring is STILL stalled. There is no free slot,
     * but there is a CC to evict, so the fix evicts the CC (last-value-wins,
     * harmless) and parks the note — no flush needed, no note touched. */
    struct midi_msg newnote = note_on(0, 80, 90);
    midi_out_send(&newnote, NULL);

    g_ring_cap   = 1000;   /* open the ring and land everything queued */
    g_ring_drain = 1000;
    midi_out_pump();

    /* The new note and all 15 original notes must have landed. */
    CHECK(sent_contains(0x90, 80, 90));
    for (int n = 60; n <= 74; n++) {
        CHECK(sent_contains(0x90, (uint8_t)n, 90));
    }
    /* The CC was the eviction victim, so its (stale) value need not appear; the
     * point is that NO note was lost to make room. (All 16 notes present.) */
}

/* CC behavior unchanged: same (status,d1) key collapses last-value-wins, so only
 * the final value is delivered when the ring drains. */
static void test_cc_last_value_wins_unchanged(void)
{
    reset();
    g_sent_n = 0;
    g_ring_cap = 0;        /* stalled: the three CC writes queue + collapse */
    g_ring_drain = 0;
    g_ring_used = 0;

    /* Same CC key, three values while the ring is full. */
    struct midi_msg a = cc(1, 10, 5);
    struct midi_msg b = cc(1, 10, 9);
    struct midi_msg d = cc(1, 10, 42);
    midi_out_send(&a, NULL);
    midi_out_send(&b, NULL);
    midi_out_send(&d, NULL);

    g_ring_cap   = 1000;   /* open the ring and drain */
    g_ring_drain = 1000;
    midi_out_pump();

    /* Exactly the final value (42) is delivered; the intermediate ones collapsed. */
    CHECK(sent_contains(0xB1, 10, 42));
    CHECK(!sent_contains(0xB1, 10, 5));
    CHECK(!sent_contains(0xB1, 10, 9));
    CHECK(g_sent_n == 1);
}

/* Worst case the fix must still survive: table full of notes AND the host is
 * genuinely stalled, so even the forced flush frees nothing. The new note can't
 * be delivered synchronously (best-effort fails, TRS already carried it), but
 * the invariant that MUST hold is that NO already-queued note was evicted to
 * make room. The old code would have dropped the oldest queued note here. */
static void test_full_of_notes_host_stalled_evicts_no_note(void)
{
    reset();
    g_sent_n = 0;
    g_ring_cap = 0;        /* stalled and stays stalled */
    g_ring_drain = 0;
    g_ring_used = 0;

    struct midi_msg victim = note_off(2, 48);   /* the oldest queued note */
    midi_out_send(&victim, NULL);
    for (int n = 49; n <= 63; n++) {            /* 15 more -> 16 total */
        struct midi_msg m = note_on(2, (uint8_t)n, 110);
        midi_out_send(&m, NULL);
    }

    /* 17th note while still fully stalled: forced flush frees nothing, direct
     * send is rejected. The queued victim NOTE OFF must NOT have been evicted. */
    struct midi_msg extra = note_on(2, 64, 110);
    midi_out_send(&extra, NULL);

    /* Now the host recovers; pump lands everything still queued. */
    g_ring_cap   = 1000;
    g_ring_drain = 1000;
    midi_out_pump();

    CHECK(sent_contains(0x82, 48, 0));          /* victim NOTE OFF survived */
    for (int n = 49; n <= 63; n++) {            /* all 15 NOTE ONs survived */
        CHECK(sent_contains(0x92, (uint8_t)n, 110));
    }
}

int main(void)
{
    test_full_of_notes_does_not_drop_a_note();
    test_full_of_notes_host_stalled_evicts_no_note();
    test_cc_is_evicted_before_a_note();
    test_cc_last_value_wins_unchanged();
    if (g_fail) {
        printf("midi_out tests FAILED\n");
        return 1;
    }
    printf("all midi_out tests passed\n");
    return 0;
}
