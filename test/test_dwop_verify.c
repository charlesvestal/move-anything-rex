/*
 * Verify DWOP decoder produces correct output.
 * Tests against LLDB-captured reference data from real binary.
 *
 * Build (native macOS): cc -O2 -I../src/dsp -o test_dwop_verify \
 *                        test_dwop_verify.c ../src/dsp/dwop.c
 * Run:   timeout 30 ./test_dwop_verify
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

int main(void) {
    /* Load SDAT */
    long sdat_sz;
    uint8_t *sdat = read_file("/tmp/rex_analysis_sdat.bin", &sdat_sz);
    if (!sdat) { printf("FAIL: Cannot read SDAT\n"); return 1; }

    /* Load reference (LLDB-captured DecompressMono output) */
    long ref_sz;
    uint8_t *ref_raw = read_file("/tmp/decompress_full_int16.bin", &ref_sz);
    int16_t *ref = ref_raw ? (int16_t *)ref_raw : NULL;
    int ref_len = ref_raw ? (int)(ref_sz / 2) : 0;

    int total = 117760;
    printf("SDAT: %ld bytes, Reference: %d samples\n", sdat_sz, ref_len);

    /* Decode */
    int16_t *decoded = malloc(total * sizeof(int16_t));
    dwop_state_t dwop;
    dwop_init(&dwop, sdat, (int)sdat_sz);
    int n_decoded = dwop_decode(&dwop, decoded, total);

    printf("Decoded: %d samples\n", n_decoded);

    if (n_decoded != total) {
        printf("FAIL: Expected %d samples, got %d\n", total, n_decoded);
        return 1;
    }

    /* Verify against reference */
    if (ref) {
        int match = 0, max_diff = 0;
        int first_err = -1;
        for (int i = 0; i < n_decoded && i < ref_len; i++) {
            int diff = abs(decoded[i] - ref[i]);
            if (diff <= 0) match++;
            else if (first_err < 0) first_err = i;
            if (diff > max_diff) max_diff = diff;
        }
        int cmp = n_decoded < ref_len ? n_decoded : ref_len;
        printf("\nReference comparison: %d/%d exact match", match, cmp);
        if (match == cmp) {
            printf(" *** PERFECT ***\n");
        } else {
            printf(", first_err=%d, max_diff=%d\n", first_err, max_diff);
        }
        free(ref_raw);
    } else {
        printf("\nNo reference data (run LLDB capture first)\n");
    }

    /* Spot checks */
    printf("\nSpot checks:\n");
    printf("  [0] = %d (expect 0)\n", decoded[0]);
    printf("  [287] = %d (expect 0)\n", decoded[287]);
    printf("  [288] = %d (expect -1, first non-zero)\n", decoded[288]);
    printf("  [322] = %d (expect -231, slice 0 start)\n", decoded[322]);

    int pass = (decoded[0] == 0 && decoded[287] == 0 &&
                decoded[288] == -1 && decoded[322] == -231);
    printf("\nResult: %s\n", pass ? "PASS" : "FAIL");

    free(sdat); free(decoded);
    return pass ? 0 : 1;
}
