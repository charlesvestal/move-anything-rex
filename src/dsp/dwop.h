/*
 * DWOP Decoder (Delta Width Optimized Predictor)
 *
 * Decodes DWOP-compressed audio as used in Propellerhead REX2 files.
 * 5-predictor adaptive lossless codec with energy-based predictor selection.
 *
 * License: MIT
 */

#ifndef DWOP_H
#define DWOP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    /* Bit reader */
    const uint8_t *data;
    int data_len;
    int byte_pos;
    int bit_pos;     /* bits remaining in current byte (0 = need new byte) */
    uint8_t cur;     /* current byte being read */

    /* Predictor state (doubled representation) */
    int32_t S[5];    /* S[0]=sample*2, S[1]=1st_diff*2, ..., S[4]=4th_diff*2 */
    int32_t e[5];    /* energy trackers */

    /* Range coder state */
    uint32_t rv;
    int ba;
} dwop_state_t;

/* Initialize DWOP decoder state */
void dwop_init(dwop_state_t *state, const uint8_t *data, int data_len);

/* Decode up to max_samples into out buffer (16-bit PCM).
 * Returns number of samples actually decoded. */
int dwop_decode(dwop_state_t *state, int16_t *out, int max_samples);

#endif /* DWOP_H */
