/*
 * REX2 Writer Round-Trip Test
 *
 * Verifies: write REX2 file → parse with rex_parse() → verify all metadata
 * and decoded audio match the original input.
 *
 * Build (native macOS/Linux):
 *   cc -O2 -Isrc/dsp -o test/test_rex_roundtrip \
 *      test/test_rex_roundtrip.c src/dsp/rex_writer.c src/dsp/dwop_encode.c \
 *      src/dsp/rex_parser.c src/dsp/dwop.c -lm
 *
 * Run:   ./test/test_rex_roundtrip
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "rex_writer.h"
#include "rex_parser.h"

static int test_count = 0;
static int pass_count = 0;

static int test_mono_roundtrip(const char *name, const int16_t *pcm, int num_frames,
                                int num_slices, const rex_write_slice_t *slices,
                                float tempo, int bars, int beats)
{
    test_count++;
    printf("  %-40s ... ", name);

    /* Write REX2 */
    int buf_cap = num_frames * 4 + 4096;
    uint8_t *buf = (uint8_t *)malloc(buf_cap);
    if (!buf) { printf("FAIL (malloc)\n"); return 0; }

    rex_write_params_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.tempo_bpm = tempo;
    wp.bars = bars;
    wp.beats = beats;
    wp.time_sig_num = 4;
    wp.time_sig_den = 4;
    wp.sample_rate = 44100;
    wp.channels = 1;
    wp.pcm_data = pcm;
    wp.num_frames = num_frames;
    wp.slice_count = num_slices;
    wp.slices = slices;

    int written = rex_write(&wp, buf, buf_cap);
    if (written <= 0) {
        printf("FAIL (rex_write returned %d)\n", written);
        free(buf);
        return 0;
    }

    /* Parse back */
    rex_file_t rex;
    int rc = rex_parse(&rex, buf, (size_t)written);
    if (rc != 0) {
        printf("FAIL (rex_parse: %s)\n", rex.error);
        free(buf);
        return 0;
    }

    /* Verify metadata */
    int errors = 0;
    if (rex.slice_count != num_slices) {
        printf("FAIL (slices: got %d, want %d)\n", rex.slice_count, num_slices);
        errors++;
    }
    if (abs(rex.bars - bars) > 0) {
        printf("FAIL (bars: got %d, want %d)\n", rex.bars, bars);
        errors++;
    }
    if (rex.pcm_samples != num_frames) {
        printf("FAIL (frames: got %d, want %d)\n", rex.pcm_samples, num_frames);
        errors++;
    }
    if (fabs(rex.tempo_bpm - tempo) > 0.1f) {
        printf("FAIL (tempo: got %.1f, want %.1f)\n", rex.tempo_bpm, tempo);
        errors++;
    }
    if (rex.sample_rate != 44100) {
        printf("FAIL (rate: got %d, want 44100)\n", rex.sample_rate);
        errors++;
    }
    if (rex.channels != 1) {
        printf("FAIL (channels: got %d, want 1)\n", rex.channels);
        errors++;
    }

    /* Verify slice offsets and lengths */
    for (int i = 0; i < num_slices && i < rex.slice_count; i++) {
        if (rex.slices[i].sample_offset != slices[i].sample_offset) {
            printf("FAIL (slice[%d] offset: got %u, want %u)\n",
                   i, rex.slices[i].sample_offset, slices[i].sample_offset);
            errors++;
        }
        if (rex.slices[i].sample_length != slices[i].sample_length) {
            printf("FAIL (slice[%d] length: got %u, want %u)\n",
                   i, rex.slices[i].sample_length, slices[i].sample_length);
            errors++;
        }
    }

    /* Verify PCM audio (exact match) */
    if (errors == 0 && rex.pcm_data) {
        int mismatches = 0;
        int first_err = -1;
        for (int i = 0; i < num_frames && i < rex.pcm_samples; i++) {
            if (rex.pcm_data[i] != pcm[i]) {
                if (first_err < 0) first_err = i;
                mismatches++;
            }
        }
        if (mismatches > 0) {
            printf("FAIL (PCM: %d mismatches, first at %d)\n", mismatches, first_err);
            errors++;
        }
    }

    rex_free(&rex);
    free(buf);

    if (errors == 0) {
        printf("PASS (%d bytes)\n", written);
        pass_count++;
        return 1;
    }
    return 0;
}

