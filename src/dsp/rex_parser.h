/*
 * REX2 File Parser
 *
 * Parses Propellerhead ReCycle .rx2/.rex files.
 * These use an IFF-style container format (big-endian) with
 * DWVW-12 compressed audio data.
 *
 * License: MIT
 */

#ifndef REX_PARSER_H
#define REX_PARSER_H

#include <stdint.h>
#include <stddef.h>

#define REX_MAX_SLICES 256

/* Slice descriptor */
typedef struct {
    uint32_t sample_offset;  /* offset in decoded samples from start of SDAT */
    uint32_t sample_length;  /* length in samples */
} rex_slice_t;

/* Parsed REX file */
typedef struct {
    /* Global info (from GLOB chunk) */
    float tempo_bpm;
    int bars;
    int beats;
    int time_sig_num;
    int time_sig_den;

    /* Audio format (from HEAD/RECY chunks) */
    int sample_rate;        /* typically 44100 */
    int channels;           /* 1 or 2 */
    int bytes_per_sample;   /* typically 2 (16-bit) */

    /* Slices (from SLCE chunks) */
    int slice_count;
    rex_slice_t slices[REX_MAX_SLICES];

    /* Decoded PCM audio (from SDAT chunk, DWVW-12 decoded) */
    int16_t *pcm_data;       /* allocated, caller must free */
    int pcm_samples;         /* total samples in pcm_data */
    int pcm_channels;        /* channels in pcm_data */

    /* Total sound length from SINF */
    uint32_t total_sample_length;

    /* Error info */
    char error[256];
} rex_file_t;

/* Parse a REX2 file from an in-memory buffer.
 * Returns 0 on success, -1 on error (check rex->error).
 * Caller must call rex_free() when done. */
int rex_parse(rex_file_t *rex, const uint8_t *data, size_t data_len);

/* Free resources allocated by rex_parse */
void rex_free(rex_file_t *rex);

#endif /* REX_PARSER_H */
