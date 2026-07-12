package com.decent.usbaudio.descriptor

/**
 * Parses a raw USB descriptor blob into a [UsbAudioDeviceLayout].
 *
 * Input is the byte array returned by
 * `UsbDeviceConnection.getRawDescriptors()`: the 18-byte device descriptor
 * followed by the active configuration descriptor and everything nested in
 * it. The walk is a flat TLV scan — every descriptor starts with
 * `[bLength][bDescriptorType]`.
 *
 * Supports both USB Audio Class 1.0 (audio10) and 2.0 (Audio20) layouts,
 * which differ at the byte level:
 *
 * | | UAC1 | UAC2 |
 * |---|---|---|
 * | AC bInterfaceProtocol | 0x00 | 0x20 |
 * | AC HEADER bcdADC | 0x0100 | 0x0200 |
 * | Rate control | endpoint SET_CUR | clock source entity |
 * | Format Type I | ch@4, subframe@5, bits@6, rate list@8.. | subslot@4, bits@5, no rates |
 * | Channels | Format Type I | AS_GENERAL @10 |
 * | Data EP descriptor | 9 bytes (bRefresh, bSynchAddress) | 7 bytes |
 * | Feedback pairing | data EP bSynchAddress | EP usage bits 5:4 = 01 |
 *
 * Real-world quirk handled deliberately: CSR/Qualcomm UAC1 devices (FiiO
 * BTR3/BTR3K/BTR13) emit the class-specific EP_GENERAL descriptor BEFORE the
 * standard endpoint descriptor, while most devices emit it after. CS-endpoint
 * attributes are therefore associated with the current alt setting, not with
 * the nearest preceding endpoint.
 *
 * Pure Kotlin, no Android imports — unit-tested on the JVM against captured
 * descriptor dumps (see `docs/hardware/fixtures/`).
 *
 * @author DecentPlayer project
 */
object UsbAudioDescriptorParser {

    // Standard descriptor types
    private const val DT_INTERFACE = 0x04
    private const val DT_ENDPOINT = 0x05
    private const val DT_CS_INTERFACE = 0x24
    private const val DT_CS_ENDPOINT = 0x25

    // Audio interface class/subclass
    private const val CLASS_AUDIO = 0x01
    private const val SUBCLASS_AUDIOCONTROL = 0x01
    private const val SUBCLASS_AUDIOSTREAMING = 0x02

    // bInterfaceProtocol
    private const val IP_VERSION_02_00 = 0x20
    private const val IP_VERSION_03_00 = 0x30

    // AudioControl CS_INTERFACE subtypes
    private const val AC_HEADER = 0x01
    private const val AC_OUTPUT_TERMINAL = 0x03
    private const val AC_FEATURE_UNIT = 0x06
    private const val AC_CLOCK_SOURCE_UAC2 = 0x0A

    // AudioStreaming CS_INTERFACE subtypes
    private const val AS_GENERAL = 0x01
    private const val AS_FORMAT_TYPE = 0x02

    // CS_ENDPOINT subtype
    private const val EP_GENERAL = 0x01

    // Endpoint bmAttributes
    private const val XFER_ISOCHRONOUS = 0x01
    private const val USAGE_FEEDBACK = 0x01

