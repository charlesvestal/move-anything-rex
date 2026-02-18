/*
 * REXWAV Reader
 *
 * Reads .rexwav files produced by the rex2rexwav desktop converter.
 * These contain pre-decoded PCM slice data from the REX SDK.
 *
 * File format:
 *   Header (64 bytes):
 *     [0:4]   magic "RXWV"
 *     [4:8]   version (1)
 *     [8:12]  sample_rate
 *     [12:16] channels
 *     [16:20] slice_count
 *     [20:24] total_frames
 *     [24:28] tempo_millibpm
 *     [28]    time_sig_num
 *     [29]    time_sig_den
 *     [30:32] bit_depth
 *     [32:64] reserved
 *   Slice table (slice_count * 8 bytes):
 *     [0:4] frame_offset
 *     [4:8] frame_length
 *   PCM data (total_frames * channels * 2 bytes):
 *     16-bit signed LE interleaved
 *
 * License: MIT
 */

#ifndef REXWAV_READER_H
#define REXWAV_READER_H

#include "rex_parser.h"  /* reuse rex_file_t and rex_slice_t */

/* Parse a .rexwav file from an in-memory buffer.
 * Populates the same rex_file_t structure used by rex_parse().
 * Returns 0 on success, -1 on error (check rex->error).
 * Caller must call rex_free() when done. */
int rexwav_parse(rex_file_t *rex, const uint8_t *data, size_t data_len);

#endif /* REXWAV_READER_H */
