/**
 * @file usb-audio-output.h
 * @brief Direct USB audio output context for bit-perfect playback.
 *
 * Pipeline architecture:
 * - Pre-allocated ring buffer of 64 URB slots
 * - Each URB = 8 ISO packets (1ms of audio at high-speed)
 * - URBs complete in FIFO order (ISO_ASAP guarantees this)
 * - No malloc/free during streaming — avoids MTE pointer tag issues
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <linux/usbdevice_fs.h>

/**
 * Maximum isochronous packets per URB submission (compile-time capacity;
 * the runtime count is UsbAudioContext::packetsPerUrb).
 * High-speed: 8 packets × 125 µs microframes = 1 ms of audio per URB.
 * Full-speed: packets are 1 ms frames, so fewer packets per URB keep the
 * submit/reap granularity comparable (2 packets = 2 ms per URB).
 */
#define USB_AUDIO_PACKETS_PER_URB 8

/**
 * Ring capacity (compile-time; runtime count is UsbAudioContext::numUrbs).
 * 80 URBs ≈ 80 ms of in-flight audio at high speed, which empirically gives
 * the xHCI host controller enough scheduling headroom to maintain
 * continuous isochronous output without underruns on commodity Android
 * SoCs. Smaller pipelines (< ~64 URBs at 44.1 kHz) trigger glitches as
 * the ring drains faster than the URB submit/reap cycle can refill it.
 * At full speed the same ~80 ms target is met with 40 URBs × 2 packets.
 */
#define USB_AUDIO_NUM_URBS 80

/**
 * Max bytes per URB data buffer.
 * High-speed worst case: 384kHz × 4 bytes × 2 ch / 8000 microframes
 *   × 8 packets = 3072 bytes per URB.
 * Full-speed worst case: 1023 bytes/frame (USB 2.0 §5.6.3 iso ceiling)
 *   × 2 packets = 2046 bytes per URB.
 * 4096 covers both; the runtime geometry keeps
 * packetsPerUrb × maxPacketSize ≤ this bound.
 */
#define USB_AUDIO_URB_BUFFER_SIZE 4096

/**
 * One slot in the pre-allocated URB ring buffer.
 * The URB struct and data buffer are allocated once at stream creation
 * and reused for the lifetime of the stream. This avoids all MTE
 * pointer tag issues since we never pass malloc'd pointers through
 * the kernel and back.
 */
struct UrbSlot {
    /**
     * The URB struct, heap-allocated with enough trailing space for
     * USB_AUDIO_PACKETS_PER_URB iso_frame_desc entries.
     */
    struct usbdevfs_urb *urb;

    /** Data buffer for PCM audio. Fixed size, pre-allocated. */
    uint8_t *buffer;

    /** Actual number of bytes written to buffer for current submission. */
    int dataLength;
};

/**
 * Aggregate state for one USB audio output stream.
 */
struct UsbAudioContext {
    int fd;
    int interfaceId;
    int endpointOut;
    int endpointFeedback;
    int32_t sampleRate;
    int32_t channelCount;
    int32_t bitDepth;
    int32_t bytesPerSample;
    int32_t bytesPerFrame;
    int32_t maxPacketSize;

    // ── Bus-speed-dependent stream geometry (set at create) ─────
    /**
     * Isochronous service intervals per second: 8000 at high speed
     * (125 µs microframes), 1000 at full speed (1 ms frames). Divides the
     * sample rate into the per-packet frame count.
     */
    int32_t packetsPerSecond;

    /** Packets per URB actually used (≤ USB_AUDIO_PACKETS_PER_URB). */
    int32_t packetsPerUrb;

    /** URB slots actually used (≤ USB_AUDIO_NUM_URBS). */
    int32_t numUrbs;

    /**
     * Auto-detected radix shift for feedback values (ALSA "freqshift"):
     * some devices report in the wrong Q format (off by ×2/×4). INT32_MIN
     * until the first valid feedback packet locks it in.
     */
    int32_t feedbackShift;

    /** LCG state for TPDF dither in the bit-depth reducers. */
    uint32_t ditherState;