    /**
     * Parse [raw] into a layout, or null when no audio function is present.
     * The first audio function wins; UAC3 functions are reported as
     * [UacVersion.UAC3] with no streaming alts (unsupported).
     */
    fun parse(raw: ByteArray): UsbAudioDeviceLayout? {
        var uacVersion = UacVersion.UNKNOWN
        var controlInterfaceId = -1
        var clockSourceId = -1
        val alts = mutableListOf<StreamingAltSetting>()

        // Volume discovery: Feature Units by bUnitID, plus the bSourceID of
        // each speaker/headphone Output Terminal — the FU directly feeding
        // an output OT is the playback volume (one hop covers every device
        // in the fixture set; fallback: any FU with volume).
        val featureUnits = mutableMapOf<Int, VolumeControl>()
        val outputSources = mutableListOf<Int>()

        // Current interface context
        var inAudioControl = false
        var inAudioStreaming = false
        var currentInterfaceId = -1
        var currentAlt = 0

        // Builder state for the alt setting being walked
        var builder: AltBuilder? = null

        fun finishAlt() {
            builder?.build()?.let { alts += it }
            builder = null
        }

        var i = 0
        while (i + 1 < raw.size) {
            val len = raw[i].toInt() and 0xFF
            if (len < 2 || i + len > raw.size) break
            val type = raw[i + 1].toInt() and 0xFF

            when (type) {
                DT_INTERFACE -> if (len >= 9) {
                    finishAlt()
                    currentInterfaceId = raw[i + 2].toInt() and 0xFF
                    currentAlt = raw[i + 3].toInt() and 0xFF
                    val cls = raw[i + 5].toInt() and 0xFF
                    val sub = raw[i + 6].toInt() and 0xFF
                    val proto = raw[i + 7].toInt() and 0xFF

                    inAudioControl = cls == CLASS_AUDIO && sub == SUBCLASS_AUDIOCONTROL
                    inAudioStreaming = cls == CLASS_AUDIO && sub == SUBCLASS_AUDIOSTREAMING

                    if (inAudioControl && controlInterfaceId < 0) {
                        controlInterfaceId = currentInterfaceId
                        uacVersion = when (proto) {
                            IP_VERSION_02_00 -> UacVersion.UAC2
                            IP_VERSION_03_00 -> UacVersion.UAC3
                            else -> UacVersion.UAC1 // refined by bcdADC below
                        }
                    }
                    if (inAudioStreaming && uacVersion != UacVersion.UAC3) {
                        builder = AltBuilder(currentInterfaceId, currentAlt, uacVersion)
                    }
                }

                DT_CS_INTERFACE -> {
                    val subtype = if (len >= 3) raw[i + 2].toInt() and 0xFF else -1
                    if (inAudioControl) {
                        // AC HEADER: bcdADC at offset 3..4 in both versions.
                        if (subtype == AC_HEADER && len >= 5 &&
                                controlInterfaceId == currentInterfaceId) {
                            val bcdAdc = le16(raw, i + 3)
                            uacVersion = when {
                                uacVersion == UacVersion.UAC3 -> UacVersion.UAC3
                                bcdAdc >= 0x0200 -> UacVersion.UAC2
                                bcdAdc >= 0x0100 -> UacVersion.UAC1
                                else -> uacVersion
                            }
                        }
                        if (uacVersion == UacVersion.UAC2 &&
                                subtype == AC_CLOCK_SOURCE_UAC2 && len >= 5 &&
                                clockSourceId < 0) {
                            clockSourceId = raw[i + 3].toInt() and 0xFF
                        }
                        if (subtype == AC_OUTPUT_TERMINAL && len >= 9) {
                            // Speaker/headphone-class terminals (0x03xx);
                            // bSourceID sits at offset 7 in both versions.
                            val terminalType = le16(raw, i + 4)
                            if ((terminalType shr 8) == 0x03) {
                                outputSources += raw[i + 7].toInt() and 0xFF
                            }
                        }
                        if (subtype == AC_FEATURE_UNIT) {
                            parseFeatureUnit(raw, i, len, uacVersion)
                                    ?.let { featureUnits[it.unitId] = it }
                        }
                    }
                    if (inAudioStreaming) {
                        builder?.onCsInterface(raw, i, len, subtype)
                    }
                }

                DT_ENDPOINT -> if (inAudioStreaming && len >= 7) {
                    builder?.onEndpoint(raw, i, len)
                }

                DT_CS_ENDPOINT -> if (inAudioStreaming && len >= 4) {
                    val subtype = raw[i + 2].toInt() and 0xFF
                    if (subtype == EP_GENERAL) builder?.onCsEndpoint(raw, i, len)
                }
            }

            i += len
        }
        finishAlt()

        if (uacVersion == UacVersion.UNKNOWN && controlInterfaceId < 0) return null

        val volume = outputSources.firstNotNullOfOrNull { featureUnits[it] }
                ?: featureUnits.values.firstOrNull { it.hasVolume }

        return UsbAudioDeviceLayout(
                uacVersion = uacVersion,
                controlInterfaceId = controlInterfaceId,
                clockSourceId = clockSourceId,
                streamingAlts = alts,
                volume = volume?.takeIf { it.hasVolume },
        )
    }

