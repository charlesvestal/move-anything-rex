/*
 * REX2 File Parser
 *
 * Parses the IFF-style container format used by Propellerhead ReCycle files.
 * Big-endian byte order. Chunk structure: 4-byte tag + 4-byte length + data.
 * CAT chunks are containers holding nested chunks.
 *
 * Key chunks:
 *   GLOB - Global info (tempo, bars, beats, time signature)
 *   HEAD - Header (bytes per sample)
 *   SINF - Sound info (sample rate, total sample length)
 *   SLCE - Per-slice info (sample offset into decoded audio)
 *   SDAT - Compressed audio data (DWOP encoded)
 *
 * Slice lengths are NOT stored in SLCE chunks. They are computed from
 * the gap between consecutive slice offsets after parsing.
 *
 * License: MIT
 */

#include "rex_parser.h"
#include "dwop.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Big-endian readers */
static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static int tag_match(const uint8_t *p, const char *tag)
{
    return p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3];
}

/* Parse GLOB chunk: global metadata
 * Layout (offsets relative to chunk data start):
 *   [0:4]   unknown (possibly PPQ-related)
 *   [4:6]   bars (uint16)
 *   [6]     beats (uint8)
 *   [7]     time signature numerator (uint8)
 *   [8]     time signature denominator (uint8)
 *   [9]     sensitivity (uint8)
 *   [10:12] gate sensitivity (uint16)
 *   [12:14] gain (uint16)
 *   [14:16] pitch (uint16)
 *   [16:20] tempo in milli-BPM (uint32, divide by 1000)
 */
static void parse_glob(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 20) return;

    rex->bars = read_u16_be(data + 4);
    rex->beats = data[6];
    rex->time_sig_num = data[7];
    rex->time_sig_den = data[8];
    rex->tempo_bpm = (float)read_u32_be(data + 16) / 1000.0f;
}

/* Parse HEAD chunk: audio format header */
static void parse_head(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 6) return;
    rex->bytes_per_sample = data[5];
}

/* Parse SINF chunk: sound info
 * Layout (offsets relative to chunk data start):
 *   [0]     channels? or version
 *   [1]     unknown
 *   [2:4]   unknown
 *   [4:6]   sample rate (uint16, e.g. 0xAC44 = 44100)
 *   [6:10]  total sample length (uint32)
 */
static void parse_sinf(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 10) return;

    /* Extract sample rate */
    uint16_t sr = read_u16_be(data + 4);
    if (sr > 0) rex->sample_rate = sr;

    /* Total decoded audio length in samples */
    rex->total_sample_length = read_u32_be(data + 6);
}

/* Parse SLCE chunk: per-slice info
 * Only the sample offset is reliable; the second uint32 is NOT a length.
 * Lengths are computed after all slices are collected. */
static void parse_slce(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 4) return;
    if (rex->slice_count >= REX_MAX_SLICES) return;

    rex_slice_t *s = &rex->slices[rex->slice_count];
    s->sample_offset = read_u32_be(data + 0);
    s->sample_length = 0;  /* computed later */
    rex->slice_count++;
}

/* Decode SDAT chunk: DWOP compressed audio.
 * Uses a 5-predictor adaptive lossless codec with energy-based selection. */
static int decode_sdat(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 1) {
        snprintf(rex->error, sizeof(rex->error), "SDAT chunk empty");
        return -1;
    }

    /* Allocate output buffer */
    int max_samples;
    if (rex->total_sample_length > 0) {
        max_samples = (int)rex->total_sample_length;
    } else {
        max_samples = (int)(len * 2) + 1024;
    }
    /* Hard cap: no REX file should have more than 10M samples (~3.8 min @ 44.1kHz) */
    if (max_samples > 10000000) {
        max_samples = 10000000;
    }

    rex->pcm_data = (int16_t *)malloc(max_samples * sizeof(int16_t));
    if (!rex->pcm_data) {
        snprintf(rex->error, sizeof(rex->error), "Failed to allocate %d samples", max_samples);
        return -1;
    }

    /* Decode SDAT with DWOP codec */
    dwop_state_t dwop;
    dwop_init(&dwop, data, (int)len);
    rex->pcm_samples = dwop_decode(&dwop, rex->pcm_data, max_samples);

    if (rex->pcm_samples <= 0) {
        snprintf(rex->error, sizeof(rex->error), "DWOP decode produced no samples");
        free(rex->pcm_data);
        rex->pcm_data = NULL;
        return -1;
    }

    rex->pcm_channels = 1;

    return 0;
}

