#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;
    uint8_t step_index;
} walkie_adpcm_state_t;

/* Runtime state for mic cleanup: noise estimate, optional gain history, and
 * speech hangover used to avoid chopping quiet syllables. */
typedef struct {
    int32_t hp_prev_x;
    int32_t hp_prev_y;
    int32_t noise_floor;
    int32_t agc_gain_q10;
    int speech_hangover;
} walkie_mic_proc_state_t;

void walkie_adpcm_reset(walkie_adpcm_state_t *state);
void walkie_mic_proc_reset(walkie_mic_proc_state_t *state);

/* Compress one PCM frame into packed IMA ADPCM bytes. */
void walkie_adpcm_encode_frame(const int16_t *pcm,
                               int samples,
                               walkie_adpcm_state_t *state,
                               uint8_t *out_bytes);

/* Expand packed IMA ADPCM bytes back into one PCM frame. */
void walkie_adpcm_decode_frame(const uint8_t *in_bytes,
                               int samples,
                               walkie_adpcm_state_t *state,
                               int16_t *pcm_out);

/* Convert raw I2S mic words into cleaned/gained PCM ready for compression. */
void walkie_process_mic_frame(const int32_t *raw_i2s,
                              int16_t *pcm_out,
                              int samples,
                              walkie_mic_proc_state_t *state,
                              int manual_mode,
                              int gain_q10);

/* Apply fixed-point speaker gain in-place. */
void walkie_apply_playback_gain(int16_t *pcm, int samples, int gain_q12);

/* Create a fading substitute frame when one or two radio packets are missing. */
void walkie_generate_plc_frame(const int16_t *last_frame,
                               int16_t *out_frame,
                               int samples,
                               int loss_count);

#ifdef __cplusplus
}
#endif
