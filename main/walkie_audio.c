#include "walkie_audio.h"

#include <stdlib.h>
#include <string.h>

/*
 * Audio pipeline helpers.
 *
 * The mic task captures 16 kHz PCM, this module applies the walkie-specific mic
 * gain/noise handling, then encodes each 20 ms frame as IMA ADPCM. ADPCM keeps
 * the voice stream near 64 kbps so it can survive over ESP-NOW long-range rates.
 */

static const int s_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int s_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/**
 * Clamp a 32-bit intermediate value to signed 16-bit PCM range.
 *
 * Most audio math is done in 32-bit integers to avoid overflow while applying
 * gain or ADPCM deltas. This helper safely returns to the I2S/playback format.
 */
static inline int16_t walkie_clip16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (int16_t)value;
}

/**
 * Reset an IMA ADPCM encoder/decoder state.
 *
 * The predictor and step index are the only state IMA needs. They are also sent
 * in each audio packet so the receiver can recover quickly after packet loss.
 */
void walkie_adpcm_reset(walkie_adpcm_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->predictor = 0;
    state->step_index = 0;
}

/**
 * Reset microphone post-processing state.
 *
 * This is called when PTT starts/stops so noise estimates and speech hangover do
 * not leak between separate transmissions.
 */
void walkie_mic_proc_reset(walkie_mic_proc_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->agc_gain_q10 = 1024;
    state->noise_floor = 512;
}

/**
 * Convert one raw 32-bit I2S microphone word to 16-bit PCM.
 *
 * The INMP-style mic used here provides the useful signed sample in the top byte
 * for this wiring/timing setup, matching the known-good MicroPython behavior.
 */
static inline int16_t walkie_extract_mic_sample(const int32_t raw_i2s)
{
    const uint8_t *bytes = (const uint8_t *)&raw_i2s;

    /* Match the known-good MicroPython capture path: use the signed top byte
     * from each 32-bit I2S word and expand it to 16-bit PCM. */
    return (int16_t)(((int8_t)bytes[3]) << 8);
}

/**
 * Encode one 16-bit PCM sample into a 4-bit IMA ADPCM nibble.
 *
 * IMA stores the direction and rough magnitude of each sample delta. The updated
 * predictor/step index are kept in state for the next sample.
 */
static uint8_t walkie_ima_encode_nibble(int16_t sample, walkie_adpcm_state_t *state)
{
    int predictor = state->predictor;
    int step = s_ima_step_table[state->step_index];
    int diff = sample - predictor;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 0x8;
        diff = -diff;
    }

    int delta = step >> 3;
    if (diff >= step) {
        nibble |= 0x4;
        diff -= step;
        delta += step;
    }
    if (diff >= (step >> 1)) {
        nibble |= 0x2;
        diff -= (step >> 1);
        delta += (step >> 1);
    }
    if (diff >= (step >> 2)) {
        nibble |= 0x1;
        delta += (step >> 2);
    }

    if (nibble & 0x8) {
        predictor -= delta;
    } else {
        predictor += delta;
    }

    if (predictor > 32767) {
        predictor = 32767;
    } else if (predictor < -32768) {
        predictor = -32768;
    }

    int index = state->step_index + s_ima_index_table[nibble & 0x0F];
    if (index < 0) {
        index = 0;
    } else if (index > 88) {
        index = 88;
    }

    state->predictor = (int16_t)predictor;
    state->step_index = (uint8_t)index;
    return nibble & 0x0F;
}

/**
 * Decode one 4-bit IMA ADPCM nibble back into a 16-bit PCM estimate.
 *
 * This mirrors walkie_ima_encode_nibble(). The result is not bit-perfect audio;
 * it is a speech-optimized approximation that saves 75% of the radio payload.
 */
static int16_t walkie_ima_decode_nibble(uint8_t nibble, walkie_adpcm_state_t *state)
{
    int predictor = state->predictor;
    int step = s_ima_step_table[state->step_index];
    int delta = step >> 3;

    if (nibble & 0x4) {
        delta += step;
    }
    if (nibble & 0x2) {
        delta += step >> 1;
    }
    if (nibble & 0x1) {
        delta += step >> 2;
    }

    if (nibble & 0x8) {
        predictor -= delta;
    } else {
        predictor += delta;
    }

    if (predictor > 32767) {
        predictor = 32767;
    } else if (predictor < -32768) {
        predictor = -32768;
    }

    int index = state->step_index + s_ima_index_table[nibble & 0x0F];
    if (index < 0) {
        index = 0;
    } else if (index > 88) {
        index = 88;
    }

    state->predictor = (int16_t)predictor;
    state->step_index = (uint8_t)index;
    return (int16_t)predictor;
}

/**
 * Encode a PCM frame into packed IMA ADPCM bytes.
 *
 * Two 4-bit nibbles are packed into each byte. With 320 samples per 20 ms frame,
 * the output is 160 bytes, small enough for one ESP-NOW packet.
 */
