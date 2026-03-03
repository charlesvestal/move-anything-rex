/*
 * Minimal WAV Reader
 *
 * Reads PCM WAV files (format tag 1). Accepts 16, 24, or 32-bit input.
 * Always outputs 16-bit PCM (higher depths are right-shifted to 16-bit).
 * Extracts sample rate, channels, and converted PCM data.
 *
 * License: MIT
 */

#ifndef WAV_READER_H
#define WAV_READER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int sample_rate;
    int channels;
    int bits_per_sample;
    int16_t *pcm_data;       /* allocated, caller must free */
    int num_frames;          /* per-channel frame count */
    char error[256];
} wav_file_t;

/* Read a WAV file from an in-memory buffer.
 * Returns 0 on success, -1 on error (check wav->error).
 * Caller must call wav_free() when done. */
int wav_read(wav_file_t *wav, const uint8_t *data, size_t data_len);

/* Free resources allocated by wav_read */
void wav_free(wav_file_t *wav);

#endif /* WAV_READER_H */
