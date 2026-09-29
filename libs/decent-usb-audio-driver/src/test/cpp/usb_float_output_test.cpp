// Exercises production JNI dispatch, float processing and USB packetization.
// Only Java array access, USB ioctl and Android logging are replaced.
// Linux host build, from the repository root (requires a JDK and Linux headers):
// c++ -std=c++11 -Wall -Werror -fsanitize=address,undefined \
//   -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
//   -Ilibs/decent-usb-audio-driver/src/test/cpp/stubs \
//   libs/decent-usb-audio-driver/src/test/cpp/usb_float_output_test.cpp \
//   libs/decent-usb-audio-driver/src/main/jni/{usb-float-processing,resampler,decimator}.cpp \
//   -o /tmp/usb_float_output_test && /tmp/usb_float_output_test
#include <sys/ioctl.h>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

static int mockIoctl(int, unsigned long, ...);
#define ioctl mockIoctl
#include "../../main/jni/usb-audio-output.cpp"
#undef ioctl

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string(__func__) + ": " + #expression + " at line " + std::to_string(__LINE__)); } while (0)

static std::deque<usbdevfs_urb *> pending;
static std::vector<unsigned> packetBytes;
static std::vector<uint8_t> sent;
static int floatReads = 0, byteReads = 0, releases = 0;

static int mockIoctl(int, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == USBDEVFS_SUBMITURB) {
        auto *urb = static_cast<usbdevfs_urb *>(arg);
        CHECK(urb->endpoint == 0x02);
        CHECK(std::find(pending.begin(), pending.end(), urb) == pending.end());
        for (int i = 0; i < urb->number_of_packets; ++i)
            packetBytes.push_back(urb->iso_frame_desc[i].length);
        auto *data = static_cast<uint8_t *>(urb->buffer);
        sent.insert(sent.end(), data, data + urb->buffer_length);
        pending.push_back(urb);
        return 0;
    }
    if (request == USBDEVFS_REAPURBNDELAY) {
        if (pending.empty()) { errno = EAGAIN; return -1; }
        *static_cast<usbdevfs_urb **>(arg) = pending.front();
        pending.pop_front();
        return 0;
    }
    if (request == USBDEVFS_DISCARDURB) return 0;
    CHECK(false);
    return -1;
}

struct TestArray { void *data; int size; bool floats; };
static jsize JNICALL arrayLength(JNIEnv *, jarray array) {
    return reinterpret_cast<TestArray *>(array)->size;
}
static jfloat *JNICALL floatElements(JNIEnv *, jfloatArray array, jboolean *) {
    auto *a = reinterpret_cast<TestArray *>(array);
    CHECK(a->floats);
    ++floatReads;
    return static_cast<float *>(a->data);
}
static jbyte *JNICALL byteElements(JNIEnv *, jbyteArray array, jboolean *) {
    auto *a = reinterpret_cast<TestArray *>(array);
    CHECK(!a->floats);
    ++byteReads;
    return static_cast<jbyte *>(a->data);
}
static void JNICALL releaseFloats(JNIEnv *, jfloatArray array, jfloat *data, jint mode) {
    CHECK(reinterpret_cast<TestArray *>(array)->data == data && mode == JNI_ABORT);
    ++releases;
}
static void JNICALL releaseBytes(JNIEnv *, jbyteArray array, jbyte *data, jint mode) {
    CHECK(reinterpret_cast<TestArray *>(array)->data == data && mode == JNI_ABORT);
    ++releases;
}

