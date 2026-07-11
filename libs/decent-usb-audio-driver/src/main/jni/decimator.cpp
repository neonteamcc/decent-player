/**
 * @file decimator.cpp
 * @brief Half-band FIR ÷2 stages, cascaded for ÷4. See decimator.h.
 */

#include "decimator.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

// 63-tap Kaiser (β = 9.6) half-band: stopband ≥ ~95 dB, transition
// ±~0.05·fs_in around fs_in/4 — passband flat to ~0.20·fs_in (38.4 kHz
// for a 192 kHz input), everything that could alias into the audible
// band is attenuated below the 16-bit noise floor.
static const int HB_TAPS = 63;
static const double HB_BETA = 9.6;

// One ÷2 half-band stage for all channels (interleaved input).
struct HalfbandStage {
    int channels;
    double coef[HB_TAPS];
    double *hist;      // channels × HB_TAPS circular delay lines
    int histPos;       // shared write index (all channels advance together)
    int phase;         // input-frame parity; output on phase == 1
};

static double besselI0(double x) {
    // Series expansion; converges quickly for the β range used here.
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; k++) {
        double f = x / (2.0 * k);
        term *= f * f;
        sum += term;
        if (term < 1e-21 * sum) break;
    }
    return sum;
}

static void designHalfband(double *h, int taps, double beta) {
    const int m = (taps - 1) / 2;
    const double i0b = besselI0(beta);
    for (int n = 0; n < taps; n++) {
        const int k = n - m;
        double ideal;
        if (k == 0) {
            ideal = 0.5;
        } else if ((k & 1) == 0) {
            ideal = 0.0; // half-band: even taps (except center) are zero
        } else {
            ideal = sin(M_PI * k / 2.0) / (M_PI * k);
        }
        const double r = (double)k / m;
        const double w = besselI0(beta * sqrt(1.0 - r * r)) / i0b;
        h[n] = ideal * w;
    }
    // Normalize DC gain to exactly 1.0 (window slightly perturbs it).
    double dc = 0.0;
    for (int n = 0; n < taps; n++) dc += h[n];
    for (int n = 0; n < taps; n++) h[n] /= dc;
}

static HalfbandStage *stageCreate(int channels) {
    auto *s = new (std::nothrow) HalfbandStage();
    if (!s) return nullptr;
    s->channels = channels;
    s->hist = (double *)calloc((size_t)channels * HB_TAPS, sizeof(double));
    if (!s->hist) { delete s; return nullptr; }
    s->histPos = 0;
    s->phase = 0;
    designHalfband(s->coef, HB_TAPS, HB_BETA);
    return s;
}

static void stageReset(HalfbandStage *s) {
    memset(s->hist, 0, (size_t)s->channels * HB_TAPS * sizeof(double));
    s->histPos = 0;
    s->phase = 0;
}

static void stageDestroy(HalfbandStage *s) {
    if (!s) return;
    free(s->hist);
    delete s;
}

/**
 * Feed inFrames of interleaved doubles, emit ~inFrames/2 to out.
 * @return output frame count.
 */
static int stageProcess(HalfbandStage *s, const double *in, int inFrames, double *out) {
    int outFrames = 0;
    const int ch = s->channels;
    for (int f = 0; f < inFrames; f++) {
        // Push one frame into each channel's delay line.
        for (int c = 0; c < ch; c++) {
            s->hist[(size_t)c * HB_TAPS + s->histPos] = in[(size_t)f * ch + c];
        }
        const int wrotePos = s->histPos;
        s->histPos = (s->histPos + 1) % HB_TAPS;
        s->phase ^= 1;
        if (s->phase == 0) continue; // keep every 2nd input frame

        for (int c = 0; c < ch; c++) {
            const double *hist = s->hist + (size_t)c * HB_TAPS;
            double acc = 0.0;
            int idx = wrotePos; // newest sample = coef[0]
            for (int k = 0; k < HB_TAPS; k++) {
                acc += s->coef[k] * hist[idx];
                idx = (idx == 0) ? HB_TAPS - 1 : idx - 1;
            }
            out[(size_t)outFrames * ch + c] = acc;
        }
        outFrames++;
    }
    return outFrames;
}

struct Decimator {
    int channels;
    int factor;
    HalfbandStage *stage1;
    HalfbandStage *stage2;  // only for factor 4
    double *dbuf1;          // input converted to double
    double *dbuf2;          // stage outputs
    double *dbuf3;
    int capacityFrames;     // sizing of dbuf1 (input domain)
};

static bool ensureCapacity(Decimator *d, int inFrames) {
    if (inFrames <= d->capacityFrames) return true;
    const size_t n = (size_t)inFrames * d->channels;
    free(d->dbuf1); free(d->dbuf2); free(d->dbuf3);
    d->dbuf1 = (double *)malloc(n * sizeof(double));
    d->dbuf2 = (double *)malloc((n / 2 + d->channels * 2) * sizeof(double));
    d->dbuf3 = d->factor == 4
            ? (double *)malloc((n / 4 + d->channels * 2) * sizeof(double))
            : nullptr;
    if (!d->dbuf1 || !d->dbuf2 || (d->factor == 4 && !d->dbuf3)) {
        free(d->dbuf1); free(d->dbuf2); free(d->dbuf3);
        d->dbuf1 = d->dbuf2 = d->dbuf3 = nullptr;
        d->capacityFrames = 0;
        return false;
    }
    d->capacityFrames = inFrames;
    return true;
}

Decimator *decimatorCreate(int channels, int factor) {
    if (channels < 1 || channels > 8) return nullptr;
    if (factor != 2 && factor != 4) return nullptr;
    auto *d = new (std::nothrow) Decimator();
    if (!d) return nullptr;
    d->channels = channels;
    d->factor = factor;
    d->stage1 = stageCreate(channels);
    d->stage2 = (factor == 4) ? stageCreate(channels) : nullptr;
    d->dbuf1 = d->dbuf2 = d->dbuf3 = nullptr;
    d->capacityFrames = 0;
    if (!d->stage1 || (factor == 4 && !d->stage2)) {
        decimatorDestroy(d);
        return nullptr;
    }
    return d;
}

void decimatorReset(Decimator *d) {
    if (!d) return;
    stageReset(d->stage1);
    if (d->stage2) stageReset(d->stage2);
}

int decimatorMaxOutFrames(const Decimator *d, int inFrames) {
    if (!d) return 0;
    return inFrames / d->factor + 2;
}

int decimatorProcess(Decimator *d, const int32_t *in, int inFrames, int32_t *out) {
    if (!d || inFrames <= 0) return 0;
    if (!ensureCapacity(d, inFrames)) return 0;

    const size_t n = (size_t)inFrames * d->channels;
    for (size_t i = 0; i < n; i++) d->dbuf1[i] = (double)in[i];

    int frames = stageProcess(d->stage1, d->dbuf1, inFrames, d->dbuf2);
    const double *result = d->dbuf2;
    if (d->factor == 4) {
        frames = stageProcess(d->stage2, d->dbuf2, frames, d->dbuf3);
        result = d->dbuf3;
    }

    const size_t outN = (size_t)frames * d->channels;
    for (size_t i = 0; i < outN; i++) {
        double v = result[i];
        if (v > 2147483647.0) v = 2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        out[i] = (int32_t)llrint(v);
    }
    return frames;
}

void decimatorDestroy(Decimator *d) {
    if (!d) return;
    stageDestroy(d->stage1);
    stageDestroy(d->stage2);
    free(d->dbuf1);
    free(d->dbuf2);
    free(d->dbuf3);
    delete d;
}
