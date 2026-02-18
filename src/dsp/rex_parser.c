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
 *   [0]     channels (1=mono, 2=stereo)
 *   [1]     bit depth indicator (3=16-bit, 5=24-bit)
 *   [2:4]   unknown
 *   [4:6]   sample rate (uint16, e.g. 0xAC44 = 44100)
 *   [6:10]  total sample length in per-channel frames (uint32)
 */
static void parse_sinf(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 10) return;

    /* Channel count from byte 0 */
    int ch = data[0];
    if (ch == 1 || ch == 2)
        rex->channels = ch;

    /* Extract sample rate */
    uint16_t sr = read_u16_be(data + 4);
    if (sr > 0) rex->sample_rate = sr;

    /* Total decoded audio length in per-channel frames */
    rex->total_sample_length = read_u32_be(data + 6);
}

/* Parse SLCE chunk: per-slice info
 * Layout (11 bytes):
 *   [0:4]  sample offset (uint32 BE)
 *   [4:8]  sample length (uint32 BE) — 1 = transient marker, >1 = real audio slice
 *   [8:10] amplitude/sensitivity (uint16 BE)
 *   [10]   zero
 *
 * Transient markers (length=1) are sub-slice positions within real slices.
 * Only real slices (length > 1) are kept for playback. */
static void parse_slce(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 8) return;
    if (rex->slice_count >= REX_MAX_SLICES) return;

    uint32_t offset = read_u32_be(data + 0);
    uint32_t sample_len = read_u32_be(data + 4);

    /* Skip transient markers (length=1) — these are not playable slices */
    if (sample_len <= 1) return;

    rex_slice_t *s = &rex->slices[rex->slice_count];
    s->sample_offset = offset;
    s->sample_length = sample_len;
    rex->slice_count++;
}

/* Decode SDAT chunk: DWOP compressed audio.
 * Uses a 5-predictor adaptive lossless codec with energy-based selection.
 * Stereo files use L/delta encoding (R = L + delta). */
static int decode_sdat(rex_file_t *rex, const uint8_t *data, uint32_t len)
{
    if (len < 1) {
        snprintf(rex->error, sizeof(rex->error), "SDAT chunk empty");
        return -1;
    }

    /* Max frames (per-channel sample count) */
    int max_frames;
    if (rex->total_sample_length > 0) {
        max_frames = (int)rex->total_sample_length;
    } else {
        max_frames = (int)(len * 2) + 1024;
    }
    /* Hard cap: no REX file should have more than 10M frames (~3.8 min @ 44.1kHz) */
    if (max_frames > 10000000) {
        max_frames = 10000000;
    }

    int is_stereo = (rex->channels == 2);

    /* Allocate output: stereo needs 2x for interleaved L/R */
    size_t alloc_samples = (size_t)max_frames * (is_stereo ? 2 : 1);
    rex->pcm_data = (int16_t *)malloc(alloc_samples * sizeof(int16_t));
    if (!rex->pcm_data) {
        snprintf(rex->error, sizeof(rex->error), "Failed to allocate %zu samples", alloc_samples);
        return -1;
    }

    if (is_stereo) {
        rex->pcm_samples = dwop_decode_stereo(data, (int)len,
                                               rex->pcm_data, max_frames);
        rex->pcm_channels = 2;
    } else {
        dwop_state_t dwop;
        dwop_init(&dwop, data, (int)len);
        rex->pcm_samples = dwop_decode(&dwop, rex->pcm_data, max_frames);
        rex->pcm_channels = 1;
    }

    if (rex->pcm_samples <= 0) {
        snprintf(rex->error, sizeof(rex->error), "DWOP decode produced no samples");
        free(rex->pcm_data);
        rex->pcm_data = NULL;
        return -1;
    }

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

/* Clamp slice lengths to decoded PCM buffer bounds */
static void clamp_slice_lengths(rex_file_t *rex)
{
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
        /* All SLCE entries were transient markers (length <= 1).
         * Fall back: treat entire decoded audio as one slice. */
        if (rex->pcm_samples > 0) {
            rex->slices[0].sample_offset = 0;
            rex->slices[0].sample_length = (uint32_t)rex->pcm_samples;
            rex->slice_count = 1;
        } else {
            snprintf(rex->error, sizeof(rex->error), "No slices found in file");
            rex_free(rex);
            return -1;
        }
    }

    /* Clamp slice lengths to decoded PCM buffer bounds */
    clamp_slice_lengths(rex);

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
