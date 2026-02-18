/*
 * Integration test: parse a stereo .rx2 file through the full rex_parse() pipeline
 * and verify the decoded PCM matches reference data.
 *
 * Build (native macOS): cc -O2 -I../src/dsp -o test_rex_stereo \
 *                        test_rex_stereo.c ../src/dsp/rex_parser.c ../src/dsp/dwop.c
 * Run:   timeout 30 ./test_rex_stereo
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "rex_parser.h"

#define MAX_INPUT (50*1024*1024)

static uint8_t *read_file(const char *p, long *sz) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (*sz <= 0 || *sz > MAX_INPUT) { fclose(f); return NULL; }
    uint8_t *b = malloc(*sz); fread(b, 1, *sz, f); fclose(f); return b;
}

int main(void) {
    const char *rex_path = "/Users/charlesvestal/SDKs/REXSDK_Mac_1.9.2/REX Test Protocol Files/120Stereo.rx2";
    int pass = 1;

    /* Load REX file */
    long fsz;
    uint8_t *data = read_file(rex_path, &fsz);
    if (!data) { printf("FAIL: Cannot read %s\n", rex_path); return 1; }

    /* Parse through full pipeline */
    rex_file_t rex;
    int rc = rex_parse(&rex, data, fsz);
    free(data);

    if (rc != 0) {
        printf("FAIL: rex_parse failed: %s\n", rex.error);
        return 1;
    }

    printf("Parsed: %d channels, %d Hz, %d frames, %d slices, %.1f BPM\n",
           rex.pcm_channels, rex.sample_rate, rex.pcm_samples,
           rex.slice_count, rex.tempo_bpm);

    /* Verify stereo detected */
    if (rex.channels != 2 || rex.pcm_channels != 2) {
        printf("FAIL: Expected stereo (channels=2), got channels=%d pcm_channels=%d\n",
               rex.channels, rex.pcm_channels);
        pass = 0;
    }

    /* Load reference */
    long ref_sz;
    uint8_t *ref_raw = read_file("/tmp/stereo_decompress_output.bin", &ref_sz);
    if (ref_raw) {
        int16_t *ref = (int16_t *)ref_raw;
        int ref_frames = (int)(ref_sz / 4);

        int cmp = rex.pcm_samples < ref_frames ? rex.pcm_samples : ref_frames;
        int match = 0, first_err = -1;
        for (int i = 0; i < cmp; i++) {
            int16_t dec_l = rex.pcm_data[i * 2];
            int16_t dec_r = rex.pcm_data[i * 2 + 1];
            if (dec_l == ref[i * 2] && dec_r == ref[i * 2 + 1]) {
                match++;
            } else if (first_err < 0) {
                first_err = i;
            }
        }

        printf("Reference comparison: %d/%d frames match", match, cmp);
        if (match == cmp) {
            printf(" *** PERFECT ***\n");
        } else {
            printf("\n  First error at frame %d\n", first_err);
            pass = 0;
        }
        free(ref_raw);
    } else {
        printf("No reference file (skipping verification)\n");
    }

    /* Verify slice info */
    printf("\nSlice info:\n");
    for (int i = 0; i < rex.slice_count && i < 5; i++) {
        printf("  [%d] offset=%u length=%u\n",
               i, rex.slices[i].sample_offset, rex.slices[i].sample_length);
    }
    if (rex.slice_count > 5) printf("  ... (%d total)\n", rex.slice_count);

    printf("\nResult: %s\n", pass ? "PASS" : "FAIL");

    rex_free(&rex);
    return pass ? 0 : 1;
}
