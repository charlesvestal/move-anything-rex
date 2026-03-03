/*
 * DWOP Encoder (Delta Width Optimized Predictor)
 *
 * Encodes PCM audio into DWOP-compressed bitstream as used in REX2 files.
 * This is the exact inverse of the decoder in dwop.c:
 *   - Same 5 adaptive predictors with energy-based selection
 *   - Same predictor update and energy tracking
 *   - Same range coder adaptation
 *   - Same zigzag encoding
 *   - Bit writer is the inverse of the bit reader
 *
 * License: MIT
 */

#include "dwop_encode.h"
#include <string.h>

/* Must match decoder exactly */
static const int PRED_MAP[5] = {0, 1, 4, 2, 3};

#define DWOP_ENERGY_INIT  2560

/* --- Bit writer (MSB first, inverse of decoder's bit reader) --- */

static inline int bw_bit(dwop_enc_state_t *st, int bit)
{
    st->cur = (st->cur << 1) | (bit & 1);
    st->bit_pos++;
    if (st->bit_pos == 8) {
        if (st->byte_pos >= st->data_cap)
            return -1;  /* buffer full */
        st->data[st->byte_pos++] = st->cur;
        st->cur = 0;
        st->bit_pos = 0;
    }
    return 0;
}

static inline int bw_bits(dwop_enc_state_t *st, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (bw_bit(st, (int)((val >> i) & 1)) < 0)
            return -1;
    }
    return 0;
}

/* --- Public API --- */

void dwop_enc_init(dwop_enc_state_t *st, uint8_t *buf, int buf_cap)
{
    memset(st, 0, sizeof(*st));
    st->data = buf;
    st->data_cap = buf_cap;
    st->rv = 2;
    for (int i = 0; i < 5; i++)
        st->e[i] = DWOP_ENERGY_INIT;
}

int dwop_enc_flush(dwop_enc_state_t *st)
{
    if (st->bit_pos > 0) {
        /* Pad remaining bits with zeros (shift left to fill byte) */
        st->cur <<= (8 - st->bit_pos);
        if (st->byte_pos >= st->data_cap)
            return 0;
        st->data[st->byte_pos++] = st->cur;
        st->cur = 0;
        st->bit_pos = 0;
    }
    return st->byte_pos;
}

