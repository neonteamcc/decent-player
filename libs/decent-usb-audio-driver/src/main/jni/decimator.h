/**
 * @file decimator.h
 * @brief Streaming integer-ratio (÷2 / ÷4) sample rate decimation.
 *
 * Used when the source rate exceeds the DAC's ceiling within the same
 * rate family: 192 kHz → 96 kHz (÷2), 384 kHz → 96 kHz (÷4),
 * 176.4 kHz → 88.2 kHz (÷2). Each ÷2 stage is a Kaiser-windowed half-band
 * FIR low-pass (transition centered at fs_in/4); ÷4 cascades two stages —
 * the standard polyphase-free construction: deterministic, linear-phase,
 * no asynchronous SRC.
 *
 * Samples are canonical full-scale int32 (interleaved frames); filtering
 * runs in double precision, output saturates back to int32. State persists
 * across process() calls for gapless streaming; reset() clears it on
 * seek/flush.
 *
 * Pure C++ (no JNI/Android) so the filter is host-testable.
 */

#pragma once

#include <cstdint>

struct Decimator;

/**
 * Create a decimator.
 * @param channels Interleaved channel count (1..8).
 * @param factor   2 or 4. Anything else returns nullptr.
 * @return Instance, or nullptr on invalid args/OOM.
 */
Decimator *decimatorCreate(int channels, int factor);

/** Clear filter history (call on seek/flush to avoid stale-tail artifacts). */
void decimatorReset(Decimator *d);

/**
 * Process interleaved int32 frames. Any inFrames value is legal (including
 * odd counts — the phase carries over to the next call).
 *
 * @param in       inFrames × channels canonical int32 samples.
 * @param inFrames Input frame count.
 * @param out      Buffer for at least decimatorMaxOutFrames(inFrames)
 *                 × channels samples.
 * @return Output frame count (≈ inFrames / factor, ±1 from phase carry).
 */
int decimatorProcess(Decimator *d, const int32_t *in, int inFrames, int32_t *out);

/** Upper bound of output frames for a given input frame count. */
int decimatorMaxOutFrames(const Decimator *d, int inFrames);

void decimatorDestroy(Decimator *d);
