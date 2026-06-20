/*
 * Host test for the pure decode + debounce + DFU-band core of buttons.c.
 * buttons.c is compiled with -DBUTTONS_HOST_TEST so its Zephyr I/O shell
 * (buttons_init / controls_read_raw) is excluded; we drive buttons_scan_pure()
 * directly with raw ADC value sequences and assert the emitted edges.
 */
#include <assert.h>
#include <stdio.h>
#include "buttons.h"

/* Pure-core symbols from buttons.c (no public header — declared here). */
int  buttons_decode_tracks_pure(int v);
int  buttons_decode_vol_pure(int v);
int  buttons_in_dfu_band_pure(int v);
struct buttons_state;
void buttons_state_init(struct buttons_state *s);
int  buttons_scan_pure(struct buttons_state *s, int trk_raw, int vol_raw,
                       struct button_event *evt, int cap);

/* The host test needs to allocate a buttons_state. It is an opaque struct in
 * buttons.c; size it generously and alias. Keep this comfortably larger than
 * the real struct (3 ints x 2 debouncers + 2 ints ~= 32 bytes). */
union state_buf { long _align; char raw[128]; };

/* Idle raw codes that decode to "none" on each ladder. */
#define TRK_IDLE 0
#define VOL_IDLE 0
/* Representative plateau codes (looper's measured centres). */
#define TRK_T1   213
#define TRK_T4   1220
#define TRK_PLAY 1823
#define VOL_LO   404   /* lowest vol plateau -> RWD (idx 8) */
#define VOL_HI   1820  /* highest vol plateau -> VolUp (idx 5) */
#define DFU_MID  1325  /* inside the Track1+4 combo band */

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* --- decode boundary tests ------------------------------------------------ */

static void test_decode_tracks_boundaries(void)
{
    CHECK(buttons_decode_tracks_pure(0)    == -1);  /* idle */
    CHECK(buttons_decode_tracks_pure(109)  == -1);  /* just below T1 entry */
    CHECK(buttons_decode_tracks_pure(110)  == 1);   /* Track1 */
    CHECK(buttons_decode_tracks_pure(299)  == 1);
    CHECK(buttons_decode_tracks_pure(300)  == 2);   /* Track2 */
    CHECK(buttons_decode_tracks_pure(559)  == 2);
    CHECK(buttons_decode_tracks_pure(560)  == 3);   /* Track3 */
    CHECK(buttons_decode_tracks_pure(949)  == 3);
    CHECK(buttons_decode_tracks_pure(950)  == 4);   /* Track4 */
    CHECK(buttons_decode_tracks_pure(1499) == 4);
    CHECK(buttons_decode_tracks_pure(1500) == 0);   /* Play (above T4 band) */
    CHECK(buttons_decode_tracks_pure(4095) == 0);
}

static void test_decode_vol_boundaries(void)
{
    CHECK(buttons_decode_vol_pure(0)    == -1);     /* idle */
    CHECK(buttons_decode_vol_pure(199)  == -1);
    CHECK(buttons_decode_vol_pure(200)  == 8);      /* RWD (lowest plateau) */
    CHECK(buttons_decode_vol_pure(559)  == 8);
    CHECK(buttons_decode_vol_pure(560)  == 6);      /* VolDown (bench-confirmed) */
    CHECK(buttons_decode_vol_pure(949)  == 6);
    CHECK(buttons_decode_vol_pure(950)  == 7);      /* FWD (bench-confirmed) */
    CHECK(buttons_decode_vol_pure(1499) == 7);
    CHECK(buttons_decode_vol_pure(1500) == 5);      /* VolUp (highest plateau) */
    CHECK(buttons_decode_vol_pure(4095) == 5);
}

static void test_dfu_band(void)
{
    CHECK(buttons_in_dfu_band_pure(1279) == 0);
    CHECK(buttons_in_dfu_band_pure(1280) == 1);
    CHECK(buttons_in_dfu_band_pure(1325) == 1);
    CHECK(buttons_in_dfu_band_pure(1390) == 1);
    CHECK(buttons_in_dfu_band_pure(1391) == 0);
}

/* --- debounce + edge emission --------------------------------------------- */

/* A plateau held for 3 consecutive scans emits exactly ONE press edge, on the
 * 3rd scan; a steady hold emits nothing further. */
static void test_press_after_three_reads(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 0);  /* read 1: candidate */
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 0);  /* read 2 */
    int n = buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);      /* read 3: commit */
    CHECK(n == 1);
    CHECK(ev[0].idx == 1 && ev[0].pressed == 1);                /* Track1 down */
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 0);  /* steady hold: silent */
}