int dwop_encode(dwop_enc_state_t *st, const int16_t *pcm, int num_samples,
                int in_shift)
{
    for (int n = 0; n < num_samples; n++) {
        /* Double the input (matches decoder's doubled representation) */
        int32_t doubled = (int32_t)pcm[n] << in_shift;

        /* 1. Find predictor with minimum energy (identical to decoder) */
        uint32_t min_e = (uint32_t)st->e[0];
        int p_order = 0;
        for (int i = 1; i < 5; i++) {
            if ((uint32_t)st->e[i] < min_e) {
                min_e = (uint32_t)st->e[i];
                p_order = i;
            }
        }

        /* 2. Quantizer step (identical to decoder) */
        uint32_t step = (min_e * 3 + 0x24) >> 7;

        /* 3. Compute delta d: given desired S[0] = doubled, derive d for
         *    the selected predictor case. This is the inverse of the
         *    predictor update in the decoder. */
        int32_t d;
        switch (PRED_MAP[p_order]) {
        case 0: /* Order 0: d IS the sample (doubled) */
            d = doubled;
            break;
        case 1: /* Order 1: d is 1st difference */
            d = doubled - st->S[0];
            break;
        case 4: /* Order 2: d is 2nd difference */
            d = doubled - st->S[0] - st->S[1];
            break;
        case 2: /* Order 3: d is 3rd difference */
            d = doubled - st->S[0] - st->S[1] - st->S[2];
            break;
        case 3: /* Order 4: d is 4th difference */
            d = doubled - st->S[0] - st->S[1] - st->S[2] - st->S[3];
            break;
        default:
            d = doubled;
            break;
        }

        /* 4. Zigzag encode (inverse of decoder's zigzag decode) */
        uint32_t val;
        if (d >= 0)
            val = (uint32_t)d;        /* 0→0, 2→2, 4→4, ... */
        else
            val = (uint32_t)(-d) - 1; /* -2→1, -4→3, -6→5, ... */

        /* 5. Unary + range code encoding
         *    Walk the unary loop: accumulate cs (step quadruples every 7 zeros)
         *    until val - acc < cs. Write N zero-bits + 1 terminator bit.
         *    Then encode the remainder via range coder. */
        uint32_t acc = 0, cs = step;
        int qc = 7;
        int unary_count = 0;

        /* Count how many unary zeros we need */
        while (val - acc >= cs) {
            acc += cs;
            unary_count++;
            if (--qc == 0) {
                cs <<= 2;
                qc = 7;
            }
        }

        /* Write unary: zeros then terminator */
        for (int i = 0; i < unary_count; i++) {
            if (bw_bit(st, 0) < 0)
                return 0;
        }
        if (bw_bit(st, 1) < 0)
            return 0;

        /* 6. Range coder adaptation (identical to decoder) */
        int nb = st->ba;
        if (cs >= st->rv) {
            while (cs >= st->rv) {
                st->rv <<= 1;
                if (!st->rv)
                    return 0;
                nb++;
            }
        } else {
            nb++;
            uint32_t t = st->rv;
            for (;;) {
                st->rv = t;
                t >>= 1;
                nb--;
                if (cs >= t)
                    break;
            }
        }

        uint32_t co = st->rv - cs;
        uint32_t rem = val - acc;

        /* Encode remainder (inverse of decoder's read) */
        if (rem < co) {
            /* rem fits in nb bits */
            if (nb > 0) {
                if (bw_bits(st, rem, nb) < 0)
                    return 0;
            }
        } else {
            /* Need nb bits + 1 extra bit */
            uint32_t ext = co + ((rem - co) >> 1);
            int x = (int)((rem - co) & 1);
            if (nb > 0) {
                if (bw_bits(st, ext, nb) < 0)
                    return 0;
            }
            if (bw_bit(st, x) < 0)
                return 0;
        }

        st->ba = nb;

        /* 7. Predictor update (identical to decoder) */
        int32_t o[5];
        memcpy(o, st->S, sizeof(o));

        switch (PRED_MAP[p_order]) {
        case 0:
            st->S[0] = d;
            st->S[1] = d - o[0];
            st->S[2] = st->S[1] - o[1];
            st->S[3] = st->S[2] - o[2];
            st->S[4] = st->S[3] - o[3];
            break;
        case 1:
            st->S[0] = o[0] + d;
            st->S[1] = d;
            st->S[2] = d - o[1];
            st->S[3] = st->S[2] - o[2];
            st->S[4] = st->S[3] - o[3];
            break;
        case 4:
            st->S[1] = o[1] + d;
            st->S[0] = o[0] + st->S[1];
            st->S[2] = d;
            st->S[3] = d - o[2];
            st->S[4] = st->S[3] - o[3];
            break;
        case 2:
            st->S[2] = o[2] + d;
            st->S[1] = o[1] + st->S[2];
            st->S[0] = o[0] + st->S[1];
            st->S[3] = d;
            st->S[4] = d - o[3];
            break;
        case 3:
            st->S[3] = o[3] + d;
            st->S[2] = o[2] + st->S[3];
            st->S[1] = o[1] + st->S[2];
            st->S[0] = o[0] + st->S[1];
            st->S[4] = d;
            break;
        }

        /* 8. Energy update (identical to decoder: cheap abs) */
        for (int i = 0; i < 5; i++) {
            int32_t as = st->S[i] ^ (st->S[i] >> 31);
            st->e[i] = st->e[i] + as - (int32_t)((uint32_t)st->e[i] >> 5);
        }
    }

    return num_samples;
}

/* --- Stereo encoder --- */

/* Internal stereo channel state (matches decoder's dwop_ch_t) */
typedef struct {
    int32_t S[5];
    int32_t e[5];
    uint32_t rv;
    int ba;
} enc_ch_t;

static void enc_ch_init(enc_ch_t *c)
{
    memset(c->S, 0, sizeof(c->S));
    for (int i = 0; i < 5; i++)
        c->e[i] = DWOP_ENERGY_INIT;
    c->rv = 2;
    c->ba = 0;
}

/* Encode one sample through a channel state into the shared bit writer.
 * doubled: the pre-doubled value to encode (sample << in_shift). */