    /**
     * Parse a FEATURE_UNIT's bmaControls into a [VolumeControl].
     *
     * UAC1 (audio10 Table 4-7): `bUnitID@3, bSourceID@4, bControlSize@5,
     * bmaControls[(ch+1) × size]@6, iFeature` — Mute = bit 0, Volume =
     * bit 1 of each entry's first byte.
     * UAC2 (Audio20 Table 4-13): `bUnitID@3, bSourceID@4,
     * bmaControls[(ch+1) × 4]@5, iFeature` — 2 bits per control:
     * Mute = bits 1:0, Volume = bits 3:2; host-writable when 0b11.
     */
    private fun parseFeatureUnit(
            raw: ByteArray,
            i: Int,
            len: Int,
            uacVersion: UacVersion,
    ): VolumeControl? {
        val unitId = if (len >= 5) raw[i + 3].toInt() and 0xFF else return null

        var masterVolume = false
        var masterMute = false
        val volumeChannels = mutableListOf<Int>()

        if (uacVersion == UacVersion.UAC2) {
            if (len < 10) return null
            val entries = (len - 6) / 4
            for (e in 0 until entries) {
                val controls = le16(raw, i + 5 + e * 4) // bits of interest are in the low 16
                val mute = (controls and 0x03) == 0x03
                val volume = (controls and 0x0C) == 0x0C
                if (e == 0) {
                    masterMute = mute
                    masterVolume = volume
                } else if (volume) {
                    volumeChannels += e
                }
            }
        } else {
            if (len < 8) return null
            val controlSize = raw[i + 5].toInt() and 0xFF
            if (controlSize < 1) return null
            val entries = (len - 7) / controlSize
            for (e in 0 until entries) {
                val bits = raw[i + 6 + e * controlSize].toInt() and 0xFF
                val mute = (bits and 0x01) != 0
                val volume = (bits and 0x02) != 0
                if (e == 0) {
                    masterMute = mute
                    masterVolume = volume
                } else if (volume) {
                    volumeChannels += e
                }
            }
        }
        return VolumeControl(
                unitId = unitId,
                masterVolume = masterVolume,
                volumeChannels = volumeChannels,
                masterMute = masterMute,
        )
    }

