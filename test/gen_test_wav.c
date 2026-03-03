/*
 * Generate a test WAV file for rex-encode end-to-end testing.
 * Creates a 1-second 44100Hz 16-bit mono sine wave.
 *
 * Build: cc -o test/gen_test_wav test/gen_test_wav.c -lm
 * Run:   ./test/gen_test_wav /tmp/test_sine.wav
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = v; p[1] = v >> 8;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/tmp/test_sine.wav";
    int sr = 44100, n = 44100, ch = 1, bps = 16;
    int data_bytes = n * ch * (bps / 8);

    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    write_u32_le(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_u32_le(hdr + 16, 16);
    write_u16_le(hdr + 20, 1);       /* PCM */
    write_u16_le(hdr + 22, ch);
    write_u32_le(hdr + 24, sr);
    write_u32_le(hdr + 28, sr * ch * (bps / 8));
    write_u16_le(hdr + 32, ch * (bps / 8));
    write_u16_le(hdr + 34, bps);
    memcpy(hdr + 36, "data", 4);
    write_u32_le(hdr + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", path); return 1; }
    fwrite(hdr, 1, 44, f);

    for (int i = 0; i < n; i++) {
        int16_t s = (int16_t)(30000.0 * sin(2.0 * M_PI * 440.0 * i / sr));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("Wrote %s (%d frames)\n", path, n);
    return 0;
}
