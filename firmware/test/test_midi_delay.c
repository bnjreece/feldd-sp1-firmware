#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../app/src/midi_delay.h"

static struct midi_msg note_on(uint8_t ch, uint8_t note, uint8_t vel)
{
    struct midi_msg m = { (uint8_t)(0x90u | ch), note, vel, 3 };
    return m;
}

static struct midi_msg note_off(uint8_t ch, uint8_t note, uint8_t vel)
{
    struct midi_msg m = { (uint8_t)(0x80u | ch), note, vel, 3 };
    return m;
}

static struct midi_msg cc(uint8_t ch, uint8_t num, uint8_t value)
{
    struct midi_msg m = { (uint8_t)(0xB0u | ch), num, value, 3 };
    return m;
}

static struct midi_msg voice3(uint8_t kind, uint8_t ch,
                              uint8_t d1, uint8_t d2)
{
    struct midi_msg m = { (uint8_t)(kind | ch), d1, d2, 3 };
    return m;
}

static struct midi_msg voice2(uint8_t kind, uint8_t ch, uint8_t d1)
{
    struct midi_msg m = { (uint8_t)(kind | ch), d1, 0, 2 };
    return m;
}

static void test_exact_delay_and_channel_remap(void)
{
    struct midi_delay d;
    struct midi_msg out[8];
    struct midi_msg on = note_on(0, 60, 91);
    struct midi_msg off = note_off(0, 60, 47);

    midi_delay_init(&d, 48, 0, 1); /* one eighth, ch1 -> ch2 */
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 100, &on) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 124, &off) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 147, out, 8) == 0);
    assert(midi_delay_advance(&d, 148, out, 8) == 1);
    assert(out[0].status == 0x91 && out[0].d1 == 60 && out[0].d2 == 91);
    assert(midi_delay_advance(&d, 171, out, 8) == 0);
    assert(midi_delay_advance(&d, 172, out, 8) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 60 && out[0].d2 == 47);
    assert(midi_delay_pending(&d) == 0);
}

static void test_rhythmic_families(void)
{
    assert(midi_delay_styled_ticks(48, MIDI_DELAY_STYLE_STRAIGHT) == 48);
    assert(midi_delay_styled_ticks(48, MIDI_DELAY_STYLE_DOTTED) == 72);
    assert(midi_delay_styled_ticks(48, MIDI_DELAY_STYLE_TRIPLET) == 32);
    assert(midi_delay_styled_ticks(3072, MIDI_DELAY_STYLE_DOTTED) == 4608);
    assert(midi_delay_styled_ticks(48, 99) == 0);
    assert(midi_delay_styled_ticks(UINT16_MAX,
                                   MIDI_DELAY_STYLE_DOTTED) == 0);
}

static void test_stopped_and_unsupported_are_silent(void)
{
    struct midi_delay d;
    struct midi_msg on = note_on(0, 60, 100);
    struct midi_msg wrong_ch = note_on(2, 60, 100);
    struct midi_msg program = voice2(0xC0, 0, 12);
    struct midi_msg channel_mode = cc(0, 123, 0);

    midi_delay_init(&d, 24, 0, 1);
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_IGNORED);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &wrong_ch) == MIDI_DELAY_IGNORED);
    assert(midi_delay_input(&d, 0, &program) == MIDI_DELAY_IGNORED);
    assert(midi_delay_input(&d, 0, &channel_mode) == MIDI_DELAY_IGNORED);
    assert(midi_delay_pending(&d) == 0);
}

static void test_expressive_messages_are_delayed_and_remapped(void)
{
    struct midi_delay d;
    struct midi_msg out[8];
    struct midi_msg poly = voice3(0xA0, 0, 60, 71);
    struct midi_msg mod = cc(0, 1, 99);
    struct midi_msg pressure = voice2(0xD0, 0, 83);
    struct midi_msg bend = voice3(0xE0, 0, 17, 92);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &poly) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 0, &mod) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 0, &pressure) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 0, &bend) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 24, out, 8) == 4);
    assert(out[0].status == 0xA1 && out[0].d1 == 60 && out[0].d2 == 71);
    assert(out[1].status == 0xB1 && out[1].d1 == 1 && out[1].d2 == 99);
    assert(out[2].status == 0xD1 && out[2].d1 == 83 && out[2].len == 2);
    assert(out[3].status == 0xE1 && out[3].d1 == 17 && out[3].d2 == 92);
}

