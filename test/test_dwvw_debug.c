/*
 * Debug tool: Try different DWVW bit widths on SDAT data to find the right one.
 * Also dumps first N bytes of SDAT for analysis.
 *
 * SAFETY: Max 10000 samples decoded, max 50MB input file.
 *
 * Build:
 *   cc -o test_dwvw test/test_dwvw_debug.c src/dsp/dwvw.c -Isrc/dsp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dwvw.h"

#define MAX_DECODE_SAMPLES 10000
#define MAX_INPUT_SIZE     (50 * 1024 * 1024)  /* 50MB */

/* Big-endian readers */
static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static int tag_match(const uint8_t *p, const char *tag) {
    return p[0]==tag[0] && p[1]==tag[1] && p[2]==tag[2] && p[3]==tag[3];
}

/* Find SDAT chunk in IFF data */
static const uint8_t *find_sdat(const uint8_t *data, size_t len, uint32_t *sdat_len)
{
    size_t offset = 0;
    while (offset + 8 <= len) {
        const uint8_t *tag = data + offset;
        uint32_t chunk_len = read_u32_be(data + offset + 4);
        uint32_t padded = chunk_len + (chunk_len % 2);

        if (tag_match(tag, "SDAT")) {
            *sdat_len = chunk_len;
            return data + offset + 8;
        }

        if (tag_match(tag, "CAT ")) {
            /* Search inside CAT */
            const uint8_t *found = find_sdat(data + offset + 12, chunk_len - 4, sdat_len);
            if (found) return found;
        }

        offset += 8 + padded;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.rx2>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
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

    uint32_t sdat_len;
    const uint8_t *sdat = find_sdat(buf, size, &sdat_len);
    if (!sdat) { fprintf(stderr, "No SDAT found\n"); free(buf); return 1; }

    printf("SDAT: %u bytes\n\n", sdat_len);

    /* Dump first 64 bytes */
    printf("First 64 bytes of SDAT:\n");
    for (int i = 0; i < 64 && i < (int)sdat_len; i++) {
        printf("%02X ", sdat[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n\n");

    /* Try different DWVW bit widths (capped decode) */
    int decode_max = MAX_DECODE_SAMPLES;
    int16_t *out = malloc(decode_max * sizeof(int16_t));

    for (int bw = 8; bw <= 24; bw += 4) {
        dwvw_state_t state;
        dwvw_init(&state, sdat, sdat_len, bw);
        int n = dwvw_decode(&state, out, decode_max);
        printf("DWVW-%d (no skip):   %6d samples decoded\n", bw, n);

        if (sdat_len > 8) {
            dwvw_init(&state, sdat + 8, sdat_len - 8, bw);
            n = dwvw_decode(&state, out, decode_max);
            printf("DWVW-%d (8b skip):   %6d samples decoded\n", bw, n);
        }

        if (sdat_len > 4) {
            dwvw_init(&state, sdat + 4, sdat_len - 4, bw);
            n = dwvw_decode(&state, out, decode_max);
            printf("DWVW-%d (4b skip):   %6d samples decoded\n", bw, n);
        }
        printf("\n");
    }

    /* Dump first 20 samples of best result */
    printf("\nDWVW-16 decode first 20 samples (no skip):\n");
    dwvw_state_t state;
    dwvw_init(&state, sdat, sdat_len, 16);
    int n = dwvw_decode(&state, out, 20);
    for (int i = 0; i < n; i++) {
        printf("  [%2d] %6d\n", i, out[i]);
    }

    free(out);
    free(buf);
    return 0;
}
