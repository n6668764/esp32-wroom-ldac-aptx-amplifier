#include "speaker_spatializer.h"

#include <limits.h>
#include <string.h>

namespace {

constexpr uint32_t SAMPLE_RATE_48K = 48000;
constexpr uint32_t SAMPLE_RATE_96K = 96000;
constexpr size_t DELAY_SAMPLES = 64;

// The side low-pass corner is approximately 180 Hz at 48 kHz. Bass width is
// kept moderate, while the side information above it receives extra gain.
constexpr int32_t SIDE_LOWPASS_ALPHA_48K_Q15 = 764;
constexpr int32_t SIDE_LOWPASS_ALPHA_96K_Q15 = 382;
constexpr int32_t BASS_LOWPASS_ALPHA_48K_Q15 = 512;
constexpr int32_t BASS_LOWPASS_ALPHA_96K_Q15 = 256;
constexpr int32_t MID_GAIN_Q15 = 19005;          // 0.58
constexpr int32_t SIDE_GAIN_Q15 = 22938;         // 0.70
constexpr int32_t HIGH_SIDE_GAIN_Q15 = 9175;     // +0.28
constexpr int32_t AMBIENCE_GAIN_Q15 = 5898;      // 0.18
constexpr int32_t CROSSTALK_CANCEL_Q15 = 5243;   // 0.16

// A DC-blocked square term adds mostly second harmonic at about -30 dB for a
// full-scale sine. Applying it to the mid channel keeps the stereo image stable.
constexpr int32_t EVEN_HARMONIC_GAIN_Q15 = 1966; // 0.06
constexpr int32_t HARMONIC_DC_ALPHA_48K_Q15 = 43; // approximately 10 Hz
constexpr int32_t HARMONIC_DC_ALPHA_96K_Q15 = 21;

constexpr size_t REVERB_A_MAX = 1117;
constexpr size_t REVERB_B_MAX = 1597;
constexpr int32_t REVERB_INPUT_Q15 = 11469;      // 0.35
constexpr int32_t REVERB_FEEDBACK_Q15 = 12452;   // 0.38
constexpr int32_t REVERB_DAMP_Q15 = 8192;        // 0.25
constexpr int32_t REVERB_CROSS_Q15 = 11469;      // 0.35
constexpr uint8_t REVERB_LEVEL_COUNT = 4;
constexpr int32_t REVERB_WET_LEVELS_Q15[REVERB_LEVEL_COUNT] = {
    0,      // off
    1638,   // light: 0.05
    3277,   // normal: 0.10
    5898,   // strong: 0.18
};

int32_t s_left_delay[DELAY_SAMPLES];
int32_t s_right_delay[DELAY_SAMPLES];
int32_t s_mid_delay[DELAY_SAMPLES];
int32_t s_side_lowpass;
int32_t s_bass_lowpass;
int32_t s_harmonic_dc;
int16_t s_reverb_a[REVERB_A_MAX];
int16_t s_reverb_b[REVERB_B_MAX];
int32_t s_reverb_damp_a;
int32_t s_reverb_damp_b;
size_t s_reverb_head_a;
size_t s_reverb_head_b;
size_t s_delay_head;
bool s_active;
uint32_t s_active_sample_rate;
int32_t s_applied_bass_boost_q15;
volatile uint8_t s_reverb_level = 2;
int32_t s_applied_reverb_wet_q15 = REVERB_WET_LEVELS_Q15[2];

int32_t saturate_s32(int64_t sample)
{
    if (sample > INT32_MAX) return INT32_MAX;
    if (sample < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(sample);
}

int16_t saturate_s16(int32_t sample)
{
    if (sample > INT16_MAX) return INT16_MAX;
    if (sample < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(sample);
}

int32_t scale_q15(int32_t sample, int32_t gain_q15)
{
    return static_cast<int32_t>((static_cast<int64_t>(sample) * gain_q15) >> 15);
}

void reset_state()
{
    memset(s_left_delay, 0, sizeof(s_left_delay));
    memset(s_right_delay, 0, sizeof(s_right_delay));
    memset(s_mid_delay, 0, sizeof(s_mid_delay));
    memset(s_reverb_a, 0, sizeof(s_reverb_a));
    memset(s_reverb_b, 0, sizeof(s_reverb_b));
    s_side_lowpass = 0;
    s_bass_lowpass = 0;
    s_harmonic_dc = 0;
    s_reverb_damp_a = 0;
    s_reverb_damp_b = 0;
    s_delay_head = 0;
    s_reverb_head_a = 0;
    s_reverb_head_b = 0;
}

int32_t bass_boost_for_volume(uint8_t volume)
{
    // Shelf gain relative to the 0.58 mid gain: about +4 dB at low volume,
    // +2.5 dB around half volume, +1 dB around 75%, then no boost at full.
    struct Point {
        uint8_t volume;
        int32_t boost_q15;
    };
    constexpr Point points[] = {{0, 11110}, {32, 11110}, {64, 6324},
                                {96, 2327}, {127, 0}};
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
        if (volume <= points[i].volume) {
            const Point &a = points[i - 1];
            const Point &b = points[i];
            return a.boost_q15 +
                static_cast<int32_t>((static_cast<int64_t>(b.boost_q15 - a.boost_q15) *
                                      (volume - a.volume)) /
                                     (b.volume - a.volume));
        }
    }
    return 0;
}

}  // namespace

void speaker_spatializer_process(int32_t *stereo, size_t frames, uint32_t sample_rate,
                                 uint8_t absolute_volume)
{
    if (stereo == nullptr || frames == 0) return;

    if (sample_rate != SAMPLE_RATE_48K && sample_rate != SAMPLE_RATE_96K) {
        if (s_active) reset_state();
        s_active = false;
        s_active_sample_rate = 0;
        return;
    }
    if (!s_active || s_active_sample_rate != sample_rate) {
        reset_state();
        s_active = true;
        s_active_sample_rate = sample_rate;
        s_applied_bass_boost_q15 = bass_boost_for_volume(absolute_volume);
    }

    const bool high_rate = sample_rate == SAMPLE_RATE_96K;
    const size_t crosstalk_delay = high_rate ? 20 : 10;
    const size_t ambience_delay_a = high_rate ? 26 : 13;
    const size_t ambience_delay_b = high_rate ? 58 : 29;
    const int32_t side_lowpass_alpha_q15 = high_rate ?
        SIDE_LOWPASS_ALPHA_96K_Q15 : SIDE_LOWPASS_ALPHA_48K_Q15;
    const int32_t bass_lowpass_alpha_q15 = high_rate ?
        BASS_LOWPASS_ALPHA_96K_Q15 : BASS_LOWPASS_ALPHA_48K_Q15;
    const int32_t harmonic_dc_alpha_q15 = high_rate ?
        HARMONIC_DC_ALPHA_96K_Q15 : HARMONIC_DC_ALPHA_48K_Q15;
    const size_t reverb_length_a = high_rate ? REVERB_A_MAX : 557;
    const size_t reverb_length_b = high_rate ? REVERB_B_MAX : 797;
    const int32_t target_bass_boost_q15 = bass_boost_for_volume(absolute_volume);
    const uint8_t reverb_level = s_reverb_level < REVERB_LEVEL_COUNT ? s_reverb_level : 2;
    const int32_t target_reverb_wet_q15 = REVERB_WET_LEVELS_Q15[reverb_level];
    int64_t bass_boost_q31 = static_cast<int64_t>(s_applied_bass_boost_q15) << 16;
    const int64_t bass_boost_step_q31 =
        ((static_cast<int64_t>(target_bass_boost_q15) - s_applied_bass_boost_q15) << 16) /
        static_cast<int64_t>(frames);
    int64_t reverb_wet_q31 = static_cast<int64_t>(s_applied_reverb_wet_q15) << 16;
    const int64_t reverb_wet_step_q31 =
        ((static_cast<int64_t>(target_reverb_wet_q15) - s_applied_reverb_wet_q15) << 16) /
        static_cast<int64_t>(frames);

    for (size_t frame = 0; frame < frames; ++frame) {
        bass_boost_q31 += bass_boost_step_q31;
        reverb_wet_q31 += reverb_wet_step_q31;
        const int32_t bass_boost_q15 = static_cast<int32_t>(bass_boost_q31 >> 16);
        const int32_t reverb_wet_q15 = static_cast<int32_t>(reverb_wet_q31 >> 16);
        const int32_t left = stereo[2 * frame];
        const int32_t right = stereo[2 * frame + 1];
        const int32_t mid = static_cast<int32_t>((static_cast<int64_t>(left) + right) / 2);
        const int32_t side = static_cast<int32_t>((static_cast<int64_t>(left) - right) / 2);
        s_mid_delay[s_delay_head] = mid;

        const int64_t bass_delta = static_cast<int64_t>(mid) - s_bass_lowpass;
        s_bass_lowpass = saturate_s32(static_cast<int64_t>(s_bass_lowpass) +
                                      ((bass_delta * bass_lowpass_alpha_q15) >> 15));

        const int64_t side_delta = static_cast<int64_t>(side) - s_side_lowpass;
        s_side_lowpass = saturate_s32(static_cast<int64_t>(s_side_lowpass) +
                                      ((side_delta * side_lowpass_alpha_q15) >> 15));
        const int32_t high_side = saturate_s32(static_cast<int64_t>(side) - s_side_lowpass);
        const size_t ambience_a =
            (s_delay_head + DELAY_SAMPLES - ambience_delay_a) % DELAY_SAMPLES;
        const size_t ambience_b =
            (s_delay_head + DELAY_SAMPLES - ambience_delay_b) % DELAY_SAMPLES;
        const int32_t ambience = saturate_s32(
            ((static_cast<int64_t>(s_mid_delay[ambience_a]) -
              s_mid_delay[ambience_b]) * AMBIENCE_GAIN_Q15) >> 15);
        const int32_t spatial_side = saturate_s32(
            static_cast<int64_t>(scale_q15(side, SIDE_GAIN_Q15)) +
            scale_q15(high_side, HIGH_SIDE_GAIN_Q15) + ambience);
        const int32_t square_mid = saturate_s32(
            (static_cast<int64_t>(mid) * mid) >> 31);
        s_harmonic_dc = saturate_s32(
            static_cast<int64_t>(s_harmonic_dc) +
            scale_q15(square_mid - s_harmonic_dc, harmonic_dc_alpha_q15));
        const int32_t even_harmonic = scale_q15(
            square_mid - s_harmonic_dc, EVEN_HARMONIC_GAIN_Q15);
        const int32_t spatial_mid = saturate_s32(
            static_cast<int64_t>(scale_q15(mid, MID_GAIN_Q15)) +
            scale_q15(s_bass_lowpass, bass_boost_q15) + even_harmonic);

        const int32_t pre_left = saturate_s32(static_cast<int64_t>(spatial_mid) + spatial_side);
        const int32_t pre_right = saturate_s32(static_cast<int64_t>(spatial_mid) - spatial_side);
        s_left_delay[s_delay_head] = pre_left;
        s_right_delay[s_delay_head] = pre_right;

        const size_t delayed =
            (s_delay_head + DELAY_SAMPLES - crosstalk_delay) % DELAY_SAMPLES;
        int32_t output_left = saturate_s32(
            static_cast<int64_t>(pre_left) -
            scale_q15(s_right_delay[delayed], CROSSTALK_CANCEL_Q15));
        int32_t output_right = saturate_s32(
            static_cast<int64_t>(pre_right) -
            scale_q15(s_left_delay[delayed], CROSSTALK_CANCEL_Q15));

        const int32_t reverb_a = s_reverb_a[s_reverb_head_a];
        const int32_t reverb_b = s_reverb_b[s_reverb_head_b];
        s_reverb_damp_a += scale_q15(reverb_a - s_reverb_damp_a, REVERB_DAMP_Q15);
        s_reverb_damp_b += scale_q15(reverb_b - s_reverb_damp_b, REVERB_DAMP_Q15);
        const int32_t reverb_input_left = pre_left / 65536;
        const int32_t reverb_input_right = pre_right / 65536;
        s_reverb_a[s_reverb_head_a] = saturate_s16(
            scale_q15(reverb_input_left, REVERB_INPUT_Q15) +
            scale_q15(s_reverb_damp_b, REVERB_FEEDBACK_Q15));
        s_reverb_b[s_reverb_head_b] = saturate_s16(
            scale_q15(reverb_input_right, REVERB_INPUT_Q15) +
            scale_q15(s_reverb_damp_a, REVERB_FEEDBACK_Q15));

        const int32_t wet_a = reverb_a * 65536;
        const int32_t wet_b = reverb_b * 65536;
        const int32_t wet_left = saturate_s32(
            static_cast<int64_t>(wet_a) + scale_q15(wet_b, REVERB_CROSS_Q15));
        const int32_t wet_right = saturate_s32(
            static_cast<int64_t>(wet_b) + scale_q15(wet_a, REVERB_CROSS_Q15));
        output_left = saturate_s32(static_cast<int64_t>(output_left) +
                                   scale_q15(wet_left, reverb_wet_q15));
        output_right = saturate_s32(static_cast<int64_t>(output_right) +
                                    scale_q15(wet_right, reverb_wet_q15));
        stereo[2 * frame] = output_left;
        stereo[2 * frame + 1] = output_right;

        if (++s_reverb_head_a >= reverb_length_a) s_reverb_head_a = 0;
        if (++s_reverb_head_b >= reverb_length_b) s_reverb_head_b = 0;
        s_delay_head = (s_delay_head + 1) % DELAY_SAMPLES;
    }
    s_applied_bass_boost_q15 = target_bass_boost_q15;
    s_applied_reverb_wet_q15 = target_reverb_wet_q15;
}

void speaker_spatializer_set_reverb_level(uint8_t level)
{
    s_reverb_level = level < REVERB_LEVEL_COUNT ? level : REVERB_LEVEL_COUNT - 1;
}

uint8_t speaker_spatializer_get_reverb_level(void)
{
    return s_reverb_level;
}