void walkie_adpcm_encode_frame(const int16_t *pcm,
                               int samples,
                               walkie_adpcm_state_t *state,
                               uint8_t *out_bytes)
{
    for (int i = 0, out = 0; i < samples; i += 2, ++out) {
        uint8_t lo = walkie_ima_encode_nibble(pcm[i], state);
        uint8_t hi = 0;
        if (i + 1 < samples) {
            hi = walkie_ima_encode_nibble(pcm[i + 1], state);
        }
        out_bytes[out] = (uint8_t)(lo | (hi << 4));
    }
}

/**
 * Decode a packed IMA ADPCM frame back to PCM.
 *
 * The receiver reconstructs 320 samples from 160 bytes, then pushes the frame
 * into the jitter buffer for timed speaker playback.
 */
void walkie_adpcm_decode_frame(const uint8_t *in_bytes,
                               int samples,
                               walkie_adpcm_state_t *state,
                               int16_t *pcm_out)
{
    for (int i = 0, in = 0; i < samples; i += 2, ++in) {
        uint8_t packed = in_bytes[in];
        pcm_out[i] = walkie_ima_decode_nibble(packed & 0x0F, state);
        if (i + 1 < samples) {
            pcm_out[i + 1] = walkie_ima_decode_nibble((packed >> 4) & 0x0F, state);
        }
    }
}

/**
 * Convert and condition one captured mic frame before radio encoding.
 *
 * This applies board-calibrated gain, a soft limiter, adaptive noise estimation,
 * and gentle quiet-frame attenuation. It avoids hard-gating frames so quiet
 * syllables are not chopped out of speech.
 */
void walkie_process_mic_frame(const int32_t *raw_i2s,
                              int16_t *pcm_out,
                              int samples,
                              walkie_mic_proc_state_t *state,
                              int manual_mode,
                              int gain_q10)
{
    int64_t sum_abs = 0;
    int peak = 1;

    if (gain_q10 <= 0) {
        gain_q10 = 1024;
    }

    for (int i = 0; i < samples; ++i) {
        int32_t sample = walkie_extract_mic_sample(raw_i2s[i]);
        sample = (sample * gain_q10) >> 10;
        if (sample > 28672) {
            sample = 28672 + ((sample - 28672) / 4);
        } else if (sample < -28672) {
            sample = -28672 + ((sample + 28672) / 4);
        }

        int16_t pcm = walkie_clip16(sample);
        pcm_out[i] = pcm;

        int abs_value = abs((int)pcm);
        sum_abs += abs_value;
        if (abs_value > peak) {
            peak = abs_value;
        }
    }

    int avg_abs = (int)(sum_abs / (samples > 0 ? samples : 1));
    if (avg_abs < (state->noise_floor * 2)) {
        state->noise_floor = (state->noise_floor * 15 + avg_abs) / 16;
    } else {
        state->noise_floor = (state->noise_floor * 31 + avg_abs) / 32;
    }
    if (state->noise_floor < 384) {
        state->noise_floor = 384;
    }

    int gate = state->noise_floor + 384;
    int quiet_atten_q10 = 512;

    if (manual_mode > 0) {
        gate -= 256;
        quiet_atten_q10 = 768;
    } else if (manual_mode < 0) {
        gate += 256;
        quiet_atten_q10 = 384;
    }

    if (gate < 384) {
        gate = 384;
    }

    bool speech = (avg_abs > gate) || (peak > gate * 2);

    if (speech) {
        state->speech_hangover = manual_mode > 0 ? 12 : 8;
    } else if (state->speech_hangover > 0) {
        speech = true;
        state->speech_hangover--;
    }

    for (int i = 0; i < samples; ++i) {
        int32_t sample = pcm_out[i];
        if (!speech) {
            sample = (sample * quiet_atten_q10) >> 10;
        }
        pcm_out[i] = walkie_clip16(sample);
    }
}

/**
 * Apply the current speaker volume/boost gain to one playback frame.
 *
 * gain_q12 is fixed-point, where 4096 means 1.0x. Using fixed-point keeps the
 * audio path deterministic and cheaper than float math on the ESP32.
 */
void walkie_apply_playback_gain(int16_t *pcm, int samples, int gain_q12)
{
    for (int i = 0; i < samples; ++i) {
        int32_t sample = ((int32_t)pcm[i] * gain_q12) >> 12;
        pcm[i] = walkie_clip16(sample);
    }
}

/**
 * Generate a short packet-loss concealment frame from the last good audio.
 *
 * Instead of dropping immediately to silence on a missing packet, playback_task()
 * fades the last good frame. That makes one- or two-packet ESP-NOW hiccups much
 * less noticeable.
 */
void walkie_generate_plc_frame(const int16_t *last_frame,
                               int16_t *out_frame,
                               int samples,
                               int loss_count)
{
    int attenuation_q12 = 4096;

    if (loss_count > 0) {
        attenuation_q12 = 4096 - (loss_count * 768);
        if (attenuation_q12 < 768) {
            attenuation_q12 = 768;
        }
    }

    for (int i = 0; i < samples; ++i) {
        int32_t sample = ((int32_t)last_frame[i] * attenuation_q12) >> 12;
        out_frame[i] = walkie_clip16(sample);
    }
}
