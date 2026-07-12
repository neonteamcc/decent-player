package com.decent.usbaudio.descriptor

/**
 * Encodes USB Audio Class control requests as plain values for
 * `UsbDeviceConnection.controlTransfer()`. Pure Kotlin so the exact wire
 * encodings are unit-testable on the JVM.
 *
 * UAC1 (audio10 §5.2.3.2) sets the sample rate on the ISO data ENDPOINT:
 * the request is class-type, endpoint-recipient, with the endpoint address
 * in wIndex and a 3-byte little-endian rate. It is only valid while a
 * non-zero alternate setting is selected (the endpoint must exist), and
 * must be re-sent after every alt-setting change.
 *
 * UAC2 (Audio20 §5.2.5.1.1) sets the rate on the CLOCK SOURCE entity via
 * the AudioControl interface: wIndex = (clockId << 8) | acInterface,
 * 4-byte little-endian rate.
 *
 * @author DecentPlayer project
 */
object UacControl {

    /** Arguments for one control transfer, plus the data buffer. */
    data class ControlRequest(
            val requestType: Int,
            val request: Int,
            val value: Int,
            val index: Int,
            val data: ByteArray,
    ) {
        override fun equals(other: Any?): Boolean =
                other is ControlRequest && requestType == other.requestType &&
                        request == other.request && value == other.value &&
                        index == other.index && data.contentEquals(other.data)

        override fun hashCode(): Int = ((requestType * 31 + request) * 31 + value) * 31 + index
    }

    // Request codes (audio10 Table A-9 / Audio20 Table A-14)
    private const val UAC1_SET_CUR = 0x01
    private const val UAC1_GET_CUR = 0x81
    private const val UAC2_CUR = 0x01
    private const val UAC2_RANGE = 0x02

    // Control selectors
    private const val UAC1_SAMPLING_FREQ_CONTROL = 0x01 // audio10 Table A-19
    private const val UAC2_CS_SAM_FREQ_CONTROL = 0x01   // Audio20 Table A-17
    private const val UAC2_CS_CLOCK_VALID_CONTROL = 0x02

    // bmRequestType: Class | recipient, direction bit 7
    private const val RT_H2D_CLASS_ENDPOINT = 0x22
    private const val RT_D2H_CLASS_ENDPOINT = 0xA2
    private const val RT_H2D_CLASS_INTERFACE = 0x21
    private const val RT_D2H_CLASS_INTERFACE = 0xA1

    /** UAC1: SET_CUR(SAMPLING_FREQ) on the data endpoint, 3-byte rate. */
    fun uac1SetSampleRate(endpointAddress: Int, rateHz: Int): ControlRequest =
            ControlRequest(
                    requestType = RT_H2D_CLASS_ENDPOINT,
                    request = UAC1_SET_CUR,
                    value = UAC1_SAMPLING_FREQ_CONTROL shl 8,
                    index = endpointAddress and 0xFF,
                    data = byteArrayOf(
                            (rateHz and 0xFF).toByte(),
                            ((rateHz shr 8) and 0xFF).toByte(),
                            ((rateHz shr 16) and 0xFF).toByte(),
                    ),
            )

    /** UAC1: GET_CUR(SAMPLING_FREQ) from the data endpoint, 3-byte reply. */
    fun uac1GetSampleRate(endpointAddress: Int): ControlRequest =
            ControlRequest(
                    requestType = RT_D2H_CLASS_ENDPOINT,
                    request = UAC1_GET_CUR,
                    value = UAC1_SAMPLING_FREQ_CONTROL shl 8,
                    index = endpointAddress and 0xFF,
                    data = ByteArray(3),
            )

    /** UAC2: SET CUR(SAM_FREQ) on the clock source entity, 4-byte rate. */
    fun uac2SetSampleRate(clockSourceId: Int, acInterface: Int, rateHz: Int): ControlRequest =
            ControlRequest(
                    requestType = RT_H2D_CLASS_INTERFACE,
                    request = UAC2_CUR,
                    value = UAC2_CS_SAM_FREQ_CONTROL shl 8,
                    index = ((clockSourceId and 0xFF) shl 8) or (acInterface and 0xFF),
                    data = byteArrayOf(
                            (rateHz and 0xFF).toByte(),
                            ((rateHz shr 8) and 0xFF).toByte(),
                            ((rateHz shr 16) and 0xFF).toByte(),
                            ((rateHz shr 24) and 0xFF).toByte(),
                    ),
            )