struct Stream {
    jlong handle;
    UsbAudioContext *ctx;
    JNINativeInterface_ table{};
    JNIEnv env{};
    Stream(int bits, int wire, int rate = 48000, int inputRate = 0, int validBits = 0) {
        CHECK(pending.empty());
        packetBytes.clear(); sent.clear();
        handle = Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioCreate(
            nullptr, nullptr, -1, 1, 0x02, 0, rate, 2, bits, 1024, 8000, 4,
            inputRate, 2, wire, validBits);
        CHECK(handle);
        ctx = reinterpret_cast<UsbAudioContext *>(handle);
        CHECK(!ctx->limiterEnabled.load());
        CHECK(Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStart(nullptr, nullptr, handle));
        table.GetArrayLength = arrayLength;
        table.GetFloatArrayElements = floatElements;
        table.GetByteArrayElements = byteElements;
        table.ReleaseFloatArrayElements = releaseFloats;
        table.ReleaseByteArrayElements = releaseBytes;
        env.functions = &table;
    }
    ~Stream() { Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioDestroy(nullptr, nullptr, handle); }
    void limiter(bool enabled) {
        Java_com_decent_usbaudio_UsbAudioStream_nativeSetLimiterEnabled(
            nullptr, nullptr, handle, enabled ? JNI_TRUE : JNI_FALSE);
    }
    void write(std::vector<float> &pcm, int chunk) {
        const auto original = pcm;
        const int readsBefore = floatReads, releasesBefore = releases;
        int calls = 0;
        for (int offset = 0; offset < int(pcm.size()) / 2; offset += chunk) {
            const int frames = std::min(chunk, int(pcm.size()) / 2 - offset);
            TestArray a{pcm.data() + offset * 2, frames * 2, true};
            Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWrite(
                &env, nullptr, handle, reinterpret_cast<jfloatArray>(&a));
            CHECK(ctx->running.load());
            ++calls;
        }
        CHECK(floatReads - readsBefore == calls && releases - releasesBefore == calls);
        CHECK(std::memcmp(pcm.data(), original.data(), pcm.size() * sizeof(float)) == 0);
    }
    void writeRaw(std::vector<uint8_t> &pcm, int inputBits, int chunk) {
        const auto original = pcm;
        const int bpf = inputBits / 8 * 2;
        const int readsBefore = byteReads, releasesBefore = releases;
        int calls = 0;
        for (int offset = 0; offset < int(pcm.size()) / bpf; offset += chunk) {
            const int frames = std::min(chunk, int(pcm.size()) / bpf - offset);
            TestArray a{pcm.data() + offset * bpf, frames * bpf, false};
            Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWriteRaw(
                &env, nullptr, handle, reinterpret_cast<jbyteArray>(&a), inputBits);
            CHECK(ctx->running.load());
            ++calls;
        }
        CHECK(byteReads - readsBefore == calls && releases - releasesBefore == calls);
        CHECK(pcm == original);
    }
    std::vector<uint8_t> finish(int frames) {
        CHECK(ctx->framesWritten == frames);
        const size_t bytes = size_t(frames) * ctx->bytesPerFrame;
        CHECK(sent.size() + ctx->residualBytes == bytes);
        CHECK(Java_com_decent_usbaudio_UsbAudioStream_nativeFinish(nullptr, nullptr, handle));
        CHECK(ctx->residualBytes == 0 && ctx->urbsInFlight == 0 && pending.empty());
        CHECK(sent.size() >= bytes);
        CHECK(std::all_of(sent.begin() + bytes, sent.end(), [](uint8_t b) { return b == 0; }));
        double accumulator = 0;
        for (unsigned packet : packetBytes) {
            accumulator += ctx->calibratedFpmf * ctx->serviceInterval;
            const int n = int(accumulator);
            accumulator -= n;
            CHECK(packet == unsigned(n * ctx->bytesPerFrame));
            CHECK(packet <= unsigned(ctx->maxPacketSize));
        }
        return std::vector<uint8_t>(sent.begin(), sent.begin() + bytes);
    }
};

static std::vector<float> floats(const std::vector<uint8_t> &bytes) {
    CHECK(bytes.size() % 4 == 0);
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}
static uint32_t wordAt(const std::vector<uint8_t> &bytes, size_t offset, int width = 4) {
    uint32_t word = 0;
    for (int b = 0; b < width; ++b) word |= uint32_t(bytes[offset + b]) << (8 * b);
    return word;
}
static std::vector<uint8_t> words(const std::vector<int32_t> &samples, int bits) {
    std::vector<uint8_t> result;
    for (int32_t sample : samples)
        for (int b = 0; b < bits / 8; ++b) result.push_back(uint32_t(sample) >> (8 * b));
    return result;
}