/* A 1-read glitch (single off-plateau sample) never commits -> no edges. */
static void test_single_glitch_emits_nothing(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    /* Commit Track1 (3 reads). */
    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 1);

    /* One stray idle sample, then back to T1 -> counter resets, no release. */
    CHECK(buttons_scan_pure(s, TRK_IDLE, VOL_IDLE, ev, 4) == 0);
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 0);
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 0);
}

/* Releasing a held button (idle for 3 reads) emits exactly ONE release edge. */
static void test_release_emits_one_up(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 1);  /* press */

    CHECK(buttons_scan_pure(s, TRK_IDLE, VOL_IDLE, ev, 4) == 0);
    CHECK(buttons_scan_pure(s, TRK_IDLE, VOL_IDLE, ev, 4) == 0);
    int n = buttons_scan_pure(s, TRK_IDLE, VOL_IDLE, ev, 4);    /* release */
    CHECK(n == 1);
    CHECK(ev[0].idx == 1 && ev[0].pressed == 0);                /* Track1 up */
}

/* Moving directly between two plateaus emits a release of the old then a press
 * of the new in the same commit scan (both fit in evt[]). */
static void test_change_emits_release_then_press(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4);
    CHECK(buttons_scan_pure(s, TRK_T1, VOL_IDLE, ev, 4) == 1);  /* Track1 committed */

    buttons_scan_pure(s, TRK_PLAY, VOL_IDLE, ev, 4);
    buttons_scan_pure(s, TRK_PLAY, VOL_IDLE, ev, 4);
    int n = buttons_scan_pure(s, TRK_PLAY, VOL_IDLE, ev, 4);    /* Play committed */
    CHECK(n == 2);
    CHECK(ev[0].idx == 1 && ev[0].pressed == 0);                /* Track1 up */
    CHECK(ev[1].idx == 0 && ev[1].pressed == 1);                /* Play down */
}

/* The two ladders are independent: a press on each commits to its own idx. */
static void test_both_ladders_independent(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    buttons_scan_pure(s, TRK_T4, VOL_HI, ev, 4);
    buttons_scan_pure(s, TRK_T4, VOL_HI, ev, 4);
    int n = buttons_scan_pure(s, TRK_T4, VOL_HI, ev, 4);
    CHECK(n == 2);
    /* Tracks ladder emitted first in scan order, then vol ladder. */
    CHECK(ev[0].idx == 4 && ev[0].pressed == 1);                /* Track4 down */
    CHECK(ev[1].idx == 5 && ev[1].pressed == 1);                /* VolUp down */
}

/* The combo band is reported as idle on the tracks ladder (no Track4/Play edge)
 * and must hold ~1.2 s (150 scans) before dfu latches. */
static void test_dfu_band_suppresses_track_edges(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    /* Even held a long while, the combo band emits NO track edges. */
    int total = 0;
    for (int i = 0; i < 160; i++) {
        total += buttons_scan_pure(s, DFU_MID, VOL_IDLE, ev, 4);
    }
    CHECK(total == 0);   /* band decodes to none -> no press/release */
}

/* Leaving the band before 1.2 s resets the hold; the scan count never reaches
 * the latch. We verify indirectly: a short stint then idle, then a real T4
 * press still works (band never poisoned the tracks debounce into a stuck
 * state). */
static void test_dfu_band_short_then_real_press(void)
{
    union state_buf sb; struct buttons_state *s = (struct buttons_state *)&sb;
    buttons_state_init(s);
    struct button_event ev[4];

    for (int i = 0; i < 10; i++) {            /* brief combo flirt, well under 150 */
        buttons_scan_pure(s, DFU_MID, VOL_IDLE, ev, 4);
    }
    /* Now a genuine Track4 press still commits normally. */
    buttons_scan_pure(s, TRK_T4, VOL_IDLE, ev, 4);
    buttons_scan_pure(s, TRK_T4, VOL_IDLE, ev, 4);
    int n = buttons_scan_pure(s, TRK_T4, VOL_IDLE, ev, 4);
    CHECK(n == 1);
    CHECK(ev[0].idx == 4 && ev[0].pressed == 1);
}

int main(void)
{
    test_decode_tracks_boundaries();
    test_decode_vol_boundaries();
    test_dfu_band();
    test_press_after_three_reads();
    test_single_glitch_emits_nothing();
    test_release_emits_one_up();
    test_change_emits_release_then_press();
    test_both_ladders_independent();
    test_dfu_band_suppresses_track_edges();
    test_dfu_band_short_then_real_press();
    if (g_fail) {
        printf("buttons tests FAILED\n");
        return 1;
    }
    printf("all buttons tests passed\n");
    return 0;
}
