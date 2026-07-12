package com.decent.usbaudio.descriptor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Wire-encoding tests for [UacControl] against the spec examples. */
class UacControlTest {

    @Test
    fun uac1SetSampleRate_encodesEndpointRequest() {
        // BTR13: data endpoint 0x03, 44100 Hz = 0x00AC44 → 44 AC 00 LE.
        val req = UacControl.uac1SetSampleRate(0x03, 44100)
        assertEquals(0x22, req.requestType)
        assertEquals(0x01, req.request)          // SET_CUR
        assertEquals(0x0100, req.value)          // SAMPLING_FREQ_CONTROL << 8
        assertEquals(0x0003, req.index)          // endpoint address, low byte
        assertArrayEquals(byteArrayOf(0x44, 0xAC.toByte(), 0x00), req.data)
    }

    @Test
    fun uac1SetSampleRate_96k() {
        // 96000 = 0x017700 → 00 77 01 LE.
        val req = UacControl.uac1SetSampleRate(0x03, 96000)
        assertArrayEquals(byteArrayOf(0x00, 0x77, 0x01), req.data)
    }

    @Test
    fun uac1GetSampleRate_encodesReadback() {
        val req = UacControl.uac1GetSampleRate(0x03)
        assertEquals(0xA2, req.requestType)
        assertEquals(0x81, req.request)          // GET_CUR
        assertEquals(0x0100, req.value)
        assertEquals(0x0003, req.index)
        assertEquals(3, req.data.size)
    }

    @Test
    fun uac2SetSampleRate_encodesClockEntityRequest() {
        // Apple adapter: clock source 9, AC interface 0, 48000 = 0x00BB80.
        val req = UacControl.uac2SetSampleRate(9, 0, 48000)
        assertEquals(0x21, req.requestType)
        assertEquals(0x01, req.request)
        assertEquals(0x0100, req.value)
        assertEquals(0x0900, req.index)          // (clockId << 8) | acInterface
        assertArrayEquals(byteArrayOf(0x80.toByte(), 0xBB.toByte(), 0x00, 0x00), req.data)
    }

    @Test
    fun uac2GetClockValid_usesSelector2() {
        val req = UacControl.uac2GetClockValid(9, 0)
        assertEquals(0xA1, req.requestType)
        assertEquals(0x0200, req.value)
        assertEquals(0x0900, req.index)
        assertEquals(1, req.data.size)
    }

    @Test
    fun decodeRate_handles3And4Bytes() {
        assertEquals(44100, UacControl.decodeRate(byteArrayOf(0x44, 0xAC.toByte(), 0x00), 3))
        assertEquals(96000, UacControl.decodeRate(byteArrayOf(0x00, 0x77, 0x01), 3))
        assertEquals(192000, UacControl.decodeRate(byteArrayOf(0x00, 0xEE.toByte(), 0x02, 0x00), 4))
        assertEquals(-1, UacControl.decodeRate(byteArrayOf(0x44), 1))
    }

    @Test
    fun uac2SampleRateRange_encodesRangeRequest() {
        val req = UacControl.uac2GetSampleRateRange(9, 0)
        assertEquals(0xA1, req.requestType)
        assertEquals(0x02, req.request)          // RANGE
        assertEquals(0x0100, req.value)
        assertEquals(0x0900, req.index)
        assertEquals(2 + 32 * 12, req.data.size)
    }

    @Test
    fun parseRanges_singleDiscreteRate() {
        // EarPods-style: one subrange, MIN==MAX==48000, RES 0.
        val data = rangeReply(Triple(48000, 48000, 0))
        assertEquals(listOf(48000), UacControl.parseUac2SampleRateRanges(data, data.size))
    }

    @Test
    fun parseRanges_discreteList() {
        // XMOS-style: each rate its own MIN==MAX triplet.
        val data = rangeReply(
            Triple(44100, 44100, 0), Triple(48000, 48000, 0),
            Triple(88200, 88200, 0), Triple(96000, 96000, 0),
        )
        assertEquals(listOf(44100, 48000, 88200, 96000),
            UacControl.parseUac2SampleRateRanges(data, data.size))
    }

    @Test
    fun parseRanges_continuousAndDegenerate() {
        // Small continuous range enumerates; huge one contributes endpoints.
        val small = rangeReply(Triple(44100, 48000, 3900))
        assertEquals(listOf(44100, 48000), UacControl.parseUac2SampleRateRanges(small, small.size))
        val huge = rangeReply(Triple(8000, 192000, 1))
        assertEquals(listOf(8000, 192000), UacControl.parseUac2SampleRateRanges(huge, huge.size))
        // Truncated reply and garbage lengths return what fits / nothing.
        assertEquals(emptyList<Int>(), UacControl.parseUac2SampleRateRanges(ByteArray(1), 1))
        val truncated = rangeReply(Triple(44100, 44100, 0), Triple(48000, 48000, 0))
        assertEquals(listOf(44100),
            UacControl.parseUac2SampleRateRanges(truncated, 2 + 12)) // only 1st triplet fits
    }

