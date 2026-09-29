// Host regression test: link with resampler.cpp and decimator.cpp.
#include "resampler.h"
#include "decimator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Filter {
    RationalResampler *r = nullptr;
    Decimator *d = nullptr;
    const int channels;

    Filter(int factor, int inputRate, int outputRate, int channels = 2)
        : channels(channels) {
        if (factor) d = decimatorCreate(channels, factor);
        else r = resamplerCreate(channels, inputRate, outputRate);
        assert(r || d);
    }
    ~Filter() { resamplerDestroy(r); decimatorDestroy(d); }
    void reset() { resamplerReset(r); decimatorReset(d); }
    int capacity(int frames) const {
        return r ? resamplerMaxOutFrames(r, frames) : decimatorMaxOutFrames(d, frames);
    }
    int process(const float *in, int frames, float *out) {
        return r ? resamplerProcessFloat(r, in, frames, out)
                 : decimatorProcessFloat(d, in, frames, out);
    }
    int process(const int32_t *in, int frames, int32_t *out) {
        return r ? resamplerProcess(r, in, frames, out)
                 : decimatorProcess(d, in, frames, out);
    }
    std::vector<float> run(const std::vector<float> &in, int chunk) {
        std::vector<float> result;
        for (int offset = 0; offset < (int)in.size() / channels; offset += chunk) {
            const int frames = std::min(chunk, (int)in.size() / channels - offset);
            std::vector<float> out(capacity(frames) * channels + 1, 99.0f);
            const int produced = process(in.data() + offset * channels, frames, out.data());
            assert(produced >= 0 && produced <= capacity(frames));
            assert(out.back() == 99.0f);
            result.insert(result.end(), out.begin(), out.begin() + produced * channels);
        }
        return result;
    }
};

static void checkFilter(int factor, int inputRate, int outputRate) {
    constexpr int frames = 4096;
    Filter filter(factor, inputRate, outputRate);
    std::vector<float> dc(frames * 2);
    for (int f = 0; f < frames; ++f) { dc[f * 2] = 2.25f; dc[f * 2 + 1] = -1.75f; }
    auto output = filter.run(dc, frames);
    for (size_t i = output.size() / 2 / 2 * 2; i < output.size(); i += 2) {
        assert(std::fabs(output[i] - 2.25f) < 0.00002f);
        assert(std::fabs(output[i + 1] + 1.75f) < 0.00002f);
    }
    filter.reset();
    assert(filter.run(dc, 17) == output); // odd chunks preserve phase and history
    filter.reset();
    assert(filter.run(dc, 1) == output);

    std::vector<float> impulse(frames * 2, 0.0f);
    impulse[0] = 0.25f;
    filter.reset();
    auto lowImpulse = filter.run(impulse, 31);
    impulse[0] = 4.0f;
    filter.reset();
    auto highImpulse = filter.run(impulse, 31);
    float peak = 0;
    for (size_t i = 0; i < highImpulse.size(); ++i) {
        assert(highImpulse[i] == lowImpulse[i] * 16.0f);
        if (i % 2) assert(highImpulse[i] == 0.0f); // independent right channel
        peak = std::max(peak, std::fabs(highImpulse[i]));
    }
    assert(peak > 0.01f); // a silent/broken filter cannot satisfy scaling alone

    std::vector<float> signal(frames * 2);
    std::vector<int32_t> integer(signal.size());
    for (int f = 0; f < frames; ++f) {
        // Multiples of 256 have exact float and canonical representations.
        integer[f * 2] = (int32_t)(std::sin(f * 0.067) * 2000000) * 256;
        integer[f * 2 + 1] = f == 0 ? 536870912 : 0;
        signal[f * 2] = (float)((double)integer[f * 2] / 2147483648.0);
        signal[f * 2 + 1] = (float)((double)integer[f * 2 + 1] / 2147483648.0);
    }
    filter.reset();
    auto reference = filter.run(signal, frames);
    filter.reset();
    std::vector<int32_t> integerOut(filter.capacity(frames) * 2);
    int count = filter.process(integer.data(), frames, integerOut.data());
    assert(count * 2 == (int)reference.size());
    for (int i = 0; i < count * 2; ++i)
        assert(std::fabs(reference[i] - integerOut[i] / 2147483648.0) < 0.00000004);

    // Alternating APIs must retain a single canonical history and rate phase.
    filter.reset();
    int total = 0;
    for (int offset = 0, call = 0; offset < frames; offset += 17, ++call) {
        const int inputCount = std::min(17, frames - offset);
        if (call % 2 == 0) {
            std::vector<int32_t> out(filter.capacity(inputCount) * 2);
            count = filter.process(integer.data() + offset * 2, inputCount, out.data());
            for (int i = 0; i < count * 2; ++i)
                assert(std::fabs(reference[total * 2 + i] - out[i] / 2147483648.0) < 0.00000004);
        } else {
            std::vector<float> out(filter.capacity(inputCount) * 2);
            count = filter.process(signal.data() + offset * 2, inputCount, out.data());
            for (int i = 0; i < count * 2; ++i) assert(reference[total * 2 + i] == out[i]);
        }
        total += count;
    }
    assert(total * 2 == (int)reference.size());
    filter.reset();
    assert(filter.run(signal, frames) == reference);
    assert(filter.process((const float *)nullptr, 0, (float *)nullptr) == 0);
    assert(filter.process((const float *)nullptr, -1, (float *)nullptr) == 0);
    std::printf("PASS float SRC factor=%d rate=%d->%d\n", factor, inputRate, outputRate);
}

int main() {
    checkFilter(2, 0, 0);
    checkFilter(4, 0, 0);
    checkFilter(0, 44100, 48000);
    checkFilter(0, 48000, 44100);
    checkFilter(0, 88200, 48000);
    assert(resamplerProcessFloat(nullptr, nullptr, 1, nullptr) == 0);
    assert(decimatorProcessFloat(nullptr, nullptr, 1, nullptr) == 0);
}
