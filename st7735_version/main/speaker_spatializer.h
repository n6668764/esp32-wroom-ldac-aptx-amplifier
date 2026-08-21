#pragma once

#include <stddef.h>
#include <stdint.h>

// Widens a two-channel loudspeaker image at 48 or 96 kHz. Other sample rates
// pass through unchanged and reset the filter history.
void speaker_spatializer_process(int32_t *stereo, size_t frames, uint32_t sample_rate,
                                 uint8_t absolute_volume);

// Reverb amount: 0=off, 1=light, 2=normal, 3=strong.
void speaker_spatializer_set_reverb_level(uint8_t level);
uint8_t speaker_spatializer_get_reverb_level(void);
