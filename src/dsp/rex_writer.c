/*
 * REX2 File Writer
 *
 * Writes REX2 IFF container files with DWOP-compressed audio.
 * Chunk layout matches what rex_parser.c expects:
 *
 *   "CAT " [total_len] "REX "
 *     "GLOB" [20]  — global metadata (tempo, bars, beats, time sig)
 *     "HEAD" [6]   — bytes per sample
 *     "SINF" [10]  — channels, sample rate, total frames
 *     "SLCE" [11]  × slice_count — per-slice info
 *     "SDAT" [len] — DWOP compressed audio
 *
 * All values big-endian. IFF alignment: odd-length chunks get 1 pad byte.
 *
 * License: MIT
 */

#include "rex_writer.h"
#include "dwop_encode.h"
#include <stdlib.h>
#include <string.h>

/* Big-endian writers */
static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void write_tag(uint8_t *p, const char *tag)
{
    p[0] = tag[0]; p[1] = tag[1]; p[2] = tag[2]; p[3] = tag[3];
}

/* Write a chunk header (tag + length). Returns bytes written (8). */
static int write_chunk_header(uint8_t *buf, int pos, int cap,
                               const char *tag, uint32_t data_len)
{
    if (pos + 8 > cap) return -1;
    write_tag(buf + pos, tag);
    write_u32_be(buf + pos + 4, data_len);
    return 8;
}

int rex_write(const rex_write_params_t *params, uint8_t *buf, int buf_cap)
{
    if (!params || !params->pcm_data || params->num_frames <= 0)
        return 0;
    if (params->slice_count <= 0 || !params->slices)
        return 0;

    /* --- Phase 1: Compress audio into temporary buffer --- */

    /* Allocate generous compressed buffer */
    int comp_cap = params->num_frames * params->channels * 3 + 4096;
    uint8_t *comp_buf = (uint8_t *)malloc(comp_cap);
    if (!comp_buf) return 0;

    int comp_bytes;
    if (params->channels == 2) {
        comp_bytes = dwop_encode_stereo(params->pcm_data, params->num_frames,
                                         comp_buf, comp_cap, 1);
    } else {
        dwop_enc_state_t enc;
        dwop_enc_init(&enc, comp_buf, comp_cap);
        int encoded = dwop_encode(&enc, params->pcm_data, params->num_frames, 1);
        if (encoded != params->num_frames) {
            free(comp_buf);
            return 0;
        }
        comp_bytes = dwop_enc_flush(&enc);
    }

    if (comp_bytes <= 0) {
        free(comp_buf);
        return 0;
    }

    /* --- Phase 2: Compute sizes --- */

    /* GLOB: 20 bytes data */
    int glob_data = 20;
    int glob_chunk = 8 + glob_data;  /* no pad needed, 20 is even */

    /* HEAD: 6 bytes data */
    int head_data = 6;
    int head_chunk = 8 + head_data;  /* 6 is even */

    /* SINF: 10 bytes data */
    int sinf_data = 10;
    int sinf_chunk = 8 + sinf_data;  /* 10 is even */

    /* SLCE: 11 bytes data each, odd → 1 pad byte each */
    int slce_data = 11;
    int slce_padded = slce_data + 1;  /* pad to even */
    int slce_total = params->slice_count * (8 + slce_padded);

    /* SDAT: variable length */
    int sdat_data = comp_bytes;
    int sdat_padded = sdat_data + (sdat_data & 1);  /* pad if odd */
    int sdat_chunk = 8 + sdat_padded;

    /* CAT contents: "REX " (4 bytes) + all chunks */
    int cat_content = 4 + glob_chunk + head_chunk + sinf_chunk + slce_total + sdat_chunk;

    /* Total file: "CAT " (4) + length (4) + content */
    int total = 8 + cat_content;

    if (total > buf_cap) {
        free(comp_buf);
        return 0;
    }

    /* --- Phase 3: Write the file --- */

    int pos = 0;

    /* CAT header */
    write_tag(buf + pos, "CAT ");
    write_u32_be(buf + pos + 4, (uint32_t)cat_content);
    pos += 8;

    /* "REX " type */
    write_tag(buf + pos, "REX ");
    pos += 4;

    /* GLOB chunk */
    write_chunk_header(buf, pos, buf_cap, "GLOB", glob_data);
    pos += 8;
    {
        uint8_t *g = buf + pos;
        memset(g, 0, glob_data);
        /* [0:4] reserved */
        write_u16_be(g + 4, (uint16_t)params->bars);
        g[6] = (uint8_t)params->beats;
        g[7] = (uint8_t)params->time_sig_num;
        g[8] = (uint8_t)params->time_sig_den;
        g[9] = 0x40;                          /* sensitivity */
        write_u16_be(g + 10, 0x7FFF);         /* gate */
        write_u16_be(g + 12, 0x7FFF);         /* gain */
        write_u16_be(g + 14, 0x4000);         /* pitch (neutral) */
        uint32_t tempo_milli = (uint32_t)(params->tempo_bpm * 1000.0f + 0.5f);
        write_u32_be(g + 16, tempo_milli);
        pos += glob_data;
    }

    /* HEAD chunk */
    write_chunk_header(buf, pos, buf_cap, "HEAD", head_data);
    pos += 8;
    {
        uint8_t *h = buf + pos;
        memset(h, 0, head_data);
        h[5] = 2;  /* bytes_per_sample = 2 (16-bit) */
        pos += head_data;
    }

    /* SINF chunk */
    write_chunk_header(buf, pos, buf_cap, "SINF", sinf_data);
    pos += 8;
    {
        uint8_t *s = buf + pos;
        memset(s, 0, sinf_data);
        s[0] = (uint8_t)params->channels;
        s[1] = 3;  /* bit depth indicator: 3 = 16-bit */
        /* [2:4] unknown, zero */
        write_u16_be(s + 4, (uint16_t)params->sample_rate);
        write_u32_be(s + 6, (uint32_t)params->num_frames);
        pos += sinf_data;
    }

    /* SLCE chunks */
    for (int i = 0; i < params->slice_count; i++) {
        write_chunk_header(buf, pos, buf_cap, "SLCE", slce_data);
        pos += 8;
        {
            uint8_t *sl = buf + pos;
            write_u32_be(sl, params->slices[i].sample_offset);
            write_u32_be(sl + 4, params->slices[i].sample_length);
            write_u16_be(sl + 8, 0x7FFF);  /* amplitude */
            sl[10] = 0;                     /* zero byte */
            pos += slce_data;
        }
        /* IFF pad byte for odd-length chunk */
        buf[pos] = 0;
        pos++;
    }

    /* SDAT chunk */
    write_chunk_header(buf, pos, buf_cap, "SDAT", sdat_data);
    pos += 8;
    memcpy(buf + pos, comp_buf, comp_bytes);
    pos += comp_bytes;
    if (comp_bytes & 1) {
        buf[pos] = 0;  /* IFF pad byte */
        pos++;
    }

    free(comp_buf);
    return pos;
}
