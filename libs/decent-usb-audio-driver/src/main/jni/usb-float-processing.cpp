#include "usb-audio-output.h"
#include "decimator.h"
#include "resampler.h"
#include <android/log.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "UsbAudioOutput", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "UsbAudioOutput", __VA_ARGS__)

static bool outputMemory(UsbAudioContext *ctx, uint8_t **buffer, int32_t *capacity, int bytes) {
    if (*buffer && *capacity >= bytes) return true;
    auto *replacement = static_cast<uint8_t *>(std::realloc(*buffer, bytes));
    if (!replacement) {
        LOGE("Output buffer allocation failed: %d bytes", bytes);
        ctx->running.store(false);
        return false;
    }
    *buffer = replacement;
    *capacity = bytes;
    return true;
}

static bool syncLimiter(UsbAudioContext *ctx) {
    const bool enabled = ctx->limiterEnabled.load(std::memory_order_relaxed);
    if (enabled != ctx->limiterApplied) {
        ctx->limiter.reset();
        ctx->limiterApplied = enabled;
        LOGI("USB limiter: %s (after SRC and software gain)", enabled ? "on" : "off");
    }
    return enabled;
}

static void recordFrames(UsbAudioContext *ctx, int frames, const char *path) {
    ctx->framesWritten += frames;
    if (ctx->framesWritten % ctx->sampleRate < frames) {
        LOGI("%s: %lld frames inflight=%d wire=%s limiter=%d fpmf=%.4f",
             path, (long long)ctx->framesWritten, ctx->urbsInFlight,
             ctx->wireFormat == 1 ? "IEEE_FLOAT32" : "PCM_INTEGER",
             ctx->limiterApplied ? 1 : 0, ctx->calibratedFpmf);
    }
}

// The only narrowing boundary for processed audio. IEEE_FLOAT keeps finite
// headroom; integer PCM saturates to the destination's representable range.
static void packOutput(UsbAudioContext *ctx, const float *samples, int count, bool dither) {
    const double scale = std::ldexp(1.0, ctx->validBitDepth - 1);
    for (int i = 0; i < count; ++i) {
        const float value = std::isfinite(samples[i]) ? samples[i] : 0.0f;
        uint32_t word;
        if (ctx->wireFormat == 1) {
            std::memcpy(&word, &value, sizeof(word));
        } else {
            double quantized = double(value) * scale;
            if (dither) {
                // Preserve the existing final PCM16 TPDF dither for SRC,
                // software attenuation and other actually processed samples.
                ctx->ditherState = ctx->ditherState * 1664525u + 1013904223u;
                const int a = int(ctx->ditherState & 0xFF);
                ctx->ditherState = ctx->ditherState * 1664525u + 1013904223u;
                const int b = int(ctx->ditherState & 0xFF);
                quantized = std::nearbyint(quantized + (a + b - 255) / 256.0);
            }
            const double bounded = std::max(-scale, std::min(scale - 1.0, quantized));
            word = uint32_t(int64_t(bounded)) << (ctx->bitDepth - ctx->validBitDepth);
        }
        for (int b = 0; b < ctx->bytesPerSample; ++b)
            ctx->transferBuffer[i * ctx->bytesPerSample + b] = uint8_t(word >> (8 * b));
    }
}

void submitFloatPcm(UsbAudioContext *ctx, const float *samples, int frames) {
    if (!ctx || !ctx->running.load() || frames <= 0) return;
    if (frames > INT32_MAX / ctx->channelCount / 4) { ctx->running.store(false); return; }
    const int count = frames * ctx->channelCount;
    if (!outputMemory(ctx, &ctx->floatInputBuffer, &ctx->floatInputCapacity, count * 4)) return;
    float *input = reinterpret_cast<float *>(ctx->floatInputBuffer);
    for (int i = 0; i < count; ++i) input[i] = std::isfinite(samples[i]) ? samples[i] : 0.0f;
    float *working = input;
    int outFrames = frames;
    if (ctx->inputRate > 0) {
        const int capacity = ctx->decimator ? decimatorMaxOutFrames(ctx->decimator, frames)
                                           : resamplerMaxOutFrames(ctx->resampler, frames);
        if (capacity > INT32_MAX / ctx->channelCount / 4) { ctx->running.store(false); return; }
        if (!outputMemory(ctx, &ctx->floatOutputBuffer, &ctx->floatOutputCapacity,
                          capacity * ctx->channelCount * 4)) return;
        working = reinterpret_cast<float *>(ctx->floatOutputBuffer);
        outFrames = ctx->decimator ? decimatorProcessFloat(ctx->decimator, input, frames, working)
                                  : resamplerProcessFloat(ctx->resampler, input, frames, working);
    }
    if (outFrames <= 0) return;
    const int outSamples = outFrames * ctx->channelCount;
    const float requestedGain = ctx->softGain.load(std::memory_order_relaxed);
    const float gain = std::isfinite(requestedGain) ? std::max(0.0f, std::min(1.0f, requestedGain)) : 0.0f;
    if (gain != 1.0f) for (int i = 0; i < outSamples; ++i) working[i] *= gain;
    const bool limited = syncLimiter(ctx) && ctx->limiter.process(working, outFrames, ctx->channelCount);
    const int bytes = outFrames * ctx->bytesPerFrame;
    if (!outputMemory(ctx, &ctx->transferBuffer, &ctx->transferBufferCapacity, bytes)) return;
    const bool dither = ctx->wireFormat == 0 && ctx->validBitDepth == 16 &&
            (ctx->inputRate > 0 || gain != 1.0f || limited);
    packOutput(ctx, working, outSamples, dither);
    submitPcmToUrbs(ctx, ctx->transferBuffer, bytes);
    recordFrames(ctx, outFrames, "WriteFloat");
}

