/*
 * DWVW Decoder (Delta Width Variable Word)
 *
 * Implementation matching the libsndfile algorithm for decoding
 * DWVW-compressed audio used in Propellerhead REX2 files.
 *
 * Key algorithm:
 *   1. Read unary-coded delta width modifier (pre-load dwm_maxsize bits,
 *      count zeros; caps at dwm_maxsize WITHOUT requiring a terminating 1)
 *   2. If modifier != 0, read 1 sign bit (1 = negative)
 *   3. delta_width = (delta_width + modifier + bit_width) % bit_width
 *   4. If delta_width > 0:
 *      a. Read (delta_width - 1) magnitude bits, OR in implicit leading 1
 *      b. Read 1 sign bit for delta
 *      c. If magnitude == max_delta - 1, read extra bit and add to delta
 *      d. Apply sign
 *   5. sample += delta, wrap to [-max_delta, max_delta)
 *   6. Output left-shifted to fill int32, then convert to int16
 *
 * License: MIT
 */

#include "dwvw.h"
#include <string.h>

void dwvw_init(dwvw_state_t *state, const uint8_t *data, size_t data_len, int bit_width)
{
    memset(state, 0, sizeof(*state));
    state->data = data;
    state->data_len = data_len;
    state->byte_pos = 0;
    state->bits = 0;
    state->bit_count = 0;
    state->bit_width = bit_width;
    state->max_delta = 1 << (bit_width - 1);
    state->span = 1 << bit_width;
    state->dwm_maxsize = bit_width / 2;
    state->last_delta_width = 0;
    state->last_sample = 0;
    state->samples_decoded = 0;
}

/*
 * Load bits into the reservoir and optionally extract a value.
 *
 * If bit_count > 0: load enough bits, return the top bit_count bits.
 * If bit_count == -1: load dwm_maxsize bits, then scan for the unary
 *   delta_width_modifier (count zeros until a 1 or dwm_maxsize reached).
 *
 * Returns the extracted value, or -1 on end of stream.
 *
 * This closely follows libsndfile's dwvw_decode_load_bits().
 */
static int dwvw_load_bits(dwvw_state_t *state, int bit_count)
{
    int output = 0;
    int get_dwm = 0;

    if (bit_count < 0) {
        get_dwm = 1;
        bit_count = state->dwm_maxsize;
    }

    /* Load bytes into bit reservoir until we have enough */
    while (state->bit_count < bit_count) {
        if (state->byte_pos >= state->data_len) {
            /* End of input */
            if (bit_count < 8)
                return -1;
            /* Pad with zero byte */
            state->bits = state->bits << 8;
            state->bit_count += 8;
            continue;
        }

        state->bits = (state->bits << 8) | state->data[state->byte_pos];
        state->byte_pos++;
        state->bit_count += 8;
    }

    /* If asked to get a fixed number of bits, extract and return them */
    if (!get_dwm) {
        output = (state->bits >> (state->bit_count - bit_count)) & ((1 << bit_count) - 1);
        state->bit_count -= bit_count;
        return output;
    }

    /* Otherwise, decode the unary delta_width_modifier from the pre-loaded bits.
     * Count consecutive 0 bits. Stop when we hit a 1 bit or reach dwm_maxsize.
     * When we reach dwm_maxsize, we do NOT consume a terminating 1 bit. */
    while (output < state->dwm_maxsize) {
        state->bit_count -= 1;
        if (state->bits & (1 << state->bit_count))
            break;
        output += 1;
    }

    return output;
}

int dwvw_decode(dwvw_state_t *state, int16_t *out, int max_samples)
{
    int count;
    int delta_width_modifier, delta_width, delta_negative, delta, sample;

    /* Hard cap to prevent runaway decodes */
    if (max_samples > 10000000) max_samples = 10000000;

    /* Restore state */
    delta_width = state->last_delta_width;
    sample = state->last_sample;

    for (count = 0; count < max_samples; count++) {
        /* Get the delta_width_modifier (unary coded) */
        delta_width_modifier = dwvw_load_bits(state, -1);

        /* Check for end of input stream */
        if (delta_width_modifier < 0)
            break;

        /* End-of-stream detection: if we've consumed all data and
         * this is the first sample, we're done */
        if (state->byte_pos >= state->data_len && state->bit_count == 0 && count == 0)
            break;

        /* Read sign bit for modifier if non-zero */
        if (delta_width_modifier && dwvw_load_bits(state, 1))
            delta_width_modifier = -delta_width_modifier;

        /* Calculate the current delta width */
        delta_width = (delta_width + delta_width_modifier + state->bit_width) % state->bit_width;

        /* Load the delta */
        delta = 0;
        if (delta_width) {
            /* Read (delta_width - 1) magnitude bits with implicit leading 1 */
            delta = dwvw_load_bits(state, delta_width - 1) | (1 << (delta_width - 1));

            /* Read sign bit */
            delta_negative = dwvw_load_bits(state, 1);

            /* Check for max_delta - 1 boundary case: read extra bit */
            if (delta == state->max_delta - 1)
                delta += dwvw_load_bits(state, 1);

            /* Apply sign AFTER the extra bit check */
            if (delta_negative)
                delta = -delta;
        }

        /* Accumulate into running sample */
        sample += delta;

        /* Wrap to valid range */
        if (sample >= state->max_delta)
            sample -= state->span;
        else if (sample < -state->max_delta)
            sample += state->span;

        /* Output: left-shift to fill 32-bit range, then truncate to 16-bit.
         * libsndfile does: arith_shift_left(sample, 32 - bit_width)
         * which for bit_width=16 is shift by 16. Then the top 16 bits = sample itself.
         * For bit_width=12, shift by 20, top 16 bits = sample << 4.
         * We output int16, so we take the top 16 bits of the 32-bit value. */
        out[count] = (int16_t)(sample << (16 - state->bit_width));

        /* End-of-stream: if all data consumed and no bits remaining */
        if (state->byte_pos >= state->data_len && state->bit_count == 0)
            break;
    }

    /* Save state */
    state->last_delta_width = delta_width;
    state->last_sample = sample;
    state->samples_decoded += count;

    return count;
}
