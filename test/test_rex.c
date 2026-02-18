/*
 * Test program for REX parser and DWVW-12 decoder.
 *
 * Parses a .rx2 file and dumps slice info + optionally writes decoded audio
 * to a raw PCM file for verification.
 *
 * SAFETY: Max 50MB input, max 500K samples per WAV output, max 256 slices.
 *
 * Build (native macOS/Linux):
 *   cc -o test_rex test/test_rex.c src/dsp/dwvw.c src/dsp/rex_parser.c -Isrc/dsp
 *
 * Usage:
 *   ./test_rex input.rx2 [--dump-wav]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rex_parser.h"

#define MAX_INPUT_SIZE    (50 * 1024 * 1024)
#define MAX_WAV_SAMPLES   500000

static void write_wav(const char *path, const int16_t *pcm, int samples, int sample_rate, int channels)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "Cannot write %s\n", path); return; }

    uint32_t data_bytes = samples * channels * sizeof(int16_t);
    uint32_t chunk_size = 36 + data_bytes;
    uint16_t audio_fmt = 1;
    uint16_t num_ch = (uint16_t)channels;
    uint32_t sr = (uint32_t)sample_rate;
    uint16_t bits = 16;
    uint16_t block_align = num_ch * bits / 8;
    uint32_t byte_rate = sr * block_align;
    uint32_t fmt_size = 16;

    fwrite("RIFF", 1, 4, fp);
    fwrite(&chunk_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_fmt, 2, 1, fp);
    fwrite(&num_ch, 2, 1, fp);
    fwrite(&sr, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, 4, 1, fp);
    fwrite(pcm, sizeof(int16_t), samples * channels, fp);

    fclose(fp);
    printf("Wrote %s (%d samples, %d Hz)\n", path, samples, sample_rate);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.rx2> [--dump-wav]\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    int dump_wav = (argc > 2 && strcmp(argv[2], "--dump-wav") == 0);

    /* Read file */
    FILE *fp = fopen(input, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", input); return 1; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > MAX_INPUT_SIZE) {
        fprintf(stderr, "File size %ld out of range (max %d)\n", size, MAX_INPUT_SIZE);
        fclose(fp);
        return 1;
    }

    uint8_t *buf = malloc(size);
    fread(buf, 1, size, fp);
    fclose(fp);

    printf("File: %s (%ld bytes)\n", input, size);

    /* Parse */
    rex_file_t rex;
    int rc = rex_parse(&rex, buf, size);
    free(buf);

    if (rc != 0) {
        fprintf(stderr, "Parse error: %s\n", rex.error);
        return 1;
    }

    /* Dump info */
    printf("\n=== REX File Info ===\n");
    printf("Tempo:       %.1f BPM\n", rex.tempo_bpm);
    printf("Time Sig:    %d/%d\n", rex.time_sig_num, rex.time_sig_den);
    printf("Bars:        %d\n", rex.bars);
    printf("Beats:       %d\n", rex.beats);
    printf("Sample Rate: %d Hz\n", rex.sample_rate);
    printf("Channels:    %d\n", rex.channels);
    printf("Total PCM:   %d samples\n", rex.pcm_samples);
    printf("Slices:      %d\n", rex.slice_count);

    printf("\n=== Slices ===\n");
    for (int i = 0; i < rex.slice_count; i++) {
        rex_slice_t *s = &rex.slices[i];
        float dur_ms = (float)s->sample_length / rex.sample_rate * 1000.0f;
        printf("  Slice %2d: offset=%6u  length=%6u  (%.1f ms)  MIDI note=%d\n",
               i, s->sample_offset, s->sample_length, dur_ms, 36 + i);
    }

    if (dump_wav && rex.pcm_data) {
        /* Write full decoded audio (capped) */
        int full_samples = rex.pcm_samples;
        if (full_samples > MAX_WAV_SAMPLES) {
            fprintf(stderr, "Warning: capping WAV output to %d samples (was %d)\n",
                    MAX_WAV_SAMPLES, full_samples);
            full_samples = MAX_WAV_SAMPLES;
        }
        write_wav("rex_decoded_full.wav", rex.pcm_data, full_samples,
                  rex.sample_rate, 1);

        /* Write individual slices */
        for (int i = 0; i < rex.slice_count; i++) {
            rex_slice_t *s = &rex.slices[i];
            if (s->sample_length == 0) continue;

            int slice_len = (int)s->sample_length;
            if (slice_len > MAX_WAV_SAMPLES) slice_len = MAX_WAV_SAMPLES;

            char fname[64];
            snprintf(fname, sizeof(fname), "rex_slice_%02d.wav", i);
            write_wav(fname, rex.pcm_data + s->sample_offset, slice_len,
                     rex.sample_rate, 1);
        }
    }

    rex_free(&rex);
    printf("\nDone.\n");
    return 0;
}
