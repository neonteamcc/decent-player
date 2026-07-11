package com.decent.usbaudio.descriptor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
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
