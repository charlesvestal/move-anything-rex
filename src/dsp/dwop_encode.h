/*
 * DWOP Encoder (Delta Width Optimized Predictor)
 *
 * Encodes PCM audio into DWOP-compressed bitstream as used in REX2 files.
 * Exact inverse of the decoder in dwop.c — same predictor, same range coder,
 * same energy tracking, producing bit-exact round-trip results.
 *
 * License: MIT
 */

#ifndef DWOP_ENCODE_H
#define DWOP_ENCODE_H

#include <stdint.h>
#include <stddef.h>

/* Encoder state */
typedef struct {
    /* Bit writer */
    uint8_t *data;       /* output buffer (caller-allocated or growable) */
    int data_cap;        /* buffer capacity in bytes */
    int byte_pos;        /* current write position */
    int bit_pos;         /* bits written in current byte (0-7) */
    uint8_t cur;         /* byte being assembled */

    /* Predictor state (doubled representation, identical to decoder) */
    int32_t S[5];
    int32_t e[5];        /* energy trackers (init 2560 each) */

    /* Range coder state */
    uint32_t rv;         /* range value (init 2) */
    int ba;              /* bits accumulated (init 0) */
} dwop_enc_state_t;

/* Initialize encoder state. buf/buf_cap is the output buffer. */
void dwop_enc_init(dwop_enc_state_t *st, uint8_t *buf, int buf_cap);

/* Encode mono PCM samples into DWOP bitstream.
 * in_shift: left-shift applied to input (1 for 16-bit, 9 for 24-bit).
 * Returns number of samples encoded (== num_samples on success, 0 on error). */
int dwop_encode(dwop_enc_state_t *st, const int16_t *pcm, int num_samples,
                int in_shift);

/* Flush any partial byte in the bit writer. Call after encoding is complete.
 * Returns total bytes written. */
int dwop_enc_flush(dwop_enc_state_t *st);

/* Encode stereo PCM (interleaved L/R) into DWOP bitstream.
 * Uses L/delta encoding: L channel encoded directly, R channel encodes (R-L).
 * buf/buf_cap: output buffer. in_shift: 1 for 16-bit, 9 for 24-bit.
 * Returns total bytes written, or 0 on error. */
int dwop_encode_stereo(const int16_t *pcm, int num_frames,
                       uint8_t *buf, int buf_cap, int in_shift);

#endif /* DWOP_ENCODE_H */
