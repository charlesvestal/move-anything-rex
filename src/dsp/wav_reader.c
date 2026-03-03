/*
 * Minimal WAV Reader
 *
 * Reads PCM WAV files (RIFF/WAVE container, format tag 1).
 * Accepts 16-bit, 24-bit, and 32-bit input; always outputs 16-bit PCM.
 * Higher bit depths are truncated (right-shifted) to 16-bit.
 *
 * License: MIT
 */

#include "wav_reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Little-endian readers */
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int tag_eq(const uint8_t *p, const char *tag)
{
    return p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3];
}

int wav_read(wav_file_t *wav, const uint8_t *data, size_t data_len)
{
    memset(wav, 0, sizeof(*wav));

    if (data_len < 44) {
        snprintf(wav->error, sizeof(wav->error), "File too small (%zu bytes)", data_len);
        return -1;
    }

    /* Verify RIFF header */
    if (!tag_eq(data, "RIFF") || !tag_eq(data + 8, "WAVE")) {
        snprintf(wav->error, sizeof(wav->error), "Not a RIFF/WAVE file");
        return -1;
    }

    /* Walk chunks to find fmt and data */
    int found_fmt = 0;
    int found_data = 0;
    uint16_t fmt_tag = 0;
    size_t offset = 12;  /* past RIFF header + "WAVE" */

    while (offset + 8 <= data_len) {
        const uint8_t *chunk_tag = data + offset;
        uint32_t chunk_len = read_u32_le(data + offset + 4);
        const uint8_t *chunk_data = data + offset + 8;

        if (offset + 8 + chunk_len > data_len) {
            /* Tolerate truncated last chunk if it's the data chunk */
            if (tag_eq(chunk_tag, "data") && !found_data) {
                chunk_len = (uint32_t)(data_len - offset - 8);
            } else {
                break;
            }
        }

        if (tag_eq(chunk_tag, "fmt ") && chunk_len >= 16) {
            fmt_tag = read_u16_le(chunk_data);
            wav->channels = read_u16_le(chunk_data + 2);
            wav->sample_rate = (int)read_u32_le(chunk_data + 4);
            wav->bits_per_sample = read_u16_le(chunk_data + 14);
            found_fmt = 1;
        } else if (tag_eq(chunk_tag, "data") && !found_data) {
            if (!found_fmt) {
                snprintf(wav->error, sizeof(wav->error), "data chunk before fmt chunk");
                return -1;
            }
            if (fmt_tag != 1) {
                snprintf(wav->error, sizeof(wav->error),
                         "Unsupported format tag %d (only PCM=1)", fmt_tag);
                return -1;
            }
            int bps = wav->bits_per_sample;
            if (bps != 16 && bps != 24 && bps != 32) {
                snprintf(wav->error, sizeof(wav->error),
                         "Unsupported bit depth %d (need 16, 24, or 32)", bps);
                return -1;
            }
            if (wav->channels < 1 || wav->channels > 2) {
                snprintf(wav->error, sizeof(wav->error),
                         "Unsupported channel count %d", wav->channels);
                return -1;
            }

            int bytes_per_sample = bps / 8;
            int bytes_per_frame = wav->channels * bytes_per_sample;
            wav->num_frames = (int)(chunk_len / bytes_per_frame);

            if (wav->num_frames <= 0) {
                snprintf(wav->error, sizeof(wav->error), "No audio frames in data chunk");
                return -1;
            }

            int total_samples = wav->num_frames * wav->channels;
            wav->pcm_data = (int16_t *)malloc((size_t)total_samples * sizeof(int16_t));
            if (!wav->pcm_data) {
                snprintf(wav->error, sizeof(wav->error), "Failed to allocate PCM buffer");
                return -1;
            }

            if (bps == 16) {
                /* Direct copy (WAV is LE, native x86/ARM is also LE) */
                memcpy(wav->pcm_data, chunk_data, (size_t)total_samples * 2);
            } else if (bps == 24) {
                /* 24-bit LE signed → 16-bit: take top 16 bits (right-shift by 8) */
                const uint8_t *src = chunk_data;
                for (int i = 0; i < total_samples; i++) {
                    /* 24-bit LE: byte0=LSB, byte1=mid, byte2=MSB (sign) */
                    int32_t val = (int32_t)(((uint32_t)src[1] << 8) |
                                            ((uint32_t)src[2] << 16));
                    /* Sign-extend from 24-bit */
                    val = (val << 8) >> 8;
                    wav->pcm_data[i] = (int16_t)(val >> 8);
                    src += 3;
                }
            } else {  /* 32-bit */
                /* 32-bit LE signed → 16-bit: take top 16 bits (right-shift by 16) */
                const uint8_t *src = chunk_data;
                for (int i = 0; i < total_samples; i++) {
                    int32_t val = (int32_t)read_u32_le(src);
                    wav->pcm_data[i] = (int16_t)(val >> 16);
                    src += 4;
                }
            }
            /* Output is always 16-bit regardless of input */
            wav->bits_per_sample = 16;
            found_data = 1;
        }

        /* Advance to next chunk (WAV chunks are word-aligned) */
        uint32_t padded = chunk_len;
        if (padded & 1) padded++;
        offset += 8 + padded;
    }

    if (!found_fmt) {
        snprintf(wav->error, sizeof(wav->error), "No fmt chunk found");
        return -1;
    }
    if (!found_data) {
        snprintf(wav->error, sizeof(wav->error), "No data chunk found");
        return -1;
    }

    return 0;
}

void wav_free(wav_file_t *wav)
{
    if (wav->pcm_data) {
        free(wav->pcm_data);
        wav->pcm_data = NULL;
    }
    wav->num_frames = 0;
}
