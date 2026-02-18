/*
 * REXWAV Reader
 *
 * Reads .rexwav files produced by the rex2rexwav desktop converter.
 *
 * License: MIT
 */

#include "rexwav_reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int rexwav_parse(rex_file_t *rex, const uint8_t *data, size_t data_len)
{
    memset(rex, 0, sizeof(*rex));

    if (data_len < 64) {
        snprintf(rex->error, sizeof(rex->error), "File too small for REXWAV header");
        return -1;
    }

    /* Check magic */
    if (data[0] != 'R' || data[1] != 'X' || data[2] != 'W' || data[3] != 'V') {
        snprintf(rex->error, sizeof(rex->error), "Not a REXWAV file (bad magic)");
        return -1;
    }

    /* Read header (all little-endian, native on ARM and x86) */
    uint32_t version       = *(const uint32_t *)(data + 4);
    uint32_t sample_rate   = *(const uint32_t *)(data + 8);
    uint32_t channels      = *(const uint32_t *)(data + 12);
    uint32_t slice_count   = *(const uint32_t *)(data + 16);
    uint32_t total_frames  = *(const uint32_t *)(data + 20);
    uint32_t tempo_mbpm    = *(const uint32_t *)(data + 24);

    if (version != 1) {
        snprintf(rex->error, sizeof(rex->error), "Unsupported REXWAV version %u", version);
        return -1;
    }

    if (slice_count > REX_MAX_SLICES) {
        snprintf(rex->error, sizeof(rex->error), "Too many slices (%u > %d)", slice_count, REX_MAX_SLICES);
        return -1;
    }

    /* Validate file size */
    size_t slice_table_size = slice_count * 8;
    size_t pcm_size = (size_t)total_frames * channels * sizeof(int16_t);
    size_t expected_size = 64 + slice_table_size + pcm_size;
    if (data_len < expected_size) {
        snprintf(rex->error, sizeof(rex->error), "File truncated (need %zu, got %zu)",
                 expected_size, data_len);
        return -1;
    }

    /* Populate rex_file_t */
    rex->sample_rate = (int)sample_rate;
    rex->channels = (int)channels;
    rex->bytes_per_sample = 2;
    rex->tempo_bpm = (float)tempo_mbpm / 1000.0f;
    rex->time_sig_num = data[28];
    rex->time_sig_den = data[29];
    rex->slice_count = (int)slice_count;
    rex->total_sample_length = total_frames;

    /* Read slice table */
    const uint8_t *stab = data + 64;
    for (int i = 0; i < (int)slice_count; i++) {
        rex->slices[i].sample_offset = *(const uint32_t *)(stab + i * 8);
        rex->slices[i].sample_length = *(const uint32_t *)(stab + i * 8 + 4);
    }

    /* Copy PCM data (we need our own allocation since the file buffer may be freed) */
    rex->pcm_data = (int16_t *)malloc(pcm_size);
    if (!rex->pcm_data) {
        snprintf(rex->error, sizeof(rex->error), "Failed to allocate %zu bytes for PCM", pcm_size);
        return -1;
    }
    memcpy(rex->pcm_data, data + 64 + slice_table_size, pcm_size);
    rex->pcm_samples = (int)total_frames;
    rex->pcm_channels = (int)channels;

    return 0;
}
