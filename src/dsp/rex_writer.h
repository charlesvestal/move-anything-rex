/*
 * REX2 File Writer
 *
 * Writes REX2 IFF container files with DWOP-compressed audio.
 * Produces files readable by the REX parser in rex_parser.c.
 *
 * License: MIT
 */

#ifndef REX_WRITER_H
#define REX_WRITER_H

#include <stdint.h>
#include <stddef.h>

/* Slice descriptor for writing */
typedef struct {
    uint32_t sample_offset;  /* offset in samples from start */
    uint32_t sample_length;  /* length in samples */
} rex_write_slice_t;

/* Parameters for writing a REX2 file */
typedef struct {
    float tempo_bpm;         /* tempo in BPM */
    int bars;                /* number of bars */
    int beats;               /* remaining beats past whole bars */
    int time_sig_num;        /* time signature numerator (e.g. 4) */
    int time_sig_den;        /* time signature denominator (e.g. 4) */

    int sample_rate;         /* e.g. 44100 */
    int channels;            /* 1=mono, 2=stereo */

    const int16_t *pcm_data; /* PCM audio (interleaved if stereo) */
    int num_frames;          /* per-channel frame count */

    int slice_count;
    const rex_write_slice_t *slices;
} rex_write_params_t;

/* Write a REX2 file to an output buffer.
 * buf/buf_cap: output buffer (caller-allocated).
 * Returns total bytes written, or 0 on error.
 * Recommended buffer size: num_frames * channels * 3 + 1024 (generous). */
int rex_write(const rex_write_params_t *params, uint8_t *buf, int buf_cap);

#endif /* REX_WRITER_H */
