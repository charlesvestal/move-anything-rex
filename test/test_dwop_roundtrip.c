/*
 * DWOP Encoder Round-Trip Test
 *
 * Verifies: encode PCM → DWOP bitstream → decode with dwop_decode() → exact match.
 * Tests: silence, DC, impulse, ramp, sine, short buffers, stereo.
 *
 * Build (native macOS/Linux):
 *   cc -O2 -I../src/dsp -o test_dwop_roundtrip \
 *      test_dwop_roundtrip.c ../src/dsp/dwop_encode.c ../src/dsp/dwop.c -lm
 *
 * Run:   ./test_dwop_roundtrip
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "dwop.h"
#include "dwop_encode.h"

#define BUF_CAP (1024 * 1024)  /* 1 MB compressed buffer */

static int test_count = 0;
static int pass_count = 0;

/* Round-trip test for mono: encode then decode, verify exact match */
static int roundtrip_mono(const char *name, const int16_t *pcm, int num_samples)
{
    test_count++;
    printf("  %-30s (%6d samples) ... ", name, num_samples);

    uint8_t *compressed = malloc(BUF_CAP);
    if (!compressed) { printf("FAIL (malloc)\n"); return 0; }

    /* Encode */
    dwop_enc_state_t enc;
    dwop_enc_init(&enc, compressed, BUF_CAP);
    int enc_count = dwop_encode(&enc, pcm, num_samples, 1);
    int comp_bytes = dwop_enc_flush(&enc);

    if (enc_count != num_samples) {
        printf("FAIL (encoded %d/%d)\n", enc_count, num_samples);
        free(compressed);
        return 0;
    }

    /* Decode */
    int16_t *decoded = malloc(num_samples * sizeof(int16_t));
    if (!decoded) { printf("FAIL (malloc)\n"); free(compressed); return 0; }

    dwop_state_t dec;
    dwop_init(&dec, compressed, comp_bytes);
    int dec_count = dwop_decode(&dec, decoded, num_samples, 1);

    if (dec_count != num_samples) {
        printf("FAIL (decoded %d/%d)\n", dec_count, num_samples);
        free(compressed);
        free(decoded);
        return 0;
    }

    /* Verify exact match */
    int mismatches = 0;
    int first_err = -1;
    for (int i = 0; i < num_samples; i++) {
        if (decoded[i] != pcm[i]) {
            if (first_err < 0) first_err = i;
            mismatches++;
        }
    }

    if (mismatches == 0) {
        float ratio = num_samples > 0 ? (float)comp_bytes / (num_samples * 2) * 100.0f : 0;
        printf("PASS (%d bytes, %.1f%%)\n", comp_bytes, ratio);
        pass_count++;
        return 1;
    } else {
        printf("FAIL (%d mismatches, first at %d: got %d, want %d)\n",
               mismatches, first_err, decoded[first_err], pcm[first_err]);
        free(compressed);
        free(decoded);
        return 0;
    }
}

/* Round-trip test for stereo */
static int roundtrip_stereo(const char *name, const int16_t *pcm, int num_frames)
{
    test_count++;
    printf("  %-30s (%6d frames) ... ", name, num_frames);

    uint8_t *compressed = malloc(BUF_CAP);
    if (!compressed) { printf("FAIL (malloc)\n"); return 0; }

    /* Encode stereo */
    int comp_bytes = dwop_encode_stereo(pcm, num_frames, compressed, BUF_CAP, 1);
    if (comp_bytes <= 0) {
        printf("FAIL (encode returned %d)\n", comp_bytes);
        free(compressed);
        return 0;
    }

    /* Decode stereo */
    int16_t *decoded = malloc(num_frames * 2 * sizeof(int16_t));
    if (!decoded) { printf("FAIL (malloc)\n"); free(compressed); return 0; }

    int dec_frames = dwop_decode_stereo(compressed, comp_bytes,
                                         decoded, num_frames, 1);
    if (dec_frames != num_frames) {
        printf("FAIL (decoded %d/%d frames)\n", dec_frames, num_frames);
        free(compressed);
        free(decoded);
        return 0;
    }

    /* Verify exact match */
    int mismatches = 0;
    int first_err = -1;
    for (int i = 0; i < num_frames * 2; i++) {
        if (decoded[i] != pcm[i]) {
            if (first_err < 0) first_err = i;
            mismatches++;
        }
    }

    if (mismatches == 0) {
        float ratio = (float)comp_bytes / (num_frames * 4) * 100.0f;
        printf("PASS (%d bytes, %.1f%%)\n", comp_bytes, ratio);
        pass_count++;
        return 1;
    } else {
        printf("FAIL (%d mismatches, first at [%d]: got %d, want %d)\n",
               mismatches, first_err, decoded[first_err], pcm[first_err]);
        free(compressed);
        free(decoded);
        return 0;
    }
}

