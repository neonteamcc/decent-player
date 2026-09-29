// Runs the production JNI create/start path, feedback decoder and packetizer.
// Only USB ioctl and Android logging are replaced; no hardware is required.
#include <sys/ioctl.h>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <vector>

static int mockIoctl(int, unsigned long, ...);
#define ioctl mockIoctl
#include "../../main/jni/usb-audio-output.cpp"
#undef ioctl

static std::deque<usbdevfs_urb *> pending;
static std::vector<unsigned> packetBytes;
static std::vector<uint8_t> sent;

static int mockIoctl(int, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == USBDEVFS_SUBMITURB) {
        auto *urb = static_cast<usbdevfs_urb *>(arg);
        assert(urb->endpoint == 0x02);
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
    assert(false && "unexpected device operation");
    return -1;
}

static void checkStream(int rate, int busFrames, int interval, int bits,
                        int maxPacket, int expectedPacketsPerUrb, uint32_t feedback = 0,
                        int feedbackLength = 4) {
    const auto handle = Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioCreate(
            nullptr, nullptr, -1, 1, 0x02, 0, rate, 2, bits, maxPacket,
            busFrames, feedbackLength, 0, interval, 0);
    assert(handle);
    auto *ctx = reinterpret_cast<UsbAudioContext *>(handle);
    assert(ctx->serviceInterval == (1 << (interval - 1)));
    assert(ctx->packetsPerUrb == expectedPacketsPerUrb);
    assert(ctx->packetsPerUrb * maxPacket <= USB_AUDIO_URB_BUFFER_SIZE);
    assert(Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStart(nullptr, nullptr, handle));
    if (feedback) {
        uint8_t bytes[] = {uint8_t(feedback), uint8_t(feedback >> 8),
                           uint8_t(feedback >> 16), uint8_t(feedback >> 24)};
        ctx->calibratedFpmf = decodeFeedback(ctx, bytes, feedbackLength);
        assert(ctx->calibratedFpmf > 0);
    }
    const double framesPerPacket = ctx->calibratedFpmf * ctx->serviceInterval;
    // More than one second of PCM, so checking a complete second never
    // depends on the last partially filled URB or on a buffer boundary.
    std::vector<uint8_t> pcm((rate * 2 + 100) * ctx->bytesPerFrame);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = uint8_t(i * 17 + i / 251);
    submitPcmToUrbs(ctx, pcm.data(), int(pcm.size()));
    const int packetsInSecond = busFrames / ctx->serviceInterval;
    assert(packetBytes.size() >= size_t(packetsInSecond));
    int framesInSecond = 0;
    for (int i = 0; i < packetsInSecond; ++i) {
        assert(packetBytes[i] <= unsigned(maxPacket));
        assert(packetBytes[i] % ctx->bytesPerFrame == 0);
        const int frames = packetBytes[i] / ctx->bytesPerFrame;
        assert(frames == int(std::floor(framesPerPacket)) ||
               frames == int(std::ceil(framesPerPacket)));
        framesInSecond += frames;
    }
    const double expected = ctx->calibratedFpmf * busFrames;
    assert(std::abs(framesInSecond - expected) < 1.01);
    assert(sent.size() + ctx->residualBytes == pcm.size());
    assert(std::equal(sent.begin(), sent.end(), pcm.begin()));
    assert(std::equal(ctx->residualBuffer, ctx->residualBuffer + ctx->residualBytes,
                      pcm.begin() + sent.size()));
    printf("rate=%d bus=%d interval=%d packets/s=%d frames/s=%d feedback=%u OK\n",
           rate, busFrames, interval, packetsInSecond, framesInSecond, feedback);
    // Finish must submit the residual tail and reap every audio URB. Padding
    // is allowed only after the final real PCM byte, in the last USB packet.
    assert(Java_com_decent_usbaudio_UsbAudioStream_nativeFinish(nullptr, nullptr, handle));
    assert(ctx->residualBytes == 0);
    assert(ctx->urbsInFlight == 0);
    assert(ctx->running.load());
    assert(pending.empty());
    assert(sent.size() >= pcm.size());
    assert(std::equal(pcm.begin(), pcm.end(), sent.begin()));
    assert(std::all_of(sent.begin() + pcm.size(), sent.end(), [](uint8_t b) { return b == 0; }));
    Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioDestroy(nullptr, nullptr, handle);
    assert(pending.empty());
    packetBytes.clear();
    sent.clear();
}

int main() {
    // Cayin: captured doubled-Q16.16 feedback and standard Q16.16.
    checkStream(44100, 8000, 2, 32, 1024, 4, 722878);
    checkStream(44100, 8000, 2, 32, 1024, 4, 361267);
    for (int rate : {44100, 48000, 96000, 192000, 384000})
        checkStream(rate, 8000, 2, 32, 1024, 4);
    // Reference high-speed devices retain their previous packet/ring geometry.
    checkStream(44100, 8000, 1, 32, 776, 4, 361267); // KA17 / DAWN
    checkStream(384000, 8000, 1, 32, 400, 8);       // DS2 / KA11
    checkStream(48000, 8000, 1, 32, 400, 8, 786432); // shifted DS2 feedback
    // Existing full-speed UAC1 geometry and 3-byte feedback.
    checkStream(48000, 1000, 1, 24, 294, 2, 786432, 3); // SB X-Fi
    checkStream(44100, 1000, 1, 16, 180, 2);           // BTR13
    // Derived intervals: packet duration, not bus speed alone, sets pacing.
    checkStream(48000, 8000, 4, 16, 196, 1);
    checkStream(48000, 1000, 2, 16, 388, 1);
    checkStream(44100, 8000, 1, 32, 200, 8); // KA17 captured 192k-limited configuration
    checkStream(96000, 8000, 1, 32, 200, 8);
    puts("USB packet timing and final drain: 16 cases passed");
}
