package com.decent.usbaudio.descriptor

/**
 * Parsed, class-version-agnostic model of a USB audio function's descriptors.
 *
 * Produced by [UsbAudioDescriptorParser] from the raw descriptor blob
 * (`UsbDeviceConnection.getRawDescriptors()`), which contains the device
 * descriptor followed by the active configuration.
 *
 * This model is deliberately pure Kotlin (no Android imports) so the parser
 * is unit-testable on the JVM against captured descriptor dumps.
 *
 * @author DecentPlayer project
 */

/** USB Audio Class version of the audio function. */
enum class UacVersion {
    /** audio10: bInterfaceProtocol 0x00, AC HEADER bcdADC 0x0100. */
    UAC1,

    /** Audio20: bInterfaceProtocol 0x20 (IP_VERSION_02_00), bcdADC 0x0200. */
    UAC2,

    /** Audio30: bInterfaceProtocol 0x30. Recognized but unsupported. */
    UAC3,

    /** No audio function found, or an unrecognized layout. */
    UNKNOWN,
}

/**
 * Isochronous synchronization type from the data endpoint's bmAttributes
 * bits 3:2 (USB 2.0 Table 9-13). Identical encoding in UAC1 and UAC2.
 */
enum class UsbSyncType(val bits: Int) {
    /** 00 — no synchronization declared. Treat like [ADAPTIVE] on the host. */
    NONE(0),

    /** 01 — asynchronous: device runs its own clock, reports via feedback. */
    ASYNC(1),

    /** 10 — adaptive: device recovers its clock from the data rate. */
    ADAPTIVE(2),

    /** 11 — synchronous: device locks to USB SOF. Host sends nominal rate. */
    SYNC(3);

    companion object {
        fun fromBits(bits: Int): UsbSyncType =
                entries.firstOrNull { it.bits == (bits and 0x3) } ?: NONE
    }

    /** True when the host must run a feedback loop to pace packets. */
    val needsFeedback: Boolean
        get() = this == ASYNC
}

/**
 * An explicit feedback (synch) endpoint paired with a playback data endpoint.
 *
 * UAC1 pairs it via the data endpoint's bSynchAddress (9-byte descriptor);
 * UAC2 marks it with usage bits 5:4 = 01 (feedback) in bmAttributes.
 */
data class FeedbackEndpoint(
        /** Endpoint address including the IN direction bit (e.g. 0x81). */
        val address: Int,

        /** wMaxPacketSize: 3 = full-speed Q10.14, 4 = Q16.16 payload. */
        val maxPacketSize: Int,

        /**
         * UAC1 bRefresh exponent from the 9-byte endpoint descriptor:
         * feedback refresh period = 2^bRefresh ms, valid range 1..9.
         * 0 when absent/invalid (UAC2 descriptors have no bRefresh).
         */
        val refresh: Int,

        /** bInterval as declared (UAC2/HS pacing: period 2^(bInterval-1)). */
        val interval: Int,
)

/**
 * One streaming (playback) alternate setting: format + data endpoint.
 * Alt settings without an isochronous OUT data endpoint (zero-bandwidth
 * alt 0, capture-only alts) are not represented.
 */