    /** Accumulates one AS alt setting's descriptors in any on-wire order. */
    private class AltBuilder(
            val interfaceId: Int,
            val altSetting: Int,
            val uacVersion: UacVersion,
    ) {
        var channels = 0
        var subslotSize = 0
        var bitResolution = 0
        var rates: List<Int> = emptyList()
        var continuous: IntRange? = null

        var epAddress = -1
        var epMaxPacket = 0
        var epMult = 0
        var epInterval = 0
        var syncType = UsbSyncType.NONE
        var synchAddress = 0

        var fbAddress = -1
        var fbMaxPacket = 0
        var fbRefresh = 0
        var fbInterval = 0

        var sampleRateControl = false
        var maxPacketsOnly = false

        fun onCsInterface(raw: ByteArray, i: Int, len: Int, subtype: Int) {
            when (subtype) {
                AS_GENERAL -> if (uacVersion == UacVersion.UAC2 && len >= 16) {
                    channels = raw[i + 10].toInt() and 0xFF
                }
                AS_FORMAT_TYPE -> when (uacVersion) {
                    UacVersion.UAC2 -> if (len >= 6) {
                        subslotSize = raw[i + 4].toInt() and 0xFF
                        bitResolution = raw[i + 5].toInt() and 0xFF
                    }
                    else -> if (len >= 8) { // UAC1 (and UNKNOWN treated as UAC1)
                        channels = raw[i + 4].toInt() and 0xFF
                        subslotSize = raw[i + 5].toInt() and 0xFF
                        bitResolution = raw[i + 6].toInt() and 0xFF
                        val freqType = raw[i + 7].toInt() and 0xFF
                        if (freqType == 0) {
                            if (len >= 14) {
                                continuous = le24(raw, i + 8)..le24(raw, i + 11)
                            }
                        } else {
                            val list = mutableListOf<Int>()
                            for (r in 0 until freqType) {
                                val off = i + 8 + r * 3
                                if (off + 2 < i + len) list += le24(raw, off)
                            }
                            rates = list
                        }
                    }
                }
            }
        }

        fun onEndpoint(raw: ByteArray, i: Int, len: Int) {
            val address = raw[i + 2].toInt() and 0xFF
            val attrs = raw[i + 3].toInt() and 0xFF
            if ((attrs and 0x03) != XFER_ISOCHRONOUS) return // interrupt EPs etc.

            val isIn = (address and 0x80) != 0
            val maxPacketRaw = le16(raw, i + 4)
            val interval = raw[i + 6].toInt() and 0xFF
            val usage = (attrs shr 4) and 0x03
            val refresh = if (len >= 9) raw[i + 7].toInt() and 0xFF else 0
            val synchAddr = if (len >= 9) raw[i + 8].toInt() and 0xFF else 0

            val isFeedback = isIn && (usage == USAGE_FEEDBACK ||
                    // UAC1 marks feedback only via the data EP's bSynchAddress;
                    // a tiny iso IN endpoint in a playback alt is the synch EP.
                    (uacVersion != UacVersion.UAC2 && (maxPacketRaw and 0x7FF) <= 4))

            if (!isIn) {
                epAddress = address
                epMaxPacket = maxPacketRaw and 0x7FF
                epMult = (maxPacketRaw shr 11) and 0x03
                epInterval = interval
                syncType = UsbSyncType.fromBits(attrs shr 2)
                synchAddress = synchAddr
            } else if (isFeedback) {
                fbAddress = address
                fbMaxPacket = maxPacketRaw and 0x7FF
                fbRefresh = refresh
                fbInterval = interval
            }
            // Iso IN data endpoints (capture alts) leave epAddress at -1;
            // build() drops the alt.
        }

        fun onCsEndpoint(raw: ByteArray, i: Int, len: Int) {
            val attrs = raw[i + 3].toInt() and 0xFF
            // UAC1: bit0 = SamplingFrequency control. UAC2 has no such bit
            // (rates live in the clock source), bit7 means the same in both.
            if (uacVersion != UacVersion.UAC2) {
                sampleRateControl = (attrs and 0x01) != 0
            }
            maxPacketsOnly = (attrs and 0x80) != 0
        }

        fun build(): StreamingAltSetting? {
            if (epAddress < 0) return null // zero-bandwidth or capture alt
            val feedback = if (fbAddress > 0) FeedbackEndpoint(
                    address = fbAddress,
                    maxPacketSize = fbMaxPacket,
                    refresh = fbRefresh,
                    interval = fbInterval,
            ) else null
            return StreamingAltSetting(
                    interfaceId = interfaceId,
                    altSetting = altSetting,
                    channels = channels,
                    subslotSize = subslotSize,
                    bitResolution = bitResolution,
                    sampleRates = rates,
                    continuousRateRange = continuous,
                    endpointAddress = epAddress,
                    maxPacketSize = epMaxPacket,
                    packetMult = epMult,
                    interval = epInterval,
                    syncType = syncType,
                    synchAddress = synchAddress,
                    feedback = feedback,
                    hasSampleRateControl = sampleRateControl,
                    maxPacketsOnly = maxPacketsOnly,
            )
        }
    }

    /**
     * Descriptor-invariant bus speed hint: true when any endpoint in [raw]
     * declares a value that is illegal at full speed, proving the device is
     * operating at high speed or above. False means "full speed or unknown"
     * — a negative result proves nothing (use USBDEVFS_GET_SPEED first).
     *
     * Invariants used (USB 2.0 §5.6.3, §5.8.3): iso payload > 1023 bytes,
     * iso additional-transaction mult bits set, or a 512-byte bulk endpoint.
     */
    fun definitelyHighSpeed(raw: ByteArray): Boolean {
        var i = 0
        while (i + 1 < raw.size) {
            val len = raw[i].toInt() and 0xFF
            if (len < 2 || i + len > raw.size) break
            if ((raw[i + 1].toInt() and 0xFF) == DT_ENDPOINT && len >= 7) {
                val attrs = raw[i + 3].toInt() and 0xFF
                val maxPacketRaw = le16(raw, i + 4)
                when (attrs and 0x03) {
                    XFER_ISOCHRONOUS ->
                        if ((maxPacketRaw and 0x7FF) > 1023 ||
                                ((maxPacketRaw shr 11) and 0x03) > 0) return true
                    0x02 -> // bulk
                        if (maxPacketRaw == 512) return true
                }
            }
            i += len
        }
        return false
    }

    private fun le16(raw: ByteArray, off: Int): Int =
            (raw[off].toInt() and 0xFF) or ((raw[off + 1].toInt() and 0xFF) shl 8)

    private fun le24(raw: ByteArray, off: Int): Int =
            (raw[off].toInt() and 0xFF) or
                    ((raw[off + 1].toInt() and 0xFF) shl 8) or
                    ((raw[off + 2].toInt() and 0xFF) shl 16)
}
