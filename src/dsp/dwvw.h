/*
 * DWVW Decoder (Delta Width Variable Word)
 *
 * Decodes DWVW-compressed audio as used in Propellerhead REX2 files.
 * Algorithm matches libsndfile's implementation.
 *
 * License: MIT
 */

#ifndef DWVW_H
#define DWVW_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    /* Input data */
    const uint8_t *data;
    size_t data_len;
    size_t byte_pos;

    /* Bit reservoir (matches libsndfile's int type) */
    int bits;
    int bit_count;

    /* Decoder parameters */
    int bit_width;       /* encoding bit width (16 for REX2) */
    int max_delta;       /* 1 << (bit_width - 1) */
    int span;            /* 1 << bit_width */
    int dwm_maxsize;     /* bit_width / 2 */

    /* Persistent state between decode calls */
    int last_delta_width;
    int last_sample;
    int samples_decoded;
} dwvw_state_t;

/* Initialize decoder for given bit width */
void dwvw_init(dwvw_state_t *state, const uint8_t *data, size_t data_len, int bit_width);

/* Decode up to max_samples into out buffer (16-bit PCM).
 * Returns number of samples actually decoded. */
int dwvw_decode(dwvw_state_t *state, int16_t *out, int max_samples);

#endif /* DWVW_H */
