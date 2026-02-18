/*
 * DWOP Codec Decoder Test - Consolidated from /tmp/dwop_*.c experiments
 *
 * Tests multiple DWOP decoder variants against SDK reference data.
 * Reads SDAT (compressed) and reference int16 PCM from binary files.
 *
 * SAFETY GUARDRAILS:
 *   - Max 2000 samples decoded per variant (enough for debugging, not enough to hang)
 *   - Divergence detection: aborts if |S[0]| > 1M (exponential blowup)
 *   - Unary overflow cap: 5000 bits max
 *   - Max 50MB input files
 *   - Bounded output: only prints first 50 samples of comparison
 *
 * Known state (as of 2025-02-18):
 *   - Sample 314 in the 2x-zigzag+shift variant is the alignment point
 *   - decoded[314] = -171, ref[0] = -342  (factor of 2 off)
 *   - Predictor diverges exponentially after first matching sample
 *   - The >>1 output shift produces half the expected value
 *   - No variant has achieved sustained match beyond sample 0
 *
 * Reference files (created by SDK-based tools):
 *   /tmp/rex_analysis_sdat.bin  - Raw SDAT chunk (118304 bytes)
 *   /tmp/rex_slice0_int16.bin   - First 1000 samples of slice 0 (int16 LE)
 *   /tmp/rex_combined_int16.bin - All decoded samples (89200 int16 LE)
 *
 * Build:
 *   cc -O2 -o test_dwop test/test_dwop.c -lm
 *
 * Usage:
 *   ./test_dwop [sdat_file] [ref_file]
 *   (defaults to /tmp/rex_analysis_sdat.bin and /tmp/rex_slice0_int16.bin)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ================================================================ */
/* Safety limits                                                     */
/* ================================================================ */

#define MAX_DECODE_SAMPLES   2000    /* Per variant - enough for debugging */
#define MAX_REF_SAMPLES      2000    /* Reference comparison window */
#define MAX_INPUT_SIZE       (50*1024*1024)
#define DIVERGENCE_THRESHOLD 1000000 /* Abort if |S[0]| exceeds this */
#define MAX_UNARY_BITS       5000    /* Cap unary code length */
#define PRINT_SAMPLES        50      /* Max samples to print in comparison */

/* ================================================================ */
/* Bitreader                                                         */
/* ================================================================ */

typedef struct {
    const uint8_t *data;
    int len, pos, bp;
    uint8_t cur;
} BitReader;

static void br_init(BitReader *b, const uint8_t *d, int l) {
    b->data = d; b->len = l; b->pos = 0; b->bp = 0; b->cur = 0;
}

static inline int br_bit(BitReader *b) {
    if (!b->bp) {
        if (b->pos >= b->len) return 0;
        b->cur = b->data[b->pos++];
        b->bp = 8;
    }
    b->bp--;
    return (b->cur >> b->bp) & 1;
}

static inline uint32_t br_bits(BitReader *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | br_bit(b);
    return v;
}

static inline int br_bitpos(BitReader *b) {
    return (b->pos * 8) - b->bp;
}

/* ================================================================ */
/* DWOP Decoder Core                                                 */
/* ================================================================ */

typedef struct {
    /* Configuration */
    int zigzag_2x;        /* 1: d = val^sg (2x), 0: d = (val>>1)^sg (1x) */
    int skip_output_shift; /* 1: output = S[0], 0: output = S[0]>>1 */
    int double_state;     /* 1: S[i] <<= 1 at entry, >>= 1 at exit */
    int energy_on_halved; /* 1: energy uses S[i]>>1 for abs value */
    int halve_d;          /* 1: d = (val^sg)>>1 after zigzag */
    const char *name;
} dwop_config_t;

typedef struct {
    int samples_decoded;
    int diverged_at;      /* -1 if no divergence */
    int first_match_offset; /* offset where first ref sample matches */
    int match_count;        /* how many of first 50 ref samples match */
} dwop_result_t;