static void test_omni_rejects_output_channel_feedback(void)
{
    struct midi_delay d;
    struct midi_msg a = note_on(0, 60, 100);
    struct midi_msg b = note_on(7, 61, 100);
    struct midi_msg echo = note_on(1, 62, 100);

    midi_delay_init(&d, 24, MIDI_DELAY_CHANNEL_OMNI, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &a) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 0, &b) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 0, &echo) == MIDI_DELAY_IGNORED);
    assert(midi_delay_pending(&d) == 2);
}

static void test_repeat_decay_and_pitch_drift(void)
{
    struct midi_delay d;
    struct midi_msg out[4];
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    struct midi_msg on = note_on(0, 60, 100);
    struct midi_msg off = note_off(0, 60, 0);

    midi_delay_init(&d, 24, 0, 1);
    assert(midi_delay_set_character(&d, 3, 64, 7,
                                    release, MIDI_DELAY_RELEASE_CAP) == 0);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 4, &off) == MIDI_DELAY_OK);

    assert(midi_delay_advance(&d, 24, out, 4) == 1);
    assert(out[0].status == 0x91 && out[0].d1 == 60 && out[0].d2 == 100);
    assert(midi_delay_advance(&d, 28, out, 4) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 60);
    assert(midi_delay_advance(&d, 48, out, 4) == 1);
    assert(out[0].d1 == 67 && out[0].d2 == 50);
    assert(midi_delay_advance(&d, 52, out, 4) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 67);
    assert(midi_delay_advance(&d, 72, out, 4) == 1);
    assert(out[0].d1 == 74 && out[0].d2 == 25);
    assert(midi_delay_advance(&d, 76, out, 4) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 74);
    assert(midi_delay_pending(&d) == 0);
}

static void test_character_change_starts_safe_epoch(void)
{
    struct midi_delay d;
    struct midi_msg out[8];
    struct midi_msg on = note_on(0, 60, 100);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 24, out, 8) == 1);
    assert(midi_delay_set_character(&d, 4, 96, -5, out, 8) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 60);
    assert(midi_delay_pending(&d) == 0);
    assert(midi_delay_running(&d));
}

static void test_overlapping_repeats_aggregate_notes_and_pedal(void)
{
    struct midi_delay d;
    struct midi_msg out[8];
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    struct midi_msg on = note_on(0, 60, 100);
    struct midi_msg off = note_off(0, 60, 0);
    struct midi_msg pedal_down = cc(0, 64, 100);
    struct midi_msg pedal_up = cc(0, 64, 0);

    midi_delay_init(&d, 24, 0, 1);
    assert(midi_delay_set_character(&d, 2, 127, 0,
                                    release, MIDI_DELAY_RELEASE_CAP) == 0);
    midi_delay_start(&d);

    /* A long held note overlaps its second identical repeat. The first
     * generation's note-off must not cut the second generation. */
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 30, &off) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 24, out, 8) == 1);
    assert(midi_delay_advance(&d, 48, out, 8) == 1);
    assert(midi_delay_advance(&d, 54, out, 8) == 0);
    assert(midi_delay_advance(&d, 78, out, 8) == 1);
    assert(out[0].status == 0x81 && out[0].d1 == 60);

    assert(midi_delay_input(&d, 100, &pedal_down) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 130, &pedal_up) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 124, out, 8) == 1);
    assert(out[0].status == 0xB1 && out[0].d2 == 100);
    /* Generation 2 holds while generation 1 rises: no aggregate change. */
    assert(midi_delay_advance(&d, 148, out, 8) == 0);
    assert(midi_delay_advance(&d, 154, out, 8) == 0);
    assert(midi_delay_advance(&d, 178, out, 8) == 1);
    assert(out[0].status == 0xB1 && out[0].d2 == 0);
}

static void test_velocity_zero_normalizes_to_note_off(void)
{
    struct midi_delay d;
    struct midi_msg out[2];
    struct midi_msg zero = note_on(0, 64, 0);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 10, &zero) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 34, out, 2) == 1);
    assert(out[0].status == 0x81);
    assert(out[0].d1 == 64 && out[0].d2 == 0 && out[0].len == 3);
}