static void identity() {
    std::vector<float> pcm(5003 * 2);
    const float pattern[] = {1.25f, -1.25f, -0.0f, 0.0f, 0.125f, -0.5f};
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = pattern[i % 6];
    std::vector<unsigned> referenceCadence;
    for (int chunk : {5003, 1, 7, 13, 257, 509}) {
        Stream stream(32, 1, 44100);
        stream.write(pcm, chunk);
        auto out = stream.finish(5003);
        CHECK(out.size() == pcm.size() * 4);
        CHECK(std::memcmp(out.data(), pcm.data(), out.size()) == 0);
        if (referenceCadence.empty()) referenceCadence = packetBytes;
        else CHECK(packetBytes == referenceCadence);
    }
}
static void gainBeforeClamp() {
    std::vector<float> pcm(4097 * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? -1.5f : 1.5f;
    for (bool limiter : {false, true}) for (int bits : {16, 24, 32}) {
        Stream stream(bits, 0);
        stream.ctx->softGain.store(0.5f);
        stream.limiter(limiter);
        stream.write(pcm, 13);
        const auto out = stream.finish(4097);
        const uint32_t positive = uint32_t(3) << (bits - 3);
        const uint32_t negative = uint32_t(-int32_t(positive));
        const uint32_t mask = bits == 32 ? UINT32_MAX : (uint32_t(1) << bits) - 1;
        for (size_t i = 0; i < pcm.size(); ++i) {
            const uint32_t expected = (i % 2 ? negative : positive) & mask;
            const uint32_t actual = wordAt(out, i * (bits / 8), bits / 8);
            if (bits == 16) {
                // Software attenuation enables final PCM16 TPDF dither.
                CHECK(std::abs(int(int16_t(actual)) - int(int16_t(expected))) <= 1);
            } else CHECK(actual == expected);
        }
    }
}
static void rawBypass() {
    std::vector<int32_t> pcm(4097 * 2);
    uint32_t state = 1;
    for (auto &sample : pcm) { state = state * 1664525u + 1013904223u; sample = int32_t(state); }
    auto input = words(pcm, 32);
    for (bool limiter : {false, true}) for (int chunk : {1, 7, 13, 257, 509}) {
        Stream stream(32, 0);
        stream.limiter(limiter);
        stream.writeRaw(input, 32, chunk);
        CHECK(stream.finish(4097) == input);
    }
}
static void rawNormalize() {
    for (int bits : {16, 24}) {
        const int32_t maximum = (int32_t(1) << (bits - 1)) - 1;
        const int32_t minimum = -(int32_t(1) << (bits - 1));
        std::vector<int32_t> pcm(4097 * 2);
        const int32_t pattern[] = {minimum, maximum, 0, 1, -1, maximum / 2};
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = pattern[i % 6];
        auto input = words(pcm, bits);
        Stream stream(32, 1);
        stream.writeRaw(input, bits, 7);
        const auto out = floats(stream.finish(4097));
        for (size_t i = 0; i < pcm.size(); ++i)
            CHECK(out[i] == float(double(pcm[i]) / std::ldexp(1.0, bits - 1)));
    }
}
static void srcHeadroom() {
    constexpr int frames = 8193;
    std::vector<float> pcm(frames * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? -1.5f : 1.5f;
    for (int inputRate : {96000, 192000, 44100}) {
        std::vector<uint8_t> reference;
        for (int chunk : {frames, 1, 7, 13, 257, 509}) {
            Stream stream(32, 1, 48000, inputRate);
            stream.ctx->softGain.store(0.5f);
            stream.write(pcm, chunk);
            const int outFrames = int((int64_t(frames) * 48000 + inputRate - 1) / inputRate);
            auto result = stream.finish(outFrames);
            if (reference.empty()) reference = result;
            else CHECK(result == reference);
            auto out = floats(result);
            for (size_t i = out.size() / 2 / 2 * 2; i < out.size(); i += 2) {
                CHECK(std::fabs(out[i] - 0.75f) < 0.00001f);
                CHECK(std::fabs(out[i + 1] + 0.75f) < 0.00001f);
            }
        }
    }
}
static void linkedLimiter() {
    constexpr int rate = 48000, frames = rate;
    for (double frequency : {20.0, 23.0, 60.0}) {
        std::vector<float> pcm(frames * 2);
        for (int f = 0; f < frames; ++f) {
            pcm[f * 2] = float(1.5 * std::sin(2 * M_PI * frequency * f / rate));
            pcm[f * 2 + 1] = 0.25f;
        }
        std::vector<uint8_t> reference;
        for (int chunk : {frames, 1, 7, 13, 257, 509}) {
            Stream stream(32, 1);
            stream.limiter(true);
            stream.write(pcm, chunk);
            auto result = stream.finish(frames);
            if (reference.empty()) reference = result;
            else CHECK(result == reference);
            auto out = floats(result);
            for (int f = 0; f < frames; ++f) {
                CHECK(std::fabs(out[f * 2]) <= 1.000001f);
                const double gain = out[f * 2 + 1] / 0.25;
                CHECK(std::fabs(out[f * 2] - pcm[f * 2] * gain) < 0.0000002);
                if (f > rate / 5) CHECK(std::fabs(gain - 2.0 / 3.0) < 0.00002);
            }
        }
    }
}
static void limiterRecovery() {
    Stream stream(32, 1);
    stream.limiter(true);
    std::vector<float> pcm(48000 * 2, 0.25f);
    pcm[0] = 2.0f;
    stream.write(pcm, 257);
    auto out = floats(stream.finish(48000));
    CHECK(out[0] == 1.0f && out[1] == 0.125f);
    CHECK(std::fabs(out.back() - 0.25f) < 0.000001f);
}
static void limiterReset() {
    Stream stream(32, 1);
    stream.limiter(true);
    std::vector<float> peak{2.0f, 0.25f};
    stream.write(peak, 1);
    stream.finish(1);
    Java_com_decent_usbaudio_UsbAudioStream_nativeFlush(nullptr, nullptr, stream.handle);
    packetBytes.clear(); sent.clear();
    std::vector<float> pcm(100 * 2, 0.25f);
    stream.write(pcm, 7);
    auto out = floats(stream.finish(100));
    CHECK(out == pcm);
}
static void limiterRawRecovery() {
    Stream stream(32, 0);
    stream.limiter(true);
    std::vector<float> peak{2.0f, 0.25f};
    stream.write(peak, 1);
    std::vector<int32_t> pcm(96000 * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = i % 2 ? -536870913 : 536870913;
    auto input = words(pcm, 32);
    stream.writeRaw(input, 32, 257);
    auto out = stream.finish(96001);
    CHECK(stream.ctx->limiter.isUnity());
    CHECK(std::equal(input.end() - 4096 * 8, input.end(), out.end() - 4096 * 8));
}
static void limiterToggle() {
    Stream stream(32, 1);
    stream.limiter(true);
    std::vector<float> peak{2.0f, 0.25f};
    stream.write(peak, 1);
    stream.limiter(false);
    std::vector<float> pcm(101 * 2, -1.25f);
    stream.write(pcm, 7);
    stream.limiter(true);
    std::vector<float> quiet(100 * 2, 0.25f);
    stream.write(quiet, 13);
    auto out = floats(stream.finish(202));
    CHECK(std::equal(pcm.begin(), pcm.end(), out.begin() + 2));
    CHECK(std::equal(quiet.begin(), quiet.end(), out.end() - quiet.size()));
}
static void valid24() {
    std::vector<float> pcm(4097 * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = float(std::sin(i * 0.17) * 1.25);
    {
        Stream stream(32, 0, 48000, 0, 24);
        stream.write(pcm, 13);
        auto out = stream.finish(4097);
        for (size_t i = 0; i < pcm.size(); ++i) {
            CHECK(out[i * 4] == 0);
            const double scaled = std::max(-8388608.0, std::min(8388607.0, double(pcm[i]) * 8388608.0));
            CHECK(wordAt(out, i * 4) == (uint32_t(int32_t(scaled)) << 8));
        }
    }
    for (int bits : {24, 32}) {
        std::vector<int32_t> raw(4097 * 2);
        uint32_t state = 1;
        for (auto &sample : raw) { state = state * 1664525u + 1013904223u; sample = int32_t(state); }
        auto input = words(raw, bits);
        Stream stream(32, 0, 48000, 0, 24);
        stream.writeRaw(input, bits, 7);
        auto out = stream.finish(4097);
        for (size_t i = 0; i < raw.size(); ++i) {
            const uint32_t expected = bits == 24 ? uint32_t(raw[i]) << 8 : uint32_t(raw[i]) & 0xffffff00u;
            CHECK(out[i * 4] == 0);
            CHECK(wordAt(out, i * 4) == expected);
        }
    }
}

static void checkHalfLsbMean(const std::vector<uint8_t> &out, int sign) {
    // The mean must retain half an LSB instead of truncating every sample to zero.
    const size_t first = out.size() / 2 / 2 * 2;
    int64_t sum = 0;
    int zeros = 0, nonzeros = 0;
    for (size_t offset = first; offset < out.size(); offset += 2) {
        const int sample = int16_t(wordAt(out, offset, 2));
        CHECK(sample == 0 || sample == sign);
        if (sample) ++nonzeros; else ++zeros;
        sum += sample;
    }
    CHECK(zeros > 0 && nonzeros > 0);
    const double mean = double(sum) / (zeros + nonzeros);
    CHECK(std::fabs(mean - sign * 0.5) < 0.02);
}
static void gainDither() {
    constexpr int frames = 16384;
    for (int sign : {-1, 1}) {
        std::vector<int32_t> samples(frames * 2, sign);
        auto pcm = words(samples, 16);
        std::vector<uint8_t> reference;
        for (int chunk : {frames, 7, 257}) {
            Stream stream(16, 0);
            stream.ctx->softGain.store(0.5f);
            const auto initialDither = stream.ctx->ditherState;
            stream.writeRaw(pcm, 16, chunk);
            auto out = stream.finish(frames);
            CHECK(stream.ctx->ditherState != initialDither);
            checkHalfLsbMean(out, sign);
            if (reference.empty()) reference = out;
            else CHECK(out == reference);
        }
    }
}
static void srcDither() {
    constexpr int frames = 16385;
    for (int inputRate : {96000, 192000, 44100}) {
        std::vector<float> pcm(frames * 2, 0.5f / 32768.0f);
        std::vector<uint8_t> reference;
        for (int chunk : {frames, 7, 257}) {
            Stream stream(16, 0, 48000, inputRate);
            const auto initialDither = stream.ctx->ditherState;
            stream.write(pcm, chunk);
            const int outFrames = int((int64_t(frames) * 48000 + inputRate - 1) / inputRate);
            auto out = stream.finish(outFrames);
            CHECK(stream.ctx->ditherState != initialDither);
            checkHalfLsbMean(out, 1);
            if (reference.empty()) reference = out;
            else CHECK(out == reference);
        }
    }
}
static void safeFloatPcm16() {
    constexpr int frames = 4097;
    const float pattern[] = {0.125f, -0.75f, 0.5f / 32768.0f, -0.5f / 32768.0f, 0.0f, -0.0f};
    std::vector<float> pcm(frames * 2);
    std::vector<int32_t> expectedSamples(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = pattern[i % 6];
        expectedSamples[i] = int32_t(pcm[i] * 32768.0f);
    }
    const auto expected = words(expectedSamples, 16);
    for (bool limiter : {false, true}) for (int chunk : {frames, 1, 13, 509}) {
        Stream stream(16, 0);
        stream.limiter(limiter);
        const auto initialDither = stream.ctx->ditherState;
        stream.write(pcm, chunk);
        CHECK(stream.finish(frames) == expected);
        CHECK(stream.ctx->ditherState == initialDither);
    }
}

int main(int argc, char **argv) {
    struct Test { const char *name; void (*run)(); };
    const Test tests[] = {{"identity", identity}, {"gain", gainBeforeClamp},
        {"raw-bypass", rawBypass}, {"raw-normalize", rawNormalize},
        {"src-headroom", srcHeadroom}, {"linked-limiter", linkedLimiter},
        {"limiter-recovery", limiterRecovery}, {"limiter-reset", limiterReset},
        {"limiter-raw-recovery", limiterRawRecovery}, {"limiter-toggle", limiterToggle},
        {"valid24", valid24}, {"gain-dither", gainDither},
        {"src-dither", srcDither}, {"safe-float-pcm16", safeFloatPcm16}};
    int count = 0;
    try {
        for (const auto &test : tests) if (argc == 1 || test.name == std::string(argv[1])) {
            test.run(); ++count; std::printf("PASS %s\n", test.name);
        }
        CHECK(count > 0);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
    std::printf("USB float output: %d cases passed\n", count);
}