static int stereo_encode_one(enc_ch_t *ch, dwop_enc_state_t *bw, int32_t doubled)
{
    /* 1. Find predictor with minimum energy */
    uint32_t min_e = (uint32_t)ch->e[0];
    int p_order = 0;
    for (int i = 1; i < 5; i++) {
        if ((uint32_t)ch->e[i] < min_e) {
            min_e = (uint32_t)ch->e[i];
            p_order = i;
        }
    }

    /* 2. Quantizer step */
    uint32_t step = (min_e * 3 + 0x24) >> 7;

    /* 3. Compute delta */
    int32_t d;
    switch (PRED_MAP[p_order]) {
    case 0: d = doubled; break;
    case 1: d = doubled - ch->S[0]; break;
    case 4: d = doubled - ch->S[0] - ch->S[1]; break;
    case 2: d = doubled - ch->S[0] - ch->S[1] - ch->S[2]; break;
    case 3: d = doubled - ch->S[0] - ch->S[1] - ch->S[2] - ch->S[3]; break;
    default: d = doubled; break;
    }

    /* 4. Zigzag encode */
    uint32_t val;
    if (d >= 0)
        val = (uint32_t)d;
    else
        val = (uint32_t)(-d) - 1;

    /* 5. Unary coding */
    uint32_t acc = 0, cs = step;
    int qc = 7;
    int unary_count = 0;

    while (val - acc >= cs) {
        acc += cs;
        unary_count++;
        if (--qc == 0) {
            cs <<= 2;
            qc = 7;
        }
    }

    for (int i = 0; i < unary_count; i++) {
        if (bw_bit(bw, 0) < 0) return -1;
    }
    if (bw_bit(bw, 1) < 0) return -1;

    /* 6. Range coder */
    int nb = ch->ba;
    if (cs >= ch->rv) {
        while (cs >= ch->rv) {
            ch->rv <<= 1;
            if (!ch->rv) return -1;
            nb++;
        }
    } else {
        nb++;
        uint32_t t = ch->rv;
        for (;;) {
            ch->rv = t;
            t >>= 1;
            nb--;
            if (cs >= t) break;
        }
    }

    uint32_t co = ch->rv - cs;
    uint32_t rem = val - acc;

    if (rem < co) {
        if (nb > 0) {
            if (bw_bits(bw, rem, nb) < 0) return -1;
        }
    } else {
        uint32_t ext = co + ((rem - co) >> 1);
        int x = (int)((rem - co) & 1);
        if (nb > 0) {
            if (bw_bits(bw, ext, nb) < 0) return -1;
        }
        if (bw_bit(bw, x) < 0) return -1;
    }

    ch->ba = nb;

    /* 7. Predictor update */
    int32_t o[5];
    memcpy(o, ch->S, sizeof(o));

    switch (PRED_MAP[p_order]) {
    case 0:
        ch->S[0] = d;
        ch->S[1] = d - o[0];
        ch->S[2] = ch->S[1] - o[1];
        ch->S[3] = ch->S[2] - o[2];
        ch->S[4] = ch->S[3] - o[3];
        break;
    case 1:
        ch->S[0] = o[0] + d;
        ch->S[1] = d;
        ch->S[2] = d - o[1];
        ch->S[3] = ch->S[2] - o[2];
        ch->S[4] = ch->S[3] - o[3];
        break;
    case 4:
        ch->S[1] = o[1] + d;
        ch->S[0] = o[0] + ch->S[1];
        ch->S[2] = d;
        ch->S[3] = d - o[2];
        ch->S[4] = ch->S[3] - o[3];
        break;
    case 2:
        ch->S[2] = o[2] + d;
        ch->S[1] = o[1] + ch->S[2];
        ch->S[0] = o[0] + ch->S[1];
        ch->S[3] = d;
        ch->S[4] = d - o[3];
        break;
    case 3:
        ch->S[3] = o[3] + d;
        ch->S[2] = o[2] + ch->S[3];
        ch->S[1] = o[1] + ch->S[2];
        ch->S[0] = o[0] + ch->S[1];
        ch->S[4] = d;
        break;
    }

    /* 8. Energy update */
    for (int i = 0; i < 5; i++) {
        int32_t as = ch->S[i] ^ (ch->S[i] >> 31);
        ch->e[i] = ch->e[i] + as - (int32_t)((uint32_t)ch->e[i] >> 5);
    }

    return 0;
}

int dwop_encode_stereo(const int16_t *pcm, int num_frames,
                       uint8_t *buf, int buf_cap, int in_shift)
{
    dwop_enc_state_t bw;
    dwop_enc_init(&bw, buf, buf_cap);

    enc_ch_t L, R;
    enc_ch_init(&L);
    enc_ch_init(&R);

    /* For stereo, the decoder applies an extra_shift = in_shift - 1 on top of
     * the un-doubling that stereo_decode_one does (S[0] >> 1). To round-trip
     * correctly, the encoder must produce the full-precision doubled value
     * that the decoder's channel state operates on. The decoder's
     * stereo_decode_one returns S[0] >> 1 (un-doubled), and the outer loop
     * applies >> extra_shift. So the final output is S[0] >> in_shift.
     * We need S[0] = sample << in_shift, which means the doubled input to
     * the channel state is sample << in_shift. */

    for (int n = 0; n < num_frames; n++) {
        int32_t l_sample = pcm[n * 2];
        int32_t r_sample = pcm[n * 2 + 1];
        int32_t l_doubled = l_sample << in_shift;

        /* R channel encodes the difference (R - L) at full precision.
         * The decoder reconstructs: r_val = l_val + r_delta where
         * l_val = L.S[0] >> 1 and r_delta = R.S[0] >> 1.
         * So r_val = (L.S[0] + R.S[0]) >> 1 and final output is
         * r_val >> extra_shift = (L.S[0] + R.S[0]) >> in_shift.
         * We need L.S[0] + R.S[0] = r_sample << in_shift
         * So R.S[0] = r_sample << in_shift - L.S[0] = (r_sample - l_sample) << in_shift */
        int32_t r_delta_doubled = (r_sample - l_sample) << in_shift;

        if (stereo_encode_one(&L, &bw, l_doubled) < 0)
            return 0;
        if (stereo_encode_one(&R, &bw, r_delta_doubled) < 0)
            return 0;
    }

    return dwop_enc_flush(&bw);
}