static dwop_result_t dwop_decode(const dwop_config_t *cfg,
                                  const uint8_t *sdat, int sdat_len,
                                  int16_t *out, int max_samples,
                                  const int16_t *ref, int ref_len)
{
    dwop_result_t result = {0, -1, -1, 0};

    if (max_samples > MAX_DECODE_SAMPLES) max_samples = MAX_DECODE_SAMPLES;

    BitReader br;
    br_init(&br, sdat, sdat_len);
    int32_t S[5] = {0}, e[5] = {2560, 2560, 2560, 2560, 2560};
    uint32_t rv = 2;
    int ba = 0;

    for (int n = 0; n < max_samples; n++) {
        /* Optional: double S[] at entry */
        if (cfg->double_state) {
            for (int i = 0; i < 5; i++) S[i] <<= 1;
        }

        /* Min energy predictor selection */
        uint32_t me = (uint32_t)e[0];
        int p = 0;
        for (int i = 1; i < 5; i++) {
            uint32_t ei = (uint32_t)e[i];
            if (ei < me) { me = ei; p = i; }
        }
        uint32_t step = (me * 3 + 0x24) >> 7;

        /* Unary coding */
        uint32_t acc = 0, cs = step;
        int qc = 7, uc = 0;
        while (1) {
            if (br_bit(&br) == 1) break;
            acc += cs;
            qc--;
            if (!qc) { cs *= 4; qc = 7; }
            if (++uc > MAX_UNARY_BITS) {
                result.samples_decoded = n;
                result.diverged_at = n;
                return result;
            }
        }

        /* Range coding */
        int nb = ba;
        if (cs >= rv) {
            while (cs >= rv) {
                rv <<= 1;
                if (!rv) { result.samples_decoded = n; result.diverged_at = n; return result; }
                nb++;
            }
        } else {
            nb++;
            uint32_t t = rv;
            while (1) {
                rv = t;
                t >>= 1;
                nb--;
                if (cs >= t) break;
            }
        }

        uint32_t ext = (nb > 0) ? br_bits(&br, nb) : 0;
        uint32_t co = rv - cs, rem;
        if (ext < co) {
            rem = ext;
        } else {
            int x = br_bit(&br);
            rem = co + (ext - co) * 2 + x;
        }

        uint32_t val = acc + rem;
        ba = nb;

        /* Zigzag decode */
        uint32_t sg = -(val & 1);
        int32_t d;
        if (cfg->zigzag_2x) {
            d = (int32_t)(val ^ sg);
        } else {
            d = (int32_t)((val >> 1) ^ sg);
        }

        if (cfg->halve_d) {
            d >>= 1;
        }

        /* Predictor update */
        int32_t o[5];
        memcpy(o, S, sizeof(S));
        switch (p) {
        case 0: S[0]=d; S[1]=d-o[0]; S[2]=S[1]-o[1]; S[3]=S[2]-o[2]; S[4]=S[3]-o[3]; break;
        case 1: S[0]=o[0]+d; S[1]=d; S[2]=d-o[1]; S[3]=S[2]-o[2]; S[4]=S[3]-o[3]; break;
        case 2: S[2]=o[2]+d; S[1]=o[1]+S[2]; S[0]=o[0]+S[1]; S[3]=d; S[4]=d-o[3]; break;
        case 3: S[3]=o[3]+d; S[2]=o[2]+S[3]; S[1]=o[1]+S[2]; S[0]=o[0]+S[1]; S[4]=d; break;
        case 4: S[1]=o[1]+d; S[0]=o[0]+S[1]; S[2]=d; S[3]=d-o[2]; S[4]=S[3]-o[3]; break;
        }

        /* Output */
        if (cfg->skip_output_shift) {
            out[n] = (int16_t)(S[0] & 0xFFFF);
        } else {
            out[n] = (int16_t)((uint32_t)S[0] >> 1);
        }

        /* Divergence detection */
        if (abs(S[0]) > DIVERGENCE_THRESHOLD) {
            result.samples_decoded = n + 1;
            result.diverged_at = n;
            /* Still write the sample so we can see what went wrong */
            break;
        }

        /* Optional: halve S[] at exit */
        if (cfg->double_state) {
            for (int i = 0; i < 5; i++) S[i] >>= 1;
        }

        /* Energy update */
        for (int i = 0; i < 5; i++) {
            int32_t si = cfg->energy_on_halved ? (S[i] >> 1) : S[i];
            int32_t as = si ^ (si >> 31);
            e[i] = e[i] + as - (int32_t)((uint32_t)e[i] >> 5);
        }

        result.samples_decoded = n + 1;
    }

    /* Find best alignment with reference */
    if (ref && ref_len > 0 && result.samples_decoded >= 10) {
        int best_score = 0, best_off = -1;
        int search_limit = result.samples_decoded - 10;
        if (search_limit > 500) search_limit = 500; /* Don't search too far */

        for (int off = 0; off < search_limit; off++) {
            int sc = 0;
            for (int i = 0; i < 10 && i < ref_len; i++) {
                if (abs(out[off + i] - ref[i]) <= 2) sc++;
            }
            if (sc > best_score) { best_score = sc; best_off = off; }
        }
        result.first_match_offset = best_off;
        result.match_count = best_score;
    }

    return result;
}

/* ================================================================ */
/* Main                                                              */
/* ================================================================ */

static uint8_t *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > MAX_INPUT_SIZE) { fclose(f); return NULL; }
    uint8_t *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    *out_size = sz;
    return buf;
}

