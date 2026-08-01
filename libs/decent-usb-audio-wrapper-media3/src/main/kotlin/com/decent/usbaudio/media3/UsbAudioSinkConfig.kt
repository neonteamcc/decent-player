package com.decent.usbaudio.media3

/**
 * Configuration for [UsbAudioSink].
 *
 * @param bitPerfectEnabled When true, audio is sent directly to the USB DAC
 *        bypassing the Android audio stack entirely.
 * @param forceRouteToSpeaker When true, the delegate AudioTrack is routed to
 *        the built-in speaker to prevent AudioFlinger from opening the USB device.
 * @param nativeFlacEngineEnabled When true (default), local FLAC files are
 *        decoded by [com.decent.usbaudio.NativeAudioEngine] — a single C++
 *        thread with zero JNI in the hot path, worth ~10x headroom on weak
 *        CPUs. When false, local files take the same route as network
 *        streams: ExoPlayer decodes and the PCM is paced into USB by the
 *        streaming thread. Output stays bit-perfect either way — the bits
 *        are preserved by intercepting PCM before AudioTrack, not by the
 *        decoder choice.
 *
 *        Turn it OFF when the host app needs ExoPlayer's buffered position
 *        to stay meaningful for local files (a visible buffer bar, or any
 *        logic keyed off it): the engine path pairs with
 *        [NativeEngineAwareLoadControl], which suppresses ExoPlayer's
 *        loading entirely, so its buffer freezes at whatever was read
 *        before the engine took over. On a device with CPU to spare that
 *        headroom buys nothing, while the frozen buffer costs correctness.
 */
data class UsbAudioSinkConfig(
    val bitPerfectEnabled: Boolean = true,
    val forceRouteToSpeaker: Boolean = true,
    val nativeFlacEngineEnabled: Boolean = true
)
