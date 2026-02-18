/*
 * Test: try interpreting SDAT as different raw PCM formats.
 * Write WAVs to compare with REX SDK output.
 *
 * SAFETY: Max 50MB input, max 500K samples per WAV output.
 *
 * Build:
 *   cc -o test_raw test/test_raw_pcm.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_INPUT_SIZE    (50 * 1024 * 1024)
#define MAX_WAV_SAMPLES   500000

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static int tag_match(const uint8_t *p, const char *tag) {
    return p[0]==tag[0] && p[1]==tag[1] && p[2]==tag[2] && p[3]==tag[3];
}

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
            const uint8_t *found = find_sdat(data + offset + 12, chunk_len - 4, sdat_len);
            if (found) return found;
        }
        offset += 8 + padded;
    }
    return NULL;
}

static void write_wav(const char *path, const int16_t *pcm, int samples, int sr)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    uint32_t data_bytes = samples * 2;
    uint32_t chunk_size = 36 + data_bytes;
    uint16_t fmt = 1, ch = 1, bits = 16, ba = 2;
    uint32_t br = sr * 2, fmtsize = 16;
    fwrite("RIFF", 1, 4, fp);
    fwrite(&chunk_size, 4, 1, fp);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    fwrite(&fmtsize, 4, 1, fp);
    fwrite(&fmt, 2, 1, fp);
    fwrite(&ch, 2, 1, fp);
    fwrite(&sr, 4, 1, fp);
    fwrite(&br, 4, 1, fp);
    fwrite(&ba, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, 4, 1, fp);
    fwrite(pcm, 2, samples, fp);
    fclose(fp);
    printf("Wrote %s (%d samples)\n", path, samples);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { fprintf(stderr, "Usage: %s <input.rx2>\n", argv[0]); return 1; }

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
    if (!sdat) { fprintf(stderr, "No SDAT\n"); return 1; }

    printf("SDAT: %u bytes\n", sdat_len);

    /* Dump first 8 bytes of SDAT in different interpretations */
    printf("First 16 bytes:\n");
    for (int i = 0; i < 16 && i < (int)sdat_len; i++)
        printf("%02X ", sdat[i]);
    printf("\n\n");

    /* Try 1: signed 8-bit PCM, no skip */
    {
        int n = (int)sdat_len;
        if (n > MAX_WAV_SAMPLES) n = MAX_WAV_SAMPLES;
        int16_t *out = malloc(n * 2);
        for (int i = 0; i < n; i++)
            out[i] = ((int8_t)sdat[i]) * 256;
        write_wav("test_8bit_signed.wav", out, n, 44100);
        free(out);
    }

    /* Try 2: unsigned 8-bit PCM, no skip */
    {
        int n = (int)sdat_len;
        if (n > MAX_WAV_SAMPLES) n = MAX_WAV_SAMPLES;
        int16_t *out = malloc(n * 2);
        for (int i = 0; i < n; i++)
            out[i] = ((int)sdat[i] - 128) * 256;
        write_wav("test_8bit_unsigned.wav", out, n, 44100);
        free(out);
    }

    /* Try 3: 16-bit big-endian PCM */
    {
        int n = (int)(sdat_len / 2);
        if (n > MAX_WAV_SAMPLES) n = MAX_WAV_SAMPLES;
        int16_t *out = malloc(n * 2);
        for (int i = 0; i < n; i++)
            out[i] = (int16_t)((sdat[i*2] << 8) | sdat[i*2+1]);
        write_wav("test_16bit_be.wav", out, n, 44100);
        free(out);
    }

    /* Try 4: 16-bit little-endian PCM */
    {
        int n = (int)(sdat_len / 2);
        if (n > MAX_WAV_SAMPLES) n = MAX_WAV_SAMPLES;
        int16_t *out = malloc(n * 2);
        for (int i = 0; i < n; i++)
            out[i] = (int16_t)((sdat[i*2+1] << 8) | sdat[i*2]);
        write_wav("test_16bit_le.wav", out, n, 44100);
        free(out);
    }

    /* Try 5: 8-bit PCM with 544-byte header skip (to match 117760 expected) */
    {
        int skip = sdat_len - 117760;
        if (skip > 0 && skip < 1024) {
            printf("Header skip hypothesis: %d bytes\n", skip);
            int n = 117760;
            if (n > MAX_WAV_SAMPLES) n = MAX_WAV_SAMPLES;
            int16_t *out = malloc(n * 2);
            for (int i = 0; i < n; i++)
                out[i] = ((int8_t)sdat[skip + i]) * 256;
            write_wav("test_8bit_skip544.wav", out, n, 44100);
            free(out);
        }
    }

    free(buf);
    return 0;
}
