/**
 * @file resampler.cpp
 * @brief Polyphase rational resampler. See resampler.h.
 */

#include "resampler.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

// Stopband attenuation target and the matching Kaiser beta
// (beta = 0.1102 * (A - 8.7) for A > 50 dB).
static const double RS_ATTEN_DB = 100.0;
static const double RS_BETA = 0.1102 * (RS_ATTEN_DB - 8.7);

// Passband edge as a fraction of the narrower Nyquist. 0.907 keeps
// 44.1↔48 conversions flat to 20 kHz.
static const double RS_PASSBAND = 0.907;

// Reduced L/M above this is pathological (not a real audio rate pair).
static const int RS_MAX_FACTOR = 512;

// Taps per polyphase branch is derived from the Kaiser length estimate;
// cap it so a weird ratio can't explode CPU/memory.
static const int RS_MAX_TAPS_PER_PHASE = 512;

struct RationalResampler {
    int channels;
    int L;              // interpolation factor (output side)
    int M;              // decimation factor (input side)
    int tapsPerPhase;
    double *coef;       // L * tapsPerPhase, phase-major: coef[p*tpp + k] = h[p + k*L]
    double *hist;       // channels * tapsPerPhase circular input history
    int histPos;        // next write slot in each channel's history
    int64_t inCount;    // total input frames consumed
    int64_t outCount;   // total output frames produced
};

static double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; k++) {
        double f = x / (2.0 * k);
        term *= f * f;
        sum += term;
        if (term < 1e-21 * sum) break;
    }
    return sum;
}

static int gcdInt(int a, int b) {
    while (b != 0) { int t = a % b; a = b; b = t; }
    return a;
}

RationalResampler *resamplerCreate(int channels, int inputRate, int outputRate) {
    if (channels < 1 || channels > 8) return nullptr;
    if (inputRate <= 0 || outputRate <= 0 || inputRate == outputRate) return nullptr;
    const int g = gcdInt(inputRate, outputRate);
    const int L = outputRate / g;
    const int M = inputRate / g;
    if (L > RS_MAX_FACTOR || M > RS_MAX_FACTOR) return nullptr;

    // Lowpass in the upsampled (L·fs_in) domain: cutoff at the narrower
    // Nyquist = π/max(L,M); transition from RS_PASSBAND·cutoff to cutoff.
    const int maxLM = (L > M) ? L : M;
    const double wStop = M_PI / maxLM;
    const double wPass = RS_PASSBAND * wStop;
    const double deltaW = wStop - wPass;
    // Kaiser length estimate: N ≈ (A - 7.95) / (2.285 · Δω)
    int totalTaps = (int)ceil((RS_ATTEN_DB - 7.95) / (2.285 * deltaW));
    int tpp = (totalTaps + L - 1) / L;
    if (tpp < 8) tpp = 8;
    if (tpp > RS_MAX_TAPS_PER_PHASE) return nullptr;
    totalTaps = tpp * L;

    auto *r = new (std::nothrow) RationalResampler();
    if (!r) return nullptr;
    r->channels = channels;
    r->L = L;
    r->M = M;
    r->tapsPerPhase = tpp;
    r->coef = (double *)malloc((size_t)totalTaps * sizeof(double));
    r->hist = (double *)calloc((size_t)channels * tpp, sizeof(double));
    if (!r->coef || !r->hist) {
        free(r->coef); free(r->hist);
        delete r;
        return nullptr;
    }
    r->histPos = 0;
    r->inCount = 0;
    r->outCount = 0;

    // Windowed-sinc prototype, cutoff centered between passband and
    // stopband edges; gain L compensates the zero-stuffing loss.
    const double wc = 0.5 * (wPass + wStop);
    const double center = (totalTaps - 1) / 2.0;
    const double i0b = besselI0(RS_BETA);
    double *proto = (double *)malloc((size_t)totalTaps * sizeof(double));
    if (!proto) {
        resamplerDestroy(r);
        return nullptr;
    }
    for (int n = 0; n < totalTaps; n++) {
        const double t = n - center;
        const double ideal = (fabs(t) < 1e-12)
                ? wc / M_PI
                : sin(wc * t) / (M_PI * t);
        const double x = t / center; // [-1, 1]
        const double w = besselI0(RS_BETA * sqrt(1.0 - x * x)) / i0b;
        proto[n] = ideal * w;
    }
    // Normalize DC gain of the polyphase sum to exactly L (each output
    // sample sums one branch; each branch must have unity DC gain).
    double dc = 0.0;
    for (int n = 0; n < totalTaps; n++) dc += proto[n];
    const double scale = (double)L / dc;
    // Re-layout phase-major for cache-friendly per-output dot products:
    // branch p convolves x[base - k] with h[p + k·L].
    for (int p = 0; p < L; p++) {
        for (int k = 0; k < tpp; k++) {
            const int n = p + k * L;
            r->coef[(size_t)p * tpp + k] = (n < totalTaps) ? proto[n] * scale : 0.0;
        }
    }
    free(proto);
    return r;
}

void resamplerReset(RationalResampler *r) {
    if (!r) return;
    memset(r->hist, 0, (size_t)r->channels * r->tapsPerPhase * sizeof(double));
    r->histPos = 0;
    r->inCount = 0;
    r->outCount = 0;
}

int resamplerMaxOutFrames(const RationalResampler *r, int inFrames) {
    if (!r) return 0;
    return (int)(((int64_t)inFrames * r->L) / r->M) + 2;
}

int resamplerProcess(RationalResampler *r, const int32_t *in, int inFrames, int32_t *out) {
    if (!r || inFrames <= 0) return 0;
    const int ch = r->channels;
    const int tpp = r->tapsPerPhase;
    int outFrames = 0;

    for (int f = 0; f < inFrames; f++) {
        // Push one frame into each channel's circular history.
        for (int c = 0; c < ch; c++) {
            r->hist[(size_t)c * tpp + r->histPos] = (double)in[(size_t)f * ch + c];
        }
        const int newestPos = r->histPos;
        r->histPos = (r->histPos + 1) % tpp;
        const int64_t newestIdx = r->inCount; // absolute index of the frame just pushed
        r->inCount++;

        // Emit every output whose source anchor is the frame just pushed:
        // output n sits at upsampled position u = n·M; its anchor input is
        // base = u / L, filtered backward over tpp history frames.
        while (true) {
            const int64_t u = r->outCount * (int64_t)r->M;
            const int64_t base = u / r->L;
            if (base > newestIdx) break;      // anchor not received yet
            const int phase = (int)(u % r->L);
            const double *h = r->coef + (size_t)phase * tpp;
            for (int c = 0; c < ch; c++) {
                const double *hist = r->hist + (size_t)c * tpp;
                double acc = 0.0;
                int idx = newestPos;          // hist[newestPos] == x[newestIdx] == x[base]
                for (int k = 0; k < tpp; k++) {
                    acc += h[k] * hist[idx];
                    idx = (idx == 0) ? tpp - 1 : idx - 1;
                }
                double v = acc;
                if (v > 2147483647.0) v = 2147483647.0;
                if (v < -2147483648.0) v = -2147483648.0;
                out[(size_t)outFrames * ch + c] = (int32_t)llrint(v);
            }
            outFrames++;
            r->outCount++;
        }
    }
    return outFrames;
}

void resamplerDestroy(RationalResampler *r) {
    if (!r) return;
    free(r->coef);
    free(r->hist);
    delete r;
}