    /** UAC2: GET CUR(SAM_FREQ) from the clock source entity, 4-byte reply. */
    fun uac2GetSampleRate(clockSourceId: Int, acInterface: Int): ControlRequest =
            ControlRequest(
                    requestType = RT_D2H_CLASS_INTERFACE,
                    request = UAC2_CUR,
                    value = UAC2_CS_SAM_FREQ_CONTROL shl 8,
                    index = ((clockSourceId and 0xFF) shl 8) or (acInterface and 0xFF),
                    data = ByteArray(4),
            )

    /**
     * UAC2: RANGE(SAM_FREQ) from the clock source entity — the ONLY place
     * a UAC2 device advertises its sample rates (the Format Type I
     * descriptor carries none). Reply: `wNumSubRanges` (LE16) followed by
     * N × (dMIN, dMAX, dRES) LE32 triplets (Audio20 §5.2.5.1.1).
     */
    fun uac2GetSampleRateRange(
            clockSourceId: Int,
            acInterface: Int,
            maxRanges: Int = 32,
    ): ControlRequest =
            ControlRequest(
                    requestType = RT_D2H_CLASS_INTERFACE,
                    request = UAC2_RANGE,
                    value = UAC2_CS_SAM_FREQ_CONTROL shl 8,
                    index = ((clockSourceId and 0xFF) shl 8) or (acInterface and 0xFF),
                    data = ByteArray(2 + maxRanges * 12),
            )

    /**
     * Parse a RANGE(SAM_FREQ) reply into discrete rates. Continuous
     * subranges (dRES > 0) are enumerated when small; unbounded ones
     * contribute their endpoints only (a rate picker can still test
     * membership against MIN/MAX separately if ever needed — real DACs
     * overwhelmingly use discrete MIN==MAX triplets).
     */
    fun parseUac2SampleRateRanges(data: ByteArray, length: Int): List<Int> {
        if (length < 2) return emptyList()
        val count = (data[0].toInt() and 0xFF) or ((data[1].toInt() and 0xFF) shl 8)
        val rates = sortedSetOf<Int>()
        for (i in 0 until count) {
            val off = 2 + i * 12
            if (off + 12 > length) break
            val min = le32(data, off)
            val max = le32(data, off + 4)
            val res = le32(data, off + 8)
            when {
                min <= 0 -> {}
                max <= min || res <= 0 -> {
                    rates += min
                    if (max > min) rates += max
                }
                (max - min) / res <= 64 -> {
                    var v = min
                    while (v <= max) { rates += v; v += res }
                }
                else -> { rates += min; rates += max }
            }
        }
        return rates.toList()
    }

    private fun le32(data: ByteArray, off: Int): Int =
            (data[off].toInt() and 0xFF) or
                    ((data[off + 1].toInt() and 0xFF) shl 8) or
                    ((data[off + 2].toInt() and 0xFF) shl 16) or
                    ((data[off + 3].toInt() and 0xFF) shl 24)

    /** UAC2: GET CUR(CLOCK_VALID) from the clock source entity, 1 byte. */
    fun uac2GetClockValid(clockSourceId: Int, acInterface: Int): ControlRequest =
            ControlRequest(
                    requestType = RT_D2H_CLASS_INTERFACE,
                    request = UAC2_CUR,
                    value = UAC2_CS_CLOCK_VALID_CONTROL shl 8,
                    index = ((clockSourceId and 0xFF) shl 8) or (acInterface and 0xFF),
                    data = ByteArray(1),
            )

    /** Decode a little-endian rate reply of 3 (UAC1) or 4 (UAC2) bytes. */
    fun decodeRate(data: ByteArray, length: Int): Int {
        if (length < 3) return -1
        var rate = (data[0].toInt() and 0xFF) or
                ((data[1].toInt() and 0xFF) shl 8) or
                ((data[2].toInt() and 0xFF) shl 16)
        if (length >= 4) rate = rate or ((data[3].toInt() and 0xFF) shl 24)
        return rate
    }
}

/**
 * Operating bus speed, from the USBDEVFS_GET_SPEED ioctl
 * (`enum usb_device_speed` in linux/usb/ch9.h).
 */
enum class UsbBusSpeed(val ioctlValue: Int) {
    UNKNOWN(0),
    LOW(1),

    /** 12 Mbps — 1 ms frames, one ISO packet per frame. */
    FULL(2),

    /** 480 Mbps — 125 µs microframes. */
    HIGH(3),
    WIRELESS(4),
    SUPER(5),
    SUPER_PLUS(6);

    companion object {
        fun fromIoctl(value: Int): UsbBusSpeed =
                entries.firstOrNull { it.ioctlValue == value } ?: UNKNOWN
    }

    /** Isochronous service intervals per second at bInterval = 1. */
    val packetsPerSecond: Int
        get() = when (this) {
            LOW, FULL, UNKNOWN -> 1000
            else -> 8000
        }
}
