#ifndef MIDI_DELAY_RUNTIME_H
#define MIDI_DELAY_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

/* Zephyr integration for the bounded expressive MIDI delay. All musical state
 * lives on one worker thread; USB and TIMER2 callbacks only enqueue events. */
int midi_delay_runtime_init(void);
bool midi_delay_runtime_running(void);
uint8_t midi_delay_runtime_length_index(void);
uint8_t midi_delay_runtime_style(void);
bool midi_delay_runtime_set_running(bool running);
bool midi_delay_runtime_toggle(void);
bool midi_delay_runtime_step_length(int direction);
bool midi_delay_runtime_step_style(int direction);
bool midi_delay_runtime_set_repeats(uint8_t repeats);
bool midi_delay_runtime_set_velocity_decay(uint8_t decay);
bool midi_delay_runtime_set_pitch_step(int8_t semitones);
bool midi_delay_runtime_set_channels(uint8_t input_channel,
                                     uint8_t output_channel);

#endif /* MIDI_DELAY_RUNTIME_H */
