# REX2 File Format & DWOP Codec Specification

Reverse-engineered specification of the Propellerhead REX2 (`.rx2`) file format
and its DWOP lossless audio codec. REX2 files store sliced audio loops used by
ReCycle, Reason, and other DAWs.

## Table of Contents

- [File Structure](#file-structure)
- [Chunk Types](#chunk-types)
  - [GLOB — Global Metadata](#glob--global-metadata)
  - [HEAD — Audio Format Header](#head--audio-format-header)
  - [SINF — Sound Information](#sinf--sound-information)
  - [SLCE — Slice Definition](#slce--slice-definition)
  - [SDAT — Compressed Audio](#sdat--compressed-audio)
- [Slice Length Computation](#slice-length-computation)
- [DWOP Codec](#dwop-codec)
  - [Overview](#overview)
  - [Initial State](#initial-state)
  - [Decoding Algorithm](#decoding-algorithm)
  - [Predictor Mapping](#predictor-mapping)
  - [Predictor Update Rules](#predictor-update-rules)
  - [Bit Reader](#bit-reader)
  - [Constants](#constants)
- [Worked Example](#worked-example)
- [Verification](#verification)

---

## File Structure

REX2 files use an IFF (Interchange File Format) container with **big-endian**
byte order throughout.

```
REX2 File
├── CAT header (4 bytes "CAT " + 4-byte length + 4-byte type)
│   ├── GLOB chunk  — tempo, time signature, bars
│   ├── HEAD chunk  — bytes per sample
│   ├── SINF chunk  — sample rate, total decode length
│   ├── SLCE chunk  — slice 0 sample offset
│   ├── SLCE chunk  — slice 1 sample offset
│   ├── ...
│   ├── SDAT chunk  — DWOP-compressed audio
│   └── (other chunks: RECY, etc.)
└── (possible nested CAT containers)
```

Every chunk has this layout:

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 4 | Tag (ASCII, e.g. `"GLOB"`, `"SDAT"`) |
| 4 | 4 | Chunk data length (uint32 big-endian) |
| 8 | N | Chunk data (N = length) |
| 8+N | 0–1 | Padding byte if length is odd (IFF alignment) |

`CAT ` chunks are containers: after the 8-byte header, they have a 4-byte type
identifier followed by nested chunks. The parser recurses into CAT containers
bounded by their declared length.

---

## Chunk Types

### GLOB — Global Metadata

Stores tempo, time signature, and loop structure.

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 4 | uint32 | (reserved / PPQ-related) |
| 4 | 2 | uint16 | Bars |
| 6 | 1 | uint8 | Beats |
| 7 | 1 | uint8 | Time signature numerator |
| 8 | 1 | uint8 | Time signature denominator |
| 9 | 1 | uint8 | Sensitivity |
| 10 | 2 | uint16 | Gate sensitivity |
| 12 | 2 | uint16 | Gain |
| 14 | 2 | uint16 | Pitch |
| 16 | 4 | uint32 | Tempo in milli-BPM |

Tempo is stored as `BPM × 1000`. For example, 120.0 BPM is stored as `120000`.

Minimum chunk data length: **20 bytes**.

### HEAD — Audio Format Header

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0–4 | 5 | — | (unknown) |
| 5 | 1 | uint8 | Bytes per sample (typically 2 = 16-bit) |

Minimum chunk data length: **6 bytes**.

### SINF — Sound Information

Core audio format parameters and the total decode length.

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 1 | uint8 | Channels / version indicator |
| 1 | 1 | uint8 | (unknown) |
| 2 | 2 | uint16 | (unknown) |
| 4 | 2 | uint16 | Sample rate (e.g. 0xAC44 = 44100) |
| 6 | 4 | uint32 | Total sample length (number of samples to decode) |

The **total sample length** is the exact number of int16 samples that
`DecompressMono` should produce from the SDAT data. This includes a leading
warmup region of silent/near-silent samples before the first slice.

Minimum chunk data length: **10 bytes**. Falls back to 44100 Hz if sample rate
reads as 0.

### SLCE — Slice Definition

Each SLCE chunk defines one slice boundary. There is one SLCE chunk per slice,
appearing in order.

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 4 | uint32 | Sample offset into decoded PCM buffer |

The sample offset is an absolute index into the decoded audio. For example, if
the first slice has offset 322, then `decoded[322]` is the first sample of
slice 0.

**Note:** Some REX2 files contain SLCE chunks that are transient markers rather
than audio slices. The REX SDK reports fewer slices (e.g. 10) than the total
SLCE chunk count in the IFF structure (e.g. 32). Marker slices typically have
very short computed lengths (1 sample).

Minimum chunk data length: **4 bytes**.

### SDAT — Compressed Audio

Contains the raw DWOP-compressed bitstream. The entire chunk data is passed
directly to the DWOP decoder. See [DWOP Codec](#dwop-codec) below.

---

## Slice Length Computation

Slice lengths are **not** stored in the file. They are derived from consecutive
slice offsets after parsing:

```
For slice i (not the last):
    length[i] = offset[i + 1] - offset[i]

For the last slice:
    length[last] = total_sample_length - offset[last]
```

If a computed length would extend past the decoded PCM buffer, it is clamped.

---

## DWOP Codec

### Overview

DWOP (Delta Width Optimized Predictor) is an adaptive lossless audio codec. It
uses 5 linear predictors of increasing order (0th through 4th difference) and
selects the best predictor per sample based on a running energy metric.

The codec operates on a **doubled representation** — internal state values are
2× the actual audio samples. The final output divides by 2.

### Initial State

| Variable | Initial Value | Description |
|----------|---------------|-------------|
| `S[0..4]` | 0 | Predictor state (doubled) |
| `e[0..4]` | 2560 | Energy trackers |
| `rv` | 2 | Range coder value |
| `ba` | 0 | Range coder bits accumulated |

### Decoding Algorithm

For each output sample:

#### Step 1: Select Predictor

Find the energy index `p` with the smallest energy value. Uses unsigned
comparison; on ties, the lowest index wins (strictly-less-than).

```
p_order = argmin(e[0], e[1], e[2], e[3], e[4])
```

#### Step 2: Compute Quantizer Step

```
step = (min_energy × 3 + 36) >> 7
```

Where `min_energy = e[p_order]`, and `>> 7` is an unsigned right shift by 7.

#### Step 3: Read Unary Code

Read bits from the SDAT stream (MSB-first). Count `0`-bits until a `1`-bit is
encountered. Each `0`-bit accumulates into the value. Every 7 consecutive
zeros, the step size quadruples:

```
acc = 0
cs = step
qc = 7

repeat:
    bit = read_bit()
    if bit == 1: break
    acc += cs
    qc -= 1
    if qc == 0:
        cs = cs × 4
        qc = 7
```

#### Step 4: Range Coder (Remainder)

Adapt the range coder and read the fractional remainder:

```
nb = ba    (previous bits-accumulated)

if cs >= rv:
    while cs >= rv:
        rv = rv × 2
        nb += 1
else:
    nb += 1
    t = rv
    loop:
        rv = t
        t = t / 2
        nb -= 1
        if cs >= t: break

ext = read_bits(nb)     (nb bits, MSB first; 0 if nb <= 0)
co = rv - cs

if ext < co:
    rem = ext
else:
    x = read_bit()
    rem = co + (ext - co) × 2 + x

val = acc + rem
ba = nb                 (save for next sample)
```

#### Step 5: DWOP Zigzag Decode

Convert the unsigned `val` to a signed doubled delta:

```
d = val XOR (-(val AND 1))
```

This maps: `0→0, 1→-2, 2→2, 3→-4, 4→4, 5→-6, ...`

All resulting `d` values are even integers (the doubled representation).

#### Step 6: Update Predictor State

Apply the predictor update using the **mapped** case (see [Predictor
Mapping](#predictor-mapping)):

See [Predictor Update Rules](#predictor-update-rules) below.

#### Step 7: Update Energy Trackers

For each predictor `i` (0–4):

```
abs_S = S[i] XOR (S[i] >> 31)     (arithmetic shift; branchless "cheap abs")
e[i] = e[i] + abs_S - (e[i] >>> 5)  (unsigned right shift by 5)
```

This is an exponential moving average with decay factor 1/32. The "cheap abs"
gives `|S| - 1` for negative values and `|S|` for non-negative — this is
intentional and matches the binary.

#### Step 8: Output

```
output = (int16_t)(S[0] >> 1)      (arithmetic right shift to un-double)
```

### Predictor Mapping

**This is the critical detail.** The energy index does not map directly to the
switch case number. It maps to the **prediction order**:

| Energy Index | Prediction Order | Switch Case | Meaning |
|:---:|:---:|:---:|---|
| 0 | 0 | 0 | `d` is raw sample |
| 1 | 1 | 1 | `d` is 1st difference |
| 2 | 2 | **4** | `d` is 2nd difference |
| 3 | 3 | **2** | `d` is 3rd difference |
| 4 | 4 | **3** | `d` is 4th difference |

The mapping array is: **`[0, 1, 4, 2, 3]`**

Energy index 2 maps to case 4 (not case 2), energy index 3 maps to case 2 (not
case 3), and energy index 4 maps to case 3 (not case 4). Getting this wrong
causes the decoder to diverge after a few hundred samples when higher-order
predictors are first selected.

### Predictor Update Rules

Let `old[0..4]` be the previous state values. After decoding delta `d`:

**Case 0 — Order 0** (d is the doubled sample):
```
S[0] = d
S[1] = d - old[0]
S[2] = S[1] - old[1]
S[3] = S[2] - old[2]
S[4] = S[3] - old[3]
```

**Case 1 — Order 1** (d is the doubled 1st difference):
```
S[0] = old[0] + d
S[1] = d
S[2] = d - old[1]
S[3] = S[2] - old[2]
S[4] = S[3] - old[3]
```

**Case 4 — Order 2** (d is the doubled 2nd difference):
```
S[1] = old[1] + d
S[0] = old[0] + S[1]
S[2] = d
S[3] = d - old[2]
S[4] = S[3] - old[3]
```

**Case 2 — Order 3** (d is the doubled 3rd difference):
```
S[2] = old[2] + d
S[1] = old[1] + S[2]
S[0] = old[0] + S[1]
S[3] = d
S[4] = d - old[3]
```

**Case 3 — Order 4** (d is the doubled 4th difference):
```
S[3] = old[3] + d
S[2] = old[2] + S[3]
S[1] = old[1] + S[2]
S[0] = old[0] + S[1]
S[4] = d
```

The pattern: for order N, `d` replaces `S[N]` (with appropriate index
remapping), then higher-order states cascade down while lower-order states
integrate up to reconstruct `S[0]`.

### Bit Reader

Bits are read **MSB-first** within each byte, processing bytes sequentially:

```
Byte:     [b7 b6 b5 b4 b3 b2 b1 b0]
Read order: 1   2   3   4   5   6   7   8
```

When all 8 bits of a byte are consumed, the next byte is loaded. If the data
stream is exhausted, 0-bits are returned.

### Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| Energy init | 2560 (0xA00) | Starting energy for all 5 predictors |
| Range init | 2 | Initial range coder value (`rv`) |
| Step formula | `(e×3+36)>>7` | Quantizer step from energy |
| Unary doubling | ×4 every 7 zeros | `cs <<= 2` when `qc` hits 0 |
| Energy decay | 1/32 | `e >>= 5` subtracted per sample |
| Predictor map | [0,1,4,2,3] | Energy index to switch case |

---

## Worked Example

For the test file `120Mono.rx2`:

| Property | Value |
|----------|-------|
| Sample rate | 44100 Hz |
| Channels | 1 (mono) |
| Bit depth | 16 |
| Tempo | 120.0 BPM |
| Original tempo | 93.3 BPM |
| Total decode length | 117,760 samples |
| SDAT compressed size | 118,304 bytes |
| Compression ratio | ~1.00× (lossless, audio fills 16 bits) |
| Slice count (IFF) | 32 (includes transient markers) |
| Slice count (SDK) | 10 (audio slices only) |
| First slice offset | 322 samples |
| Warmup region | Samples 0–287 (all zero) |
| Transition region | Samples 288–321 (small ramp-up values) |

First decoded samples at the audio boundary:
```
[288] = -1    (first non-zero)
[312] =  6
[322] = -231  (first audio slice starts here)
[323] = -421
[324] = -410
```

---

## Verification

The decoder was verified by:

1. **LLDB tracing** the proprietary `DecompressMono` function in the REX Shared
   Library framework (macOS ARM64)
2. Capturing the complete 117,760-sample int16 output buffer from the real binary
3. Comparing our decoder output sample-by-sample: **117,760/117,760 exact match**
4. Confirming the decoder does not diverge over the full file length
5. Round-trip testing: decode SDAT → re-encode → compare bits against original
   SDAT (bit-perfect match for the core entropy coding)