void submitRawPcm(UsbAudioContext *ctx, const uint8_t *pcm, int bytes, int inputBits) {
    if (!ctx || !ctx->running.load() || bytes <= 0) return;
    if (inputBits != 16 && inputBits != 24 && inputBits != 32) return;
    const int inputBps = inputBits / 8;
    if (bytes % (inputBps * ctx->channelCount) != 0) return;
    const int count = bytes / inputBps;
    const int frames = count / ctx->channelCount;
    const bool limit = syncLimiter(ctx);
    // Integer input cannot exceed nominal full scale. Preserve every bit at
    // unity when no SRC or pending limiter recovery needs processing.
    const bool processing = ctx->inputRate > 0 || ctx->wireFormat == 1 ||
            ctx->softGain.load(std::memory_order_relaxed) != 1.0f ||
            (limit && !ctx->limiter.isUnity());
    if (processing) {
        if (count > INT32_MAX / 4) { ctx->running.store(false); return; }
        if (!outputMemory(ctx, &ctx->floatInputBuffer, &ctx->floatInputCapacity, count * 4)) return;
        float *input = reinterpret_cast<float *>(ctx->floatInputBuffer);
        for (int i = 0; i < count; ++i) {
            uint32_t value = 0;
            for (int b = 0; b < inputBps; ++b) value |= uint32_t(pcm[i * inputBps + b]) << (8 * b);
            const int32_t signedValue = int32_t(value << (32 - inputBits));
            input[i] = float(double(signedValue) / 2147483648.0);
        }
        submitFloatPcm(ctx, input, frames);
        return;
    }
    if (count > INT32_MAX / ctx->bytesPerSample) { ctx->running.store(false); return; }
    const int outBytes = count * ctx->bytesPerSample;
    if (!outputMemory(ctx, &ctx->transferBuffer, &ctx->transferBufferCapacity, outBytes)) return;
    if (ctx->validBitDepth == 16 && inputBits > 16) {
        if (!outputMemory(ctx, &ctx->canonicalBuffer, &ctx->canonicalCapacity, count * 2)) return;
        if (inputBits == 24)
            ditherInt24ToInt16(pcm, ctx->canonicalBuffer, count, &ctx->ditherState);
        else
            ditherInt32ToInt16(pcm, ctx->canonicalBuffer, count, &ctx->ditherState);
        for (int i = 0; i < count; ++i) {
            uint32_t value = uint32_t(ctx->canonicalBuffer[i * 2]) |
                    (uint32_t(ctx->canonicalBuffer[i * 2 + 1]) << 8);
            value <<= ctx->bitDepth - 16;
            for (int b = 0; b < ctx->bytesPerSample; ++b)
                ctx->transferBuffer[i * ctx->bytesPerSample + b] = uint8_t(value >> (8 * b));
        }
    }
    else if (inputBits == ctx->bitDepth) std::memcpy(ctx->transferBuffer, pcm, bytes);
    else if (inputBits == 32 && ctx->bitDepth == 24)
        packInt32ToInt24(pcm, ctx->transferBuffer, count);
    else {
        const int shift = ctx->bitDepth - inputBits;
        if (shift < 0) { ctx->running.store(false); return; }
        for (int i = 0; i < count; ++i) {
            uint32_t value = 0;
            for (int b = 0; b < inputBps; ++b) value |= uint32_t(pcm[i * inputBps + b]) << (8 * b);
            value <<= shift;
            for (int b = 0; b < ctx->bytesPerSample; ++b)
                ctx->transferBuffer[i * ctx->bytesPerSample + b] = uint8_t(value >> (8 * b));
        }
    }
    const int unusedBits = ctx->bitDepth - ctx->validBitDepth;
    if (unusedBits > 0) {
        const uint32_t mask = UINT32_MAX << unusedBits;
        for (int i = 0; i < count; ++i) {
            uint32_t value = 0;
            for (int b = 0; b < ctx->bytesPerSample; ++b)
                value |= uint32_t(ctx->transferBuffer[i * ctx->bytesPerSample + b]) << (8 * b);
            value &= mask;
            for (int b = 0; b < ctx->bytesPerSample; ++b)
                ctx->transferBuffer[i * ctx->bytesPerSample + b] = uint8_t(value >> (8 * b));
        }
    }
    submitPcmToUrbs(ctx, ctx->transferBuffer, outBytes);
    recordFrames(ctx, frames, "WriteRaw");
}
