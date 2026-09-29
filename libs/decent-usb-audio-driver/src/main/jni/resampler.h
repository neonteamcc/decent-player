/**
 * @file resampler.h
 * @brief Streaming rational (L/M) polyphase sample rate conversion.
 *
 * Covers cross-family conversions that the half-band decimator can't:
 * 44.1 kHz → 48 kHz (L/M = 160/147, e.g. a device advertising only the
 * 48 k family), 88.2 kHz → 48 kHz (80/147), 48 kHz → 44.1 kHz (147/160),
 * and MaxPacketsOnly devices that behave correctly at exactly one rate.
 * In-family ÷2/÷4 stays on the half-band decimator (cheaper, flatter).
 *
 * Design: windowed-sinc (Kaiser β 10.06, ~100 dB stopband) polyphase
 * interpolate-by-L / decimate-by-M. Passband is flat to ~0.907 of the
 * narrower Nyquist (20 kHz for 44.1↔48 conversions); everything that
 * could alias lands below the 16-bit noise floor. Filtering runs in
 * double precision over int32 or normalized float frames; state
 * persists across process() calls, reset() clears it on seek/flush.
 *
 * Pure C++ (no JNI/Android) so the filter is host-testable.
 */

#pragma once

#include <cstdint>

struct RationalResampler;

/**
 * Create a resampler converting inputRate → outputRate.
 * The ratio is reduced internally (L = out/gcd, M = in/gcd).
 * @return Instance, or nullptr when the ratio is unsupported
 *         (reduced L or M above 512 — pathological rates) or on OOM.
 */
RationalResampler *resamplerCreate(int channels, int inputRate, int outputRate);

/** Clear filter history and phase (call on seek/flush). */
void resamplerReset(RationalResampler *r);

/** Upper bound of output frames produced for inFrames of input. */
int resamplerMaxOutFrames(const RationalResampler *r, int inFrames);

/**
 * Process interleaved canonical int32 frames. Any inFrames is legal; the
 * conversion phase carries across calls for gapless streaming.
 * @return Output frame count.
 */
int resamplerProcess(RationalResampler *r, const int32_t *in, int inFrames, int32_t *out);

/**
 * Process interleaved float frames (1.0 is full scale). Values outside
 * [-1, 1] remain unclipped through filtering and output. Allocate at least
 * resamplerMaxOutFrames(r, inFrames) × channels float samples for out.
 * Shares history, phase, frame counts and reset semantics with the int32 API;
 * callers may alternate entry points without resetting the filter.
 */
int resamplerProcessFloat(RationalResampler *r, const float *in, int inFrames, float *out);

void resamplerDestroy(RationalResampler *r);