static void test_three_poetry_pedals_and_half_values(void)
{
    struct midi_delay d;
    struct midi_msg out[4];
    struct midi_msg damper = cc(0, 64, 73);
    struct midi_msg sostenuto = cc(0, 66, 127);
    struct midi_msg soft = cc(0, 67, 42);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 1, &damper) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 1, &sostenuto) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 1, &soft) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 25, out, 4) == 3);
    assert(out[0].status == 0xB1 && out[0].d1 == 64 && out[0].d2 == 73);
    assert(out[1].d1 == 66 && out[1].d2 == 127);
    assert(out[2].d1 == 67 && out[2].d2 == 42);
}

static void test_stop_cancels_pending_and_releases_active(void)
{
    struct midi_delay d;
    struct midi_msg due[4];
    struct midi_msg releases[MIDI_DELAY_RELEASE_CAP];
    struct midi_msg on = note_on(0, 60, 100);
    struct midi_msg off = note_off(0, 60, 64);
    struct midi_msg damper = cc(0, 64, 96);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 1, &damper) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, 30, &off) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 25, due, 4) == 2);
    assert(midi_delay_pending(&d) == 1);

    int n = midi_delay_stop(&d, releases, MIDI_DELAY_RELEASE_CAP);
    assert(n == 2);
    assert(releases[0].status == 0x81 && releases[0].d1 == 60);
    assert(releases[1].status == 0xB1 && releases[1].d1 == 64 &&
           releases[1].d2 == 0);
    assert(midi_delay_pending(&d) == 0);
    assert(!midi_delay_running(&d));

    midi_delay_start(&d);
    assert(midi_delay_advance(&d, 100, due, 4) == 0);
}

static void test_length_change_cancels_old_epoch(void)
{
    struct midi_delay d;
    struct midi_msg out[8];
    struct midi_msg on = note_on(0, 60, 100);
    struct midi_msg off = note_off(0, 60, 64);

    midi_delay_init(&d, 48, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, 0, &on) == MIDI_DELAY_OK);
    assert(midi_delay_set_length(&d, 24, out, 8) == 0);
    assert(midi_delay_pending(&d) == 0);
    assert(midi_delay_input(&d, 10, &off) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 34, out, 8) == 1);
    assert(out[0].status == 0x81);
}

static void test_due_drain_respects_cap_and_wrap(void)
{
    struct midi_delay d;
    struct midi_msg out[2];
    struct midi_msg a = note_on(0, 60, 1);
    struct midi_msg b = note_on(0, 61, 2);
    struct midi_msg c = note_on(0, 62, 3);
    uint32_t start = UINT32_MAX - 10u;

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    assert(midi_delay_input(&d, start, &a) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, start, &b) == MIDI_DELAY_OK);
    assert(midi_delay_input(&d, start, &c) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&d, 13, out, 2) == 2);
    assert(out[0].d1 == 60 && out[1].d1 == 61);
    assert(midi_delay_advance(&d, 13, out, 2) == 1);
    assert(out[0].d1 == 62);
}

static void test_queue_full_is_atomic(void)
{
    struct midi_delay d;
    struct midi_msg on = note_on(0, 60, 100);

    midi_delay_init(&d, 24, 0, 1);
    midi_delay_start(&d);
    for (unsigned i = 0; i < MIDI_DELAY_QUEUE_CAP; i++) {
        assert(midi_delay_input(&d, i, &on) == MIDI_DELAY_OK);
    }
    assert(midi_delay_pending(&d) == MIDI_DELAY_QUEUE_CAP);
    assert(midi_delay_input(&d, 999, &on) == MIDI_DELAY_ERR_FULL);
    assert(midi_delay_pending(&d) == MIDI_DELAY_QUEUE_CAP);
}

int main(void)
{
    test_exact_delay_and_channel_remap();
    test_rhythmic_families();
    test_stopped_and_unsupported_are_silent();
    test_expressive_messages_are_delayed_and_remapped();
    test_omni_rejects_output_channel_feedback();
    test_repeat_decay_and_pitch_drift();
    test_character_change_starts_safe_epoch();
    test_overlapping_repeats_aggregate_notes_and_pedal();
    test_velocity_zero_normalizes_to_note_off();
    test_three_poetry_pedals_and_half_values();
    test_stop_cancels_pending_and_releases_active();
    test_length_change_cancels_old_epoch();
    test_due_drain_respects_cap_and_wrap();
    test_queue_full_is_atomic();
    puts("all midi_delay tests passed");
    return 0;
}
