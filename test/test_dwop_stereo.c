/*
 * Verify stereo DWOP decoder produces correct output.
 * Tests against LLDB-captured reference data from real DecompressStereo binary.
 *
 * Reference file: /tmp/stereo_decompress_output.bin
 *   - 183056 bytes = 91528 frames * 2 channels * 2 bytes/sample
 *   - Interleaved int16 LE: L0, R0, L1, R1, ...
 *   - Captured from REX SDK's DecompressStereo on 120Stereo.rx2
 *
 * Build (native macOS): cc -O2 -I../src/dsp -o test_dwop_stereo \
 *                        test_dwop_stereo.c ../src/dsp/dwop.c
 * Run:   timeout 30 ./test_dwop_stereo
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "dwop.h"

#define MAX_INPUT (50*1024*1024)

static uint8_t *read_file(const char *p, long *sz) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (*sz <= 0 || *sz > MAX_INPUT) { fclose(f); return NULL; }
    uint8_t *b = malloc(*sz); fread(b, 1, *sz, f); fclose(f); return b;
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

int main(void) {
    const char *rex_path = "/Users/charlesvestal/SDKs/REXSDK_Mac_1.9.2/REX Test Protocol Files/120Stereo.rx2";

    /* Load REX file and extract SDAT */
    long fsz;
    uint8_t *data = read_file(rex_path, &fsz);
    if (!data) { printf("FAIL: Cannot read %s\n", rex_path); return 1; }

    const uint8_t *sdat = NULL; uint32_t sdat_len = 0, total_len = 0;
    for (size_t off = 0; off + 8 <= (size_t)fsz; ) {
        uint32_t tag = rd32(data + off);
        uint32_t clen = rd32(data + off + 4);
        uint32_t padded = clen + (clen & 1);
        if (tag == 0x43415420) { off += 12; continue; }
        if (tag == 0x53494E46 && clen >= 10) total_len = rd32(data + off + 8 + 6);
        if (tag == 0x53444154) { sdat = data + off + 8; sdat_len = clen; }
        off += 8 + padded;
    }

    if (!sdat) { printf("FAIL: No SDAT chunk\n"); free(data); return 1; }
    printf("SDAT: %u bytes, total_sample_length: %u frames\n", sdat_len, total_len);

    /* Load reference (LLDB-captured DecompressStereo output, interleaved int16) */
    long ref_sz;
    uint8_t *ref_raw = read_file("/tmp/stereo_decompress_output.bin", &ref_sz);
    if (!ref_raw) { printf("FAIL: Cannot read reference\n"); free(data); return 1; }
    int16_t *ref = (int16_t *)ref_raw;
    int ref_frames = (int)(ref_sz / 4);  /* 4 bytes per frame (2 channels * 2 bytes) */
    printf("Reference: %d stereo frames\n", ref_frames);

    /* Decode with our stereo decoder */
    int max_frames = (int)total_len;
    int16_t *decoded = malloc(max_frames * 2 * sizeof(int16_t));
    int n_frames = dwop_decode_stereo(sdat, (int)sdat_len, decoded, max_frames);
    printf("Decoded: %d stereo frames\n", n_frames);

    if (n_frames != max_frames) {
        printf("FAIL: Expected %d frames, got %d\n", max_frames, n_frames);
        free(data); free(ref_raw); free(decoded);
        return 1;
    }

    /* Verify against reference */
    int cmp = n_frames < ref_frames ? n_frames : ref_frames;
    int match = 0, first_err = -1;
    for (int i = 0; i < cmp; i++) {
        int16_t dec_l = decoded[i * 2];
        int16_t dec_r = decoded[i * 2 + 1];
        int16_t ref_l = ref[i * 2];
        int16_t ref_r = ref[i * 2 + 1];
        if (dec_l == ref_l && dec_r == ref_r) {
            match++;
        } else if (first_err < 0) {
            first_err = i;
        }
    }

    printf("\nStereo comparison: %d/%d frames match", match, cmp);
    if (match == cmp) {
        printf(" *** PERFECT ***\n");
    } else {
        printf("\n  First error at frame %d\n", first_err);
        if (first_err >= 0) {
            printf("  got L=%d R=%d, expected L=%d R=%d\n",
                   decoded[first_err*2], decoded[first_err*2+1],
                   ref[first_err*2], ref[first_err*2+1]);
        }
    }

    /* Show first 10 frames */
    printf("\nFirst 10 frames:\n");
    for (int i = 0; i < 10 && i < cmp; i++) {
        printf("  [%d] L=%d (ref %d %s), R=%d (ref %d %s)\n",
               i, decoded[i*2], ref[i*2],
               decoded[i*2]==ref[i*2] ? "OK" : "FAIL",
               decoded[i*2+1], ref[i*2+1],
               decoded[i*2+1]==ref[i*2+1] ? "OK" : "FAIL");
    }

    int pass = (match == cmp);
    printf("\nResult: %s\n", pass ? "PASS" : "FAIL");

    free(data); free(ref_raw); free(decoded);
    return pass ? 0 : 1;
}