    private fun rangeReply(vararg ranges: Triple<Int, Int, Int>): ByteArray {
        val out = ByteArray(2 + ranges.size * 12)
        out[0] = (ranges.size and 0xFF).toByte()
        out[1] = ((ranges.size shr 8) and 0xFF).toByte()
        ranges.forEachIndexed { i, (min, max, res) ->
            writeLe32(out, 2 + i * 12, min)
            writeLe32(out, 2 + i * 12 + 4, max)
            writeLe32(out, 2 + i * 12 + 8, res)
        }
        return out
    }

    private fun writeLe32(buf: ByteArray, off: Int, v: Int) {
        buf[off] = (v and 0xFF).toByte()
        buf[off + 1] = ((v shr 8) and 0xFF).toByte()
        buf[off + 2] = ((v shr 16) and 0xFF).toByte()
        buf[off + 3] = ((v shr 24) and 0xFF).toByte()
    }

    @Test
    fun volumeRequests_encodePerSpec() {
        // SET_CUR(VOLUME) master channel, -12.00 dB = -3072 = 0xF400.
        val set = UacControl.setVolume(unitId = 2, acInterface = 0, channel = 0, valueDb256 = -3072)
        assertEquals(0x21, set.requestType)
        assertEquals(0x01, set.request)
        assertEquals(0x0200, set.value)          // VOLUME_CONTROL << 8 | CN 0
        assertEquals(0x0200, set.index)          // unit 2 << 8 | ac 0
        assertArrayEquals(byteArrayOf(0x00, 0xF4.toByte()), set.data)

        val ch2 = UacControl.setVolume(2, 0, 2, 0)
        assertEquals(0x0202, ch2.value)          // CN in the low byte

        val getMin = UacControl.uac1GetVolume(2, 0, 0, UacControl.UAC1_GET_MIN)
        assertEquals(0xA1, getMin.requestType)
        assertEquals(0x82, getMin.request)

        val mute = UacControl.setMute(2, 0, muted = true)
        assertEquals(0x0100, mute.value)         // MUTE_CONTROL << 8
        assertArrayEquals(byteArrayOf(0x01), mute.data)

        val range = UacControl.uac2GetVolumeRange(2, 0, 1)
        assertEquals(0x02, range.request)        // RANGE
        assertEquals(0x0201, range.value)

        // UAC2 reads CUR with request code 0x01 + direction bit — NOT the
        // UAC1-style 0x81 attribute request.
        val getCur2 = UacControl.uac2GetVolume(10, 0, 0)
        assertEquals(0xA1, getCur2.requestType)
        assertEquals(0x01, getCur2.request)
        assertEquals(0x0200, getCur2.value)
        assertEquals(0x0A00, getCur2.index)      // unit 10 << 8 | ac 0
        assertEquals(2, getCur2.data.size)
    }

    @Test
    fun volumeRange_parsesAndDecodesSigned() {
        // wNumSubRanges=1, MIN=-60.00 dB (-15360=0xC400), MAX=0, RES=1.00 dB (256)
        val data = byteArrayOf(1, 0, 0x00, 0xC4.toByte(), 0x00, 0x00, 0x00, 0x01)
        val r = UacControl.parseUac2VolumeRange(data, data.size)!!
        assertEquals(-15360, r.minDb256)
        assertEquals(0, r.maxDb256)
        assertEquals(256, r.resDb256)
        assertEquals(-3072, UacControl.decodeS16(byteArrayOf(0x00, 0xF4.toByte()), 0))
        assertNull(UacControl.parseUac2VolumeRange(ByteArray(4), 4))
    }

    @Test
    fun busSpeed_mapsIoctlValues() {
        assertEquals(UsbBusSpeed.FULL, UsbBusSpeed.fromIoctl(2))
        assertEquals(UsbBusSpeed.HIGH, UsbBusSpeed.fromIoctl(3))
        assertEquals(UsbBusSpeed.SUPER, UsbBusSpeed.fromIoctl(5))
        assertEquals(UsbBusSpeed.UNKNOWN, UsbBusSpeed.fromIoctl(-19)) // -ENODEV etc.
        assertEquals(1000, UsbBusSpeed.FULL.packetsPerSecond)
        assertEquals(8000, UsbBusSpeed.HIGH.packetsPerSecond)
        assertEquals(8000, UsbBusSpeed.SUPER.packetsPerSecond)
    }
}