    /**
     * ISO packet length for feedback URBs = the feedback endpoint's
     * wMaxPacketSize (3 = full-speed Q10.14, 4 = Q16.16). Requesting more
     * than wMaxPacketSize makes SUBMITURB fail with EMSGSIZE; requesting
     * less risks per-packet overflow errors.
     */
    int32_t feedbackPacketLen;

    std::atomic<bool> running;

    /** Scratch buffer for PCM format conversion (float -> int16/24/32). */
    uint8_t *transferBuffer;
    int32_t transferBufferCapacity;

    int64_t framesWritten;
    bool interfaceClaimed;

    // ── Ring buffer ─────────────────────────────────────────────
    /** Pre-allocated URB slots. Never freed during streaming. */
    UrbSlot ring[USB_AUDIO_NUM_URBS];

    /** Index of next slot to fill and submit (wraps modulo NUM_URBS). */
    int submitIdx;

    /** Index of next slot to reap (wraps modulo NUM_URBS). */
    int reapIdx;

    /** Number of URBs currently submitted and not yet reaped. */
    int urbsInFlight;

    /** Whether ring buffers have been allocated. */
    bool ringAllocated;

    /** Fractional accumulator for sample-rate-to-packet-size conversion. */
    double frameAccumulator;

    /**
     * Frames per isochronous service interval (microframe at high speed,
     * 1 ms frame at full speed), calibrated from the DAC's async feedback
     * endpoint when one exists. More accurate than the nominal
     * sampleRate/packetsPerSecond calculation because it reflects the
     * DAC's actual hardware clock frequency.
     * E.g., nominal 44100Hz at HS = 5.5125, but the DAC may report 5.5127
     * (44101.6Hz). Without calibration, the drift causes periodic pops as
     * the DAC's internal buffer under/overflows. Adaptive/sync devices
     * (no feedback endpoint) run on the nominal value — they recover
     * their clock from our pace, so nominal IS correct for them.
     */
    double calibratedFpmf;

    /**
     * Residual buffer for bytes that didn't align to a complete frame
     * at the end of a write() call. Prepended to the next call's data
     * to avoid micro-discontinuities (pops) at buffer boundaries.
     * Max size = bytesPerFrame - 1 (e.g., 7 bytes for 32-bit stereo).
     */
    uint8_t residualBuffer[USB_AUDIO_URB_BUFFER_SIZE];
    int residualBytes;

    // ── Continuous feedback ─────────────────────────────────────
    /**
     * Dedicated URB for the async feedback endpoint. Submitted at start
     * and continuously recycled during streaming. When the reap loop
     * gets this URB instead of an audio URB, it updates calibratedFpmf
     * and resubmits — tracking the DAC's actual clock in real-time.
     * Effective polling interval is ~1 ms (the host controller schedules
     * the feedback endpoint once per microframe).
     */
    struct usbdevfs_urb *feedbackUrb;
    uint8_t feedbackBuffer[4];
    bool feedbackInFlight;
};

// ── Functions accessible from native-audio-engine ──────────────────

/**
 * Submit PCM data (already in the target bit depth) to the USB pipeline.
 * Blocks when the URB ring is full (natural backpressure from DAC clock).
 */
void submitPcmToUrbs(UsbAudioContext *ctx, const uint8_t *pcmData, int totalBytes);

/** 16-bit → 32-bit: shift left 16. */
void padInt16ToInt32(const uint8_t *src, uint8_t *dst, int numSamples);

/** 24-bit packed (3 bytes/sample) → 32-bit: sign-extend + shift left 8. */
void padInt24ToInt32(const uint8_t *src, uint8_t *dst, int numSamples);

/** int32 (24-bit sign-extended from libFLAC) → 32-bit: shift left 8. */
void shiftInt32From24(const uint8_t *src, uint8_t *dst, int numSamples);

// ── Bit-depth reduction (TPDF-dithered, for 16-bit-only devices) ────

/** 24-bit packed (3 bytes/sample) → 16-bit with TPDF dither at the target LSB. */
void ditherInt24ToInt16(const uint8_t *src, uint8_t *dst, int numSamples, uint32_t *ditherState);

/** int32 → 16-bit with TPDF dither at the target LSB. */
void ditherInt32ToInt16(const uint8_t *src, uint8_t *dst, int numSamples, uint32_t *ditherState);