int main(void)
{
    printf("=== DWOP Encoder Round-Trip Tests ===\n\n");

    /* --- Mono tests --- */
    printf("Mono tests:\n");

    /* 1. Silence */
    {
        int16_t silence[1024];
        memset(silence, 0, sizeof(silence));
        roundtrip_mono("Silence (1024)", silence, 1024);
    }

    /* 2. Single sample */
    {
        int16_t one[1] = {12345};
        roundtrip_mono("Single sample", one, 1);
    }

    /* 3. Two samples */
    {
        int16_t two[2] = {-32768, 32767};
        roundtrip_mono("Two samples (min/max)", two, 2);
    }

    /* 4. DC offset */
    {
        int16_t dc[512];
        for (int i = 0; i < 512; i++) dc[i] = 1000;
        roundtrip_mono("DC offset (1000)", dc, 512);
    }

    /* 5. Impulse */
    {
        int16_t impulse[256];
        memset(impulse, 0, sizeof(impulse));
        impulse[0] = 32767;
        roundtrip_mono("Impulse", impulse, 256);
    }

    /* 6. Ramp */
    {
        int16_t ramp[1000];
        for (int i = 0; i < 1000; i++)
            ramp[i] = (int16_t)((i * 65535 / 999) - 32768);
        roundtrip_mono("Ramp (-32768..32767)", ramp, 1000);
    }

    /* 7. Sine wave (440 Hz @ 44100) */
    {
        int n = 44100;
        int16_t *sine = malloc(n * sizeof(int16_t));
        for (int i = 0; i < n; i++)
            sine[i] = (int16_t)(32000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
        roundtrip_mono("Sine 440Hz (1 sec)", sine, n);
        free(sine);
    }

    /* 8. Noisy signal (pseudo-random) */
    {
        int n = 8192;
        int16_t *noise = malloc(n * sizeof(int16_t));
        uint32_t seed = 0xDEADBEEF;
        for (int i = 0; i < n; i++) {
            seed = seed * 1664525 + 1013904223;
            noise[i] = (int16_t)(seed >> 16);
        }
        roundtrip_mono("Pseudo-random noise (8192)", noise, n);
        free(noise);
    }

    /* 9. Alternating extremes */
    {
        int16_t alt[256];
        for (int i = 0; i < 256; i++)
            alt[i] = (i & 1) ? 32767 : -32768;
        roundtrip_mono("Alternating extremes", alt, 256);
    }

    /* 10. Realistic: short drum hit envelope */
    {
        int n = 4410;  /* 100ms at 44100 */
        int16_t *drum = malloc(n * sizeof(int16_t));
        uint32_t seed = 42;
        for (int i = 0; i < n; i++) {
            double env = exp(-5.0 * i / n);
            seed = seed * 1664525 + 1013904223;
            double noise = ((int32_t)(seed >> 16)) / 32768.0;
            drum[i] = (int16_t)(env * noise * 30000.0);
        }
        roundtrip_mono("Drum envelope (100ms)", drum, n);
        free(drum);
    }

    /* --- Stereo tests --- */
    printf("\nStereo tests:\n");

    /* 11. Stereo silence */
    {
        int16_t silence[2048];
        memset(silence, 0, sizeof(silence));
        roundtrip_stereo("Stereo silence (1024)", silence, 1024);
    }

    /* 12. Stereo identical channels */
    {
        int n = 1000;
        int16_t *stereo = malloc(n * 2 * sizeof(int16_t));
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(16000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
            stereo[i * 2] = v;
            stereo[i * 2 + 1] = v;
        }
        roundtrip_stereo("Stereo identical (1000)", stereo, n);
        free(stereo);
    }

    /* 13. Stereo different channels */
    {
        int n = 4410;
        int16_t *stereo = malloc(n * 2 * sizeof(int16_t));
        for (int i = 0; i < n; i++) {
            stereo[i * 2]     = (int16_t)(20000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
            stereo[i * 2 + 1] = (int16_t)(15000.0 * sin(2.0 * M_PI * 554.0 * i / 44100.0));
        }
        roundtrip_stereo("Stereo diff freq (4410)", stereo, n);
        free(stereo);
    }

    /* 14. Stereo single frame */
    {
        int16_t one[2] = {-12345, 6789};
        roundtrip_stereo("Stereo single frame", one, 1);
    }

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