/* Recursive IFF chunk parser.
 * boundary limits how far we parse (prevents reading past CAT containers). */
static int parse_chunks(rex_file_t *rex, const uint8_t *data, size_t boundary,
                        size_t offset, int *sdat_decoded)
{
    while (offset + 8 <= boundary) {
        const uint8_t *tag = data + offset;
        uint32_t chunk_len = read_u32_be(data + offset + 4);

        /* IFF: pad to even length */
        uint32_t padded_len = chunk_len;
        if (padded_len % 2 == 1) padded_len++;

        if (offset + 8 + padded_len > boundary) {
            break;
        }

        const uint8_t *chunk_data = data + offset + 8;

        if (tag_match(tag, "CAT ")) {
            /* CAT container: 4-byte type descriptor, then nested chunks.
             * Limit recursion to within this CAT's boundary. */
            if (chunk_len >= 4) {
                size_t cat_boundary = offset + 8 + chunk_len;
                parse_chunks(rex, data, cat_boundary, offset + 12, sdat_decoded);
            }
        } else if (tag_match(tag, "GLOB")) {
            parse_glob(rex, chunk_data, chunk_len);
        } else if (tag_match(tag, "HEAD")) {
            parse_head(rex, chunk_data, chunk_len);
        } else if (tag_match(tag, "SINF")) {
            parse_sinf(rex, chunk_data, chunk_len);
        } else if (tag_match(tag, "SLCE")) {
            parse_slce(rex, chunk_data, chunk_len);
        } else if (tag_match(tag, "SDAT")) {
            if (!*sdat_decoded) {
                if (decode_sdat(rex, chunk_data, chunk_len) == 0) {
                    *sdat_decoded = 1;
                }
            }
        }

        offset += 8 + padded_len;
    }

    return 0;
}

/* Compute slice lengths from gaps between consecutive offsets */
static void compute_slice_lengths(rex_file_t *rex)
{
    for (int i = 0; i < rex->slice_count; i++) {
        if (i + 1 < rex->slice_count) {
            /* Length = next slice offset - this slice offset */
            uint32_t next_off = rex->slices[i + 1].sample_offset;
            uint32_t this_off = rex->slices[i].sample_offset;
            if (next_off > this_off) {
                rex->slices[i].sample_length = next_off - this_off;
            }
        } else {
            /* Last slice: extends to end of audio */
            if (rex->total_sample_length > rex->slices[i].sample_offset) {
                rex->slices[i].sample_length =
                    rex->total_sample_length - rex->slices[i].sample_offset;
            } else if (rex->pcm_samples > (int)rex->slices[i].sample_offset) {
                rex->slices[i].sample_length =
                    rex->pcm_samples - rex->slices[i].sample_offset;
            }
        }
    }
}

int rex_parse(rex_file_t *rex, const uint8_t *data, size_t data_len)
{
    memset(rex, 0, sizeof(*rex));

    rex->sample_rate = 44100;
    rex->channels = 1;

    if (data_len < 12) {
        snprintf(rex->error, sizeof(rex->error), "File too small (%zu bytes)", data_len);
        return -1;
    }

    /* Verify IFF CAT header */
    if (!tag_match(data, "CAT ")) {
        snprintf(rex->error, sizeof(rex->error), "Not an IFF file (no CAT header)");
        return -1;
    }

    int sdat_decoded = 0;
    parse_chunks(rex, data, data_len, 0, &sdat_decoded);

    if (!sdat_decoded || !rex->pcm_data) {
        if (!rex->error[0]) {
            snprintf(rex->error, sizeof(rex->error), "No audio data found in file");
        }
        return -1;
    }

    if (rex->slice_count == 0) {
        snprintf(rex->error, sizeof(rex->error), "No slices found in file");
        rex_free(rex);
        return -1;
    }

    /* Compute slice lengths from offset gaps */
    compute_slice_lengths(rex);

    /* Validate slice boundaries against decoded data */
    for (int i = 0; i < rex->slice_count; i++) {
        rex_slice_t *s = &rex->slices[i];
        if ((int)(s->sample_offset + s->sample_length) > rex->pcm_samples) {
            if ((int)s->sample_offset >= rex->pcm_samples) {
                s->sample_length = 0;
            } else {
                s->sample_length = rex->pcm_samples - s->sample_offset;
            }
        }
    }

    return 0;
}

void rex_free(rex_file_t *rex)
{
    if (rex->pcm_data) {
        free(rex->pcm_data);
        rex->pcm_data = NULL;
    }
    rex->pcm_samples = 0;
    rex->slice_count = 0;
}