static int test_stereo_roundtrip(const char *name, const int16_t *pcm, int num_frames,
                                  int num_slices, const rex_write_slice_t *slices,
                                  float tempo)
{
    test_count++;
    printf("  %-40s ... ", name);

    int buf_cap = num_frames * 8 + 4096;
    uint8_t *buf = (uint8_t *)malloc(buf_cap);
    if (!buf) { printf("FAIL (malloc)\n"); return 0; }

    rex_write_params_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.tempo_bpm = tempo;
    wp.bars = 1;
    wp.beats = 0;
    wp.time_sig_num = 4;
    wp.time_sig_den = 4;
    wp.sample_rate = 44100;
    wp.channels = 2;
    wp.pcm_data = pcm;
    wp.num_frames = num_frames;
    wp.slice_count = num_slices;
    wp.slices = slices;

    int written = rex_write(&wp, buf, buf_cap);
    if (written <= 0) {
        printf("FAIL (rex_write returned %d)\n", written);
        free(buf);
        return 0;
    }

    rex_file_t rex;
    int rc = rex_parse(&rex, buf, (size_t)written);
    if (rc != 0) {
        printf("FAIL (rex_parse: %s)\n", rex.error);
        free(buf);
        return 0;
    }

    int errors = 0;
    if (rex.channels != 2 || rex.pcm_channels != 2) {
        printf("FAIL (channels: got %d/%d, want 2)\n", rex.channels, rex.pcm_channels);
        errors++;
    }

    /* Verify PCM (interleaved L/R) */
    if (errors == 0 && rex.pcm_data) {
        int mismatches = 0;
        int first_err = -1;
        for (int i = 0; i < num_frames * 2 && i < rex.pcm_samples * 2; i++) {
            if (rex.pcm_data[i] != pcm[i]) {
                if (first_err < 0) first_err = i;
                mismatches++;
            }
        }
        if (mismatches > 0) {
            printf("FAIL (PCM: %d mismatches, first at %d)\n", mismatches, first_err);
            errors++;
        }
    }

    rex_free(&rex);
    free(buf);

    if (errors == 0) {
        printf("PASS (%d bytes)\n", written);
        pass_count++;
        return 1;
    }
    return 0;
}

int main(void)
{
    printf("=== REX2 Writer Round-Trip Tests ===\n\n");

    /* --- Test 1: Simple 2-slice silence --- */
    {
        int16_t pcm[44100];
        memset(pcm, 0, sizeof(pcm));
        rex_write_slice_t slices[2] = {
            {0, 22050},
            {22050, 22050}
        };
        test_mono_roundtrip("Mono silence (2 slices)", pcm, 44100,
                            2, slices, 120.0f, 2, 0);
    }

    /* --- Test 2: Sine wave, 4 slices --- */
    {
        int n = 44100;
        int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
        for (int i = 0; i < n; i++)
            pcm[i] = (int16_t)(30000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
        rex_write_slice_t slices[4] = {
            {0, 11025}, {11025, 11025}, {22050, 11025}, {33075, 11025}
        };
        test_mono_roundtrip("Mono sine 440Hz (4 slices)", pcm, n,
                            4, slices, 120.0f, 2, 0);
        free(pcm);
    }

    /* --- Test 3: Single slice, whole file --- */
    {
        int n = 8820;  /* 200ms */
        int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
        uint32_t seed = 0xCAFE;
        for (int i = 0; i < n; i++) {
            seed = seed * 1664525 + 1013904223;
            double env = exp(-3.0 * i / n);
            pcm[i] = (int16_t)(env * ((int32_t)(seed >> 16)) / 2);
        }
        rex_write_slice_t slices[1] = {{0, 8820}};
        test_mono_roundtrip("Mono drum hit (1 slice)", pcm, n,
                            1, slices, 140.0f, 1, 0);
        free(pcm);
    }

    /* --- Test 4: Many slices (32) --- */
    {
        int n = 44100;
        int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
        for (int i = 0; i < n; i++)
            pcm[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * 220.0 * i / 44100.0));
        rex_write_slice_t slices[32];
        int slice_len = n / 32;
        for (int i = 0; i < 32; i++) {
            slices[i].sample_offset = i * slice_len;
            slices[i].sample_length = slice_len;
        }
        test_mono_roundtrip("Mono 32 slices", pcm, n,
                            32, slices, 120.0f, 4, 0);
        free(pcm);
    }

    /* --- Test 5: Non-standard tempo --- */
    {
        int16_t pcm[22050];
        for (int i = 0; i < 22050; i++)
            pcm[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 880.0 * i / 44100.0));
        rex_write_slice_t slices[2] = {{0, 11025}, {11025, 11025}};
        test_mono_roundtrip("Mono 87.5 BPM (2 slices)", pcm, 22050,
                            2, slices, 87.5f, 1, 0);
    }

    /* --- Test 6: Stereo 2 slices --- */
    {
        int n = 22050;
        int16_t *pcm = (int16_t *)malloc(n * 2 * sizeof(int16_t));
        for (int i = 0; i < n; i++) {
            pcm[i * 2]     = (int16_t)(20000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
            pcm[i * 2 + 1] = (int16_t)(15000.0 * sin(2.0 * M_PI * 554.0 * i / 44100.0));
        }
        rex_write_slice_t slices[2] = {{0, 11025}, {11025, 11025}};
        test_stereo_roundtrip("Stereo diff freq (2 slices)", pcm, n,
                               2, slices, 120.0f);
        free(pcm);
    }

    /* --- Test 7: IFF alignment stress (odd compressed size) --- */
    {
        /* 3 samples of specific values to try to produce odd compressed byte count */
        int16_t pcm[3] = {100, -200, 300};
        rex_write_slice_t slices[1] = {{0, 3}};
        test_mono_roundtrip("Tiny 3-sample file", pcm, 3,
                            1, slices, 120.0f, 1, 0);
    }

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
