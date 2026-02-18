/*
 * DWOP Decoder (Delta Width Optimized Predictor)
 *
 * Decodes DWOP-compressed audio as used in Propellerhead REX2 files.
 * Reverse-engineered from the REX Shared Library binary.
 *
 * Algorithm:
 *   1. Select predictor with minimum energy (5 adaptive predictors, orders 0-4)
 *   2. Compute quantizer step from minimum energy
 *   3. Read unary-coded quotient (0-bits until 1-bit, step doubles every 7 zeros)
 *   4. Read remainder via adaptive range coder
 *   5. Apply DWOP zigzag to get doubled delta: d = val ^ -(val & 1)
 *   6. Update predictor state and energy trackers
 *   7. Output = S[0] >> 1 (un-double)
 *
 * License: MIT
 */

#include "dwop.h"
#include <string.h>

/* Predictor case mapping: energy index -> prediction order.
 * Energy index 0 -> case 0 (order 0: raw sample)
 * Energy index 1 -> case 1 (order 1: 1st difference)
 * Energy index 2 -> case 4 (order 2: 2nd difference)
 * Energy index 3 -> case 2 (order 3: 3rd difference)
 * Energy index 4 -> case 3 (order 4: 4th difference) */
static const int PRED_MAP[5] = {0, 1, 4, 2, 3};

#define DWOP_ENERGY_INIT  2560
#define DWOP_MAX_UNARY    50000

/* --- Bit reader (MSB first, byte-by-byte) --- */

static inline int br_bit(dwop_state_t *st)
{
    if (!st->bit_pos) {
        if (st->byte_pos >= st->data_len)
            return 0;
        st->cur = st->data[st->byte_pos++];
        st->bit_pos = 8;
    }
    st->bit_pos--;
    return (st->cur >> st->bit_pos) & 1;
}

static inline uint32_t br_bits(dwop_state_t *st, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)br_bit(st);
    return v;
}

/* --- Public API --- */

void dwop_init(dwop_state_t *state, const uint8_t *data, int data_len)
{
    memset(state, 0, sizeof(*state));
    state->data = data;
    state->data_len = data_len;
    state->rv = 2;
    for (int i = 0; i < 5; i++)
        state->e[i] = DWOP_ENERGY_INIT;
}

int dwop_decode(dwop_state_t *state, int16_t *out, int max_samples)
{
    int n;

    for (n = 0; n < max_samples; n++) {
        /* 1. Find predictor with minimum energy */
        uint32_t min_e = (uint32_t)state->e[0];
        int p_order = 0;
        for (int i = 1; i < 5; i++) {
            if ((uint32_t)state->e[i] < min_e) {
                min_e = (uint32_t)state->e[i];
                p_order = i;
            }
        }

        /* 2. Quantizer step */
        uint32_t step = (min_e * 3 + 0x24) >> 7;

        /* 3. Unary-coded quotient */
        uint32_t acc = 0, cs = step;
        int qc = 7, uc = 0;
        for (;;) {
            if (br_bit(state) == 1)
                break;
            acc += cs;
            if (--qc == 0) {
                cs <<= 2;
                qc = 7;
            }
            if (++uc > DWOP_MAX_UNARY)
                return n;  /* safety bail */
        }

        /* 4. Range coder for remainder */
        int nb = state->ba;
        if (cs >= state->rv) {
            while (cs >= state->rv) {
                state->rv <<= 1;
                if (!state->rv)
                    return n;
                nb++;
            }
        } else {
            nb++;
            uint32_t t = state->rv;
            for (;;) {
                state->rv = t;
                t >>= 1;
                nb--;
                if (cs >= t)
                    break;
            }
        }

        uint32_t ext = (nb > 0) ? br_bits(state, nb) : 0;
        uint32_t co = state->rv - cs;
        uint32_t rem;
        if (ext < co) {
            rem = ext;
        } else {
            int x = br_bit(state);
            rem = co + (ext - co) * 2 + (uint32_t)x;
        }

        uint32_t val = acc + rem;
        state->ba = nb;

        /* 5. DWOP zigzag: produces doubled delta */
        int32_t d = (int32_t)(val ^ (uint32_t)(-(int32_t)(val & 1)));

        /* 6. Predictor update */
        int32_t o[5];
        memcpy(o, state->S, sizeof(o));

        switch (PRED_MAP[p_order]) {
        case 0: /* Order 0: d is the sample (doubled) */
            state->S[0] = d;
            state->S[1] = d - o[0];
            state->S[2] = state->S[1] - o[1];
            state->S[3] = state->S[2] - o[2];
            state->S[4] = state->S[3] - o[3];
            break;
        case 1: /* Order 1: d is 1st difference (doubled) */
            state->S[0] = o[0] + d;
            state->S[1] = d;
            state->S[2] = d - o[1];
            state->S[3] = state->S[2] - o[2];
            state->S[4] = state->S[3] - o[3];
            break;
        case 4: /* Order 2: d is 2nd difference (doubled) */
            state->S[1] = o[1] + d;
            state->S[0] = o[0] + state->S[1];
            state->S[2] = d;
            state->S[3] = d - o[2];
            state->S[4] = state->S[3] - o[3];
            break;
        case 2: /* Order 3: d is 3rd difference (doubled) */
            state->S[2] = o[2] + d;
            state->S[1] = o[1] + state->S[2];
            state->S[0] = o[0] + state->S[1];
            state->S[3] = d;
            state->S[4] = d - o[3];
            break;
        case 3: /* Order 4: d is 4th difference (doubled) */
            state->S[3] = o[3] + d;
            state->S[2] = o[2] + state->S[3];
            state->S[1] = o[1] + state->S[2];
            state->S[0] = o[0] + state->S[1];
            state->S[4] = d;
            break;
        }

        /* Energy update: cheap abs = S ^ (S >> 31) */
        for (int i = 0; i < 5; i++) {
            int32_t as = state->S[i] ^ (state->S[i] >> 31);
            state->e[i] = state->e[i] + as - (int32_t)((uint32_t)state->e[i] >> 5);
        }

        /* 7. Output: un-double via arithmetic right shift */
        out[n] = (int16_t)(state->S[0] >> 1);
    }

    return n;
}