int main(int argc, char *argv[])
{
    const char *sdat_path = (argc > 1) ? argv[1] : "/tmp/rex_analysis_sdat.bin";
    const char *ref_path  = (argc > 2) ? argv[2] : "/tmp/rex_slice0_int16.bin";

    /* Load SDAT */
    long sdat_size;
    uint8_t *sdat = read_file(sdat_path, &sdat_size);
    if (!sdat) {
        fprintf(stderr, "Cannot read SDAT from %s\n", sdat_path);
        return 1;
    }
    printf("SDAT: %ld bytes from %s\n", sdat_size, sdat_path);

    /* Load reference */
    long ref_size;
    uint8_t *ref_raw = read_file(ref_path, &ref_size);
    int ref_len = 0;
    int16_t *ref = NULL;
    if (ref_raw) {
        ref_len = (int)(ref_size / 2);
        if (ref_len > MAX_REF_SAMPLES) ref_len = MAX_REF_SAMPLES;
        ref = (int16_t *)ref_raw;
        printf("Reference: %d samples from %s\n", ref_len, ref_path);
        printf("  First 10: %d %d %d %d %d %d %d %d %d %d\n",
               ref[0], ref[1], ref[2], ref[3], ref[4],
               ref[5], ref[6], ref[7], ref[8], ref[9]);
    } else {
        printf("No reference file at %s (comparison disabled)\n", ref_path);
    }

    /* Define decoder variants to test */
    dwop_config_t variants[] = {
        {1, 0, 0, 0, 0, "2x_zigzag + shift"},         /* Original best: offset 314, factor-2 error */
        {1, 1, 0, 0, 0, "2x_zigzag + no_shift"},       /* Skip output >>1 */
        {0, 1, 0, 0, 0, "1x_zigzag + no_shift"},       /* Standard zigzag, no shift */
        {0, 0, 0, 0, 0, "1x_zigzag + shift"},           /* Standard zigzag with shift */
        {1, 0, 1, 0, 0, "2x_zz + shift + doubled_S"},   /* Double/halve S[] around sample */
        {1, 0, 0, 1, 0, "2x_zz + shift + halved_energy"}, /* Energy on S>>1 */
        {1, 0, 1, 0, 0, "doubled_S + 2x_zz"},           /* Variant A from dwop_doubled.c */
        {1, 1, 0, 0, 1, "2x_zz_then_halve_d + no_shift"}, /* d = (val^sg)>>1, out=S[0] */
        {1, 0, 0, 0, 1, "2x_zz_then_halve_d + shift"},    /* d = (val^sg)>>1, out=S[0]>>1 */
    };
    int nvar = sizeof(variants) / sizeof(variants[0]);

    int16_t *dec = calloc(MAX_DECODE_SAMPLES, sizeof(int16_t));

    int overall_best_score = 0;
    const char *overall_best_name = "";
    int overall_best_offset = -1;

    printf("\n");
    for (int v = 0; v < nvar; v++) {
        memset(dec, 0, MAX_DECODE_SAMPLES * sizeof(int16_t));
        dwop_result_t r = dwop_decode(&variants[v], sdat, (int)sdat_size,
                                       dec, MAX_DECODE_SAMPLES, ref, ref_len);

        printf("--- [%d] %s ---\n", v, variants[v].name);
        printf("  Decoded: %d samples", r.samples_decoded);
        if (r.diverged_at >= 0) {
            printf(" (DIVERGED at sample %d)", r.diverged_at);
        }
        printf("\n");

        if (ref && ref_len > 0) {
            printf("  Best alignment: %d/10 at offset %d\n", r.match_count, r.first_match_offset);

            /* Show extended match at best offset */
            if (r.first_match_offset >= 0 && r.match_count > 0) {
                int off = r.first_match_offset;
                int extended = 0;
                int first_div = -1;
                int limit = PRINT_SAMPLES;
                if (off + limit > r.samples_decoded) limit = r.samples_decoded - off;
                if (limit > ref_len) limit = ref_len;

                for (int i = 0; i < limit; i++) {
                    if (abs(dec[off + i] - ref[i]) <= 2) extended++;
                    else if (first_div < 0) first_div = i;
                }
                printf("  Extended: %d/%d match\n", extended, limit);
                if (first_div >= 0 && first_div < ref_len) {
                    printf("  First diverge: ref[%d]: got %d, expected %d (diff=%d)\n",
                           first_div, dec[off + first_div], ref[first_div],
                           dec[off + first_div] - ref[first_div]);
                }

                /* Print comparison table (capped) */
                int show = limit < 20 ? limit : 20;
                printf("  %6s %8s %8s %6s\n", "ref_i", "decoded", "ref", "diff");
                for (int i = 0; i < show; i++) {
                    int diff = dec[off + i] - ref[i];
                    printf("  %6d %8d %8d %6d %s\n",
                           i, dec[off + i], ref[i], diff,
                           (abs(diff) <= 2) ? "" : "MISMATCH");
                }
            }
        }

        /* Track overall best */
        if (r.match_count > overall_best_score) {
            overall_best_score = r.match_count;
            overall_best_name = variants[v].name;
            overall_best_offset = r.first_match_offset;
        }
        printf("\n");
    }

    printf("========================================\n");
    printf("OVERALL BEST: %d/10 match (%s) at offset %d\n",
           overall_best_score, overall_best_name, overall_best_offset);
    printf("========================================\n");

    free(dec);
    free(sdat);
    if (ref_raw) free(ref_raw);
    return 0;
}
