/*
 * rex-encode: CLI tool to create REX2 files from WAV + slice boundaries.
 *
 * Usage: rex-encode <input.wav> <output.rx2> <boundaries> [tempo]
 *
 *   boundaries: comma-separated sample positions (N+1 values for N slices)
 *               e.g. "0,44100,88200" for 2 slices
 *   tempo:      BPM (default 120)
 *
 * Exit code: 0 = success, 1 = error
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav_reader.h"
#include "rex_writer.h"

#define MAX_SLICES 1024
#define MAX_FILE_SIZE (100 * 1024 * 1024)  /* 100 MB */

static uint8_t *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*size <= 0 || *size > MAX_FILE_SIZE) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc(*size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, *size, f) != (size_t)*size) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Parse comma-separated boundaries string into array.
 * Returns number of boundaries parsed. */
static int parse_boundaries(const char *str, uint32_t *out, int max_count)
{
    int count = 0;
    const char *p = str;
    while (*p && count < max_count) {
        char *end;
        long val = strtol(p, &end, 10);
        if (end == p) break;
        out[count++] = (uint32_t)val;
        if (*end == ',') end++;
        p = end;
    }
    return count;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: rex-encode <input.wav> <output.rx2> <boundaries> [tempo]\n");
        fprintf(stderr, "  boundaries: comma-separated sample positions (N+1 for N slices)\n");
        fprintf(stderr, "  tempo: BPM (default 120)\n");
        return 1;
    }

    const char *wav_path = argv[1];
    const char *rx2_path = argv[2];
    const char *boundaries_str = argv[3];
    float tempo = (argc >= 5) ? (float)atof(argv[4]) : 120.0f;

    if (tempo <= 0 || tempo > 999) {
        fprintf(stderr, "Error: invalid tempo %.1f\n", tempo);
        return 1;
    }

    /* Parse boundaries */
    uint32_t boundaries[MAX_SLICES + 1];
    int num_boundaries = parse_boundaries(boundaries_str, boundaries, MAX_SLICES + 1);
    int num_slices = num_boundaries - 1;

    if (num_slices < 1) {
        fprintf(stderr, "Error: need at least 2 boundary values for 1 slice\n");
        return 1;
    }

    /* Read WAV file */
    long wav_size;
    uint8_t *wav_raw = read_file(wav_path, &wav_size);
    if (!wav_raw) {
        fprintf(stderr, "Error: cannot read '%s'\n", wav_path);
        return 1;
    }

    wav_file_t wav;
    if (wav_read(&wav, wav_raw, (size_t)wav_size) != 0) {
        fprintf(stderr, "Error: %s\n", wav.error);
        free(wav_raw);
        return 1;
    }
    free(wav_raw);

    /* Build slice descriptors */
    rex_write_slice_t slices[MAX_SLICES];
    for (int i = 0; i < num_slices; i++) {
        slices[i].sample_offset = boundaries[i];
        slices[i].sample_length = boundaries[i + 1] - boundaries[i];
        if ((int)(slices[i].sample_offset + slices[i].sample_length) > wav.num_frames) {
            fprintf(stderr, "Error: slice %d extends past end of audio (%u+%u > %d)\n",
                    i, slices[i].sample_offset, slices[i].sample_length, wav.num_frames);
            wav_free(&wav);
            return 1;
        }
    }

    /* Compute bars and beats from tempo and total length (assumes 4/4) */
    double total_seconds = (double)wav.num_frames / wav.sample_rate;
    double total_beats = total_seconds * tempo / 60.0;
    int bars = (int)(total_beats / 4.0);
    int beats = (int)(total_beats - bars * 4.0);
    if (bars < 1) bars = 1;

    /* Allocate output buffer */
    int out_cap = wav.num_frames * wav.channels * 3 + num_slices * 32 + 4096;
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!out_buf) {
        fprintf(stderr, "Error: failed to allocate output buffer\n");
        wav_free(&wav);
        return 1;
    }

    /* Write REX2 file */
    rex_write_params_t params;
    memset(&params, 0, sizeof(params));
    params.tempo_bpm = tempo;
    params.bars = bars;
    params.beats = beats;
    params.time_sig_num = 4;
    params.time_sig_den = 4;
    params.sample_rate = wav.sample_rate;
    params.channels = wav.channels;
    params.pcm_data = wav.pcm_data;
    params.num_frames = wav.num_frames;
    params.slice_count = num_slices;
    params.slices = slices;

    int written = rex_write(&params, out_buf, out_cap);
    if (written <= 0) {
        fprintf(stderr, "Error: rex_write failed\n");
        free(out_buf);
        wav_free(&wav);
        return 1;
    }

    /* Write output file */
    if (write_file(rx2_path, out_buf, (size_t)written) != 0) {
        fprintf(stderr, "Error: cannot write '%s'\n", rx2_path);
        free(out_buf);
        wav_free(&wav);
        return 1;
    }

    fprintf(stderr, "OK: %d slices, %d frames, %.1f BPM, %d bytes\n",
            num_slices, wav.num_frames, tempo, written);

    free(out_buf);
    wav_free(&wav);
    return 0;
}