data class StreamingAltSetting(
        /** bInterfaceNumber of the AudioStreaming interface. */
        val interfaceId: Int,

        /** bAlternateSetting. */
        val altSetting: Int,

        /** Channel count (UAC1: Format Type I; UAC2: AS_GENERAL). */
        val channels: Int,

        /** Bytes per sample slot (UAC1 bSubframeSize / UAC2 bSubslotSize). */
        val subslotSize: Int,

        /** Meaningful bits per sample (bBitResolution). */
        val bitResolution: Int,

        /**
         * Discrete sample rates. UAC1: from the Format Type I descriptor's
         * tSamFreq list. UAC2: empty — rates must be queried from the clock
         * source entity via a RANGE request.
         */
        val sampleRates: List<Int>,

        /** UAC1 continuous-range variant (bSamFreqType == 0), else null. */
        val continuousRateRange: IntRange?,

        /** Data endpoint address (OUT, e.g. 0x03). */
        val endpointAddress: Int,

        /** Data endpoint wMaxPacketSize, payload bits 10:0 only. */
        val maxPacketSize: Int,

        /** Additional-transactions mult bits 12:11 of wMaxPacketSize. */
        val packetMult: Int,

        /** Data endpoint bInterval as declared. */
        val interval: Int,

        /** Synchronization type from the data endpoint bmAttributes. */
        val syncType: UsbSyncType,

        /** bSynchAddress from a 9-byte UAC1 data endpoint descriptor, or 0. */
        val synchAddress: Int,

        /** Explicit feedback endpoint in this alt setting, if any. */
        val feedback: FeedbackEndpoint?,

        /**
         * UAC1 CS endpoint bmAttributes bit0: the endpoint accepts
         * SET_CUR(SAMPLING_FREQ_CONTROL). UAC2 sets rates via the clock
         * source instead, so this is false there.
         */
        val hasSampleRateControl: Boolean,

        /**
         * CS endpoint bmAttributes bit7 (MaxPacketsOnly / UsesMaxPacketSize):
         * the device wants only wMaxPacketSize-sized packets (ALSA fill_max).
         */
        val maxPacketsOnly: Boolean,
) {
    /** Bytes per audio frame (one sample slot per channel). */
    val bytesPerFrame: Int
        get() = subslotSize * channels

    /** True when [rate] is advertised by this alt setting (UAC2: unknown → true). */
    fun supportsRate(rate: Int): Boolean = when {
        sampleRates.isNotEmpty() -> rate in sampleRates
        continuousRateRange != null -> rate in continuousRateRange
        else -> true // UAC2: rates live in the clock source, not the format
    }

    /**
     * True when [rate] fits this alt's bandwidth at full-speed 1 ms frames:
     * the worst-case packet (nominal + 1 frame for the fractional carry)
     * must not exceed wMaxPacketSize.
     */
    fun rateFitsMaxPacket(rate: Int): Boolean {
        val worstFrames = rate / 1000 + if (rate % 1000 != 0) 1 else 0
        return worstFrames * bytesPerFrame <= maxPacketSize
    }
}

/**
 * Hardware volume capability from a Feature Unit in the playback path.
 *
 * Addressing quirk this models: some devices put Volume on the master
 * channel (FiiO BTR13: bmaControls(0) = Mute+Volume), others only on the
 * individual channels (Apple dongles, FiiO BTR3K: master carries Mute,
 * channels 1..N carry Volume) — the host must then write each channel.
 */
data class VolumeControl(
        /** Feature Unit bUnitID (wIndex high byte of the requests). */
        val unitId: Int,

        /** Volume controllable on the master channel (CN = 0). */
        val masterVolume: Boolean,

        /** Channels (1-based CN) with volume when the master lacks it. */
        val volumeChannels: List<Int>,

        /** Mute controllable on the master channel. */
        val masterMute: Boolean,
) {
    val hasVolume: Boolean
        get() = masterVolume || volumeChannels.isNotEmpty()

    /** Channel numbers to address for a volume write. */
    val writeChannels: List<Int>
        get() = if (masterVolume) listOf(0) else volumeChannels
}

/** Complete parsed layout of the device's (first) audio function. */
data class UsbAudioDeviceLayout(
        val uacVersion: UacVersion,

        /** bInterfaceNumber of the AudioControl interface, or -1. */
        val controlInterfaceId: Int,

        /** UAC2 clock source entity bClockID, or -1 (always -1 for UAC1). */
        val clockSourceId: Int,

        /** Playback alt settings across all AudioStreaming interfaces. */
        val streamingAlts: List<StreamingAltSetting>,

        /** Playback-path Feature Unit volume capability, or null. */
        val volume: VolumeControl? = null,
) {
    /** True when at least one playback path exists. */
    val hasPlayback: Boolean
        get() = streamingAlts.isNotEmpty()
}
