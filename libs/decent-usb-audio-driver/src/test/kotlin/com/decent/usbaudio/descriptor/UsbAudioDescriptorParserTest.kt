package com.decent.usbaudio.descriptor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * JVM unit tests for [UsbAudioDescriptorParser] against byte-exact
 * reconstructions of captured descriptor dumps (sources in
 * `docs/hardware/fixtures/`). Each blob mirrors what Android's
 * `UsbDeviceConnection.getRawDescriptors()` returns: device descriptor
 * followed by the active configuration, in on-wire order.
 */
class UsbAudioDescriptorParserTest {

    // ── FiiO BTR13 (0a12:4007) — Thesycon dump, full-speed UAC1 ────────
    // Quirk preserved: CS_ENDPOINT (EP_GENERAL) precedes the standard
    // endpoint descriptor (CSR/Qualcomm family wire order).
    private val btr13 = hex(
            "12 01 00 02 00 00 00 40 12 0A 07 40 70 19 01 02 03 01", // device
            "09 02 A9 00 04 01 00 C0 00",                            // config, 169 bytes
            "09 04 00 00 00 01 01 00 00",                            // if0 AudioControl, proto 0
            "09 24 01 00 01 28 00 01 01",                            // AC HEADER bcdADC 0x0100
            "0C 24 02 01 01 01 00 02 03 00 00 00",                   // INPUT_TERMINAL #1
            "0A 24 06 02 01 01 03 00 00 00",                         // FEATURE_UNIT #2
            "09 24 03 03 01 03 00 02 00",                            // OUTPUT_TERMINAL #3
            "09 04 01 00 00 01 02 00 00",                            // if1 alt0 zero-bandwidth
            "09 04 01 01 01 01 02 00 00",                            // if1 alt1
            "07 24 01 01 00 01 00",                                  // AS_GENERAL (PCM)
            "14 24 02 01 02 02 10 04 00 77 01 88 58 01 80 BB 00 44 AC 00", // FORMAT_TYPE_I 16-bit, 96/88.2/48/44.1k
            "07 25 01 01 02 00 00",                                  // EP_GENERAL: SamplingFreq (BEFORE the EP!)
            "09 05 03 01 80 01 01 00 00",                            // EP 0x03 OUT iso/None, 384B, no synch
            "09 04 02 00 01 03 00 00 00",                            // if2 HID
            "09 21 11 01 00 01 22 62 00",
            "07 05 81 03 40 00 01",
            "09 04 03 00 01 03 00 00 00",                            // if3 HID
            "09 21 11 01 00 01 22 52 00",
            "07 05 89 03 40 00 01",
    )

    // ── FiiO BTR3K (0a12:1004) — lsusb, full-speed UAC1, MaxPacketsOnly ─
    private val btr3k = hex(
            "12 01 00 02 00 00 00 40 12 0A 04 10 05 36 01 02 03 01",
            "09 02 74 00 02 01 00 80 32",
            "09 04 00 00 00 01 01 00 00",
            "09 24 01 00 01 2B 00 01 01",
            "0C 24 02 01 01 01 00 02 03 00 00 00",
            "0D 24 06 02 01 02 01 00 02 00 02 00 00",                // FEATURE_UNIT, bControlSize 2
            "09 24 03 03 01 03 00 02 00",
            "09 04 01 00 00 01 02 00 00",
            "09 04 01 01 01 01 02 00 00",
            "07 24 01 01 00 01 00",
            "0E 24 02 01 02 02 10 02 80 BB 00 44 AC 00",             // 16-bit, 48/44.1k
            "07 25 01 81 02 00 00",                                  // EP_GENERAL: SamplingFreq + MaxPacketsOnly
            "09 05 03 01 C0 00 01 00 00",                            // EP 0x03 OUT, 192B
    )

    // ── AudioQuest DragonFly Black v1.5 (21b4:0083) — UAC1 explicit
    //    async with a 3-byte feedback endpoint, bRefresh 5 ──────────────
    private val dragonfly = hex(
            "12 01 00 02 00 00 00 40 B4 21 83 00 06 01 01 02 03 01",
            "09 02 98 00 03 01 00 80 14",
            "09 04 00 00 00 01 01 00 00",
            "09 24 01 00 01 27 00 01 01",
            "0C 24 02 09 01 01 00 02 03 00 00 00",                   // INPUT_TERMINAL #9
            "09 24 06 05 09 02 03 00 00",                            // FEATURE_UNIT #5
            "09 24 03 02 01 03 00 05 00",                            // OUTPUT_TERMINAL #2
            "09 04 01 00 00 01 02 00 00",
            "09 04 01 01 02 01 02 00 00",                            // alt1: TWO endpoints
            "07 24 01 09 01 01 00",
            "14 24 02 01 02 03 18 04 44 AC 00 80 BB 00 88 58 01 00 77 01", // 24-bit, 44.1/48/88.2/96k
            "09 05 01 05 4C 02 01 00 81",                            // EP 0x01 OUT iso/Async, 588B, synch→0x81
            "07 25 01 01 00 00 00",                                  // EP_GENERAL: SamplingFreq (AFTER the EP)
            "09 05 81 05 03 00 01 05 00",                            // EP 0x81 IN feedback, 3B, bRefresh 5
            "09 04 02 00 01 03 00 00 05",                            // if2 HID
            "09 21 11 01 00 01 22 15 00",
            "07 05 82 03 40 00 20",
    )

    // ── Apple USB-C→3.5mm adapter (05ac:110a), config 1 — UAC2 at full
    //    speed, synchronous endpoints, clock source #9. Audio interfaces
    //    verbatim; HID tail omitted (config header adjusted accordingly) ─
    private val appleUac2 = hex(
            "12 01 01 02 EF 02 01 40 AC 05 0A 11 11 26 01 02 03 03",
            "09 02 C6 00 02 01 00 A0 96",
            "08 0B 00 02 01 00 20 00",                               // IAD, function protocol 0x20
            "09 04 00 00 01 01 01 20 00",                            // if0 AC, proto 0x20 (UAC2)
            "09 24 01 00 02 04 40 00 00",                            // AC HEADER bcdADC 0x0200
            "11 24 02 01 01 01 00 09 02 03 00 00 00 00 00 00 00",    // INPUT_TERMINAL, bCSourceID 9
            "12 24 06 02 01 03 00 00 00 0C 00 00 00 0C 00 00 00 00", // FEATURE_UNIT (UAC2 layout)
            "0C 24 03 03 02 03 00 02 09 00 00 00",                   // OUTPUT_TERMINAL (Headphones)
            "08 24 0A 09 07 07 00 00",                               // CLOCK_SOURCE bClockID 9
            "07 05 81 03 06 00 01",                                  // interrupt EP in AC (ignored)
            "09 04 01 00 00 01 02 20 00",
            "09 04 01 01 01 01 02 20 00",                            // alt1: 24-bit
            "10 24 01 01 05 01 01 00 00 00 02 03 00 00 00 00",       // AS_GENERAL: PCM, 2ch
            "06 24 02 01 03 18",                                     // FORMAT_TYPE_I: subslot 3, 24-bit
            "07 05 02 0D 20 01 01",                                  // EP 0x02 OUT iso/Synchronous, 288B
            "08 25 01 00 00 01 64 00",                               // EP_GENERAL (UAC2)
            "09 04 01 02 01 01 02 20 00",                            // alt2: 16-bit
            "10 24 01 01 05 01 01 00 00 00 02 03 00 00 00 00",
            "06 24 02 01 02 10",
            "07 05 02 0D C0 00 01",                                  // EP 0x02 OUT, 192B
            "08 25 01 00 00 01 64 00",
    )

    // ── FiiO KA17 (2972:0093) — REAL raw bytes from pyusb GET_DESCRIPTOR
    //    (macOS), no hand-reconstruction. High-speed UAC2: clock source
    //    0x29 behind selector 0x28, FU#10 with MASTER Mute+Volume, three
    //    alts (32-bit PCM / 16-bit PCM / DSD with bmFormats 0x80000000),
    //    4-byte async feedback EP marked via usage bits. ──────────────
    private val ka17 = hex(
            "12 01 00 02 EF 02 01 40 72 29 93 00 25 02 01 03 02 02 09 02 55 01 05 01",
            "00 80 00 08 0B 00 02 01 00 20 00 09 04 00 00 00 01 01 20 03 09 24 01 00",
            "02 08 48 00 00 08 24 0A 29 03 07 00 09 08 24 0B 28 01 29 03 08 11 24 02",
            "02 01 01 00 28 02 00 00 00 00 0B 00 00 06 12 24 06 0A 02 0F 00 00 00 00",
            "00 00 00 00 00 00 00 00 0C 24 03 14 01 03 00 0A 28 00 00 00 09 04 01 00",
            "00 01 02 20 04 09 04 01 01 02 01 02 20 04 10 24 01 02 00 01 01 00 00 00",
            "02 00 00 00 00 0B 06 24 02 01 04 20 07 05 01 05 08 03 01 08 25 01 00 00",
            "02 08 00 07 05 81 11 04 00 04 09 04 01 02 02 01 02 20 04 10 24 01 02 00",
            "01 01 00 00 00 02 00 00 00 00 0B 06 24 02 01 02 10 07 05 01 05 84 01 01",
            "08 25 01 00 00 02 08 00 07 05 81 11 04 00 04 09 04 01 03 02 01 02 20 04",
            "10 24 01 02 00 01 00 00 00 80 02 00 00 00 00 0B 06 24 02 01 04 20 07 05",
            "01 05 08 03 01 08 25 01 00 00 02 08 00 07 05 81 11 04 00 04 09 04 02 00",
            "00 FE 01 01 0A 09 21 07 FA 00 40 00 10 01 09 04 03 00 01 03 00 00 0E 09",
            "21 10 01 00 01 22 15 00 07 05 83 03 40 00 08 09 04 04 00 02 03 00 00 06",
            "09 21 11 01 00 01 22 40 00 07 05 84 03 40 00 08 07 05 02 03 40 00 08",
    )

    // ── KM-HIFI-384KHZ dongle (3302:3366, TTGK) — Thesycon dump.
    //    High-speed UAC2 × SYNCHRONOUS (bmAttributes 0x0D, no feedback
    //    EP — SOF-locked): the nominal-pacing sink combination. FU#2 is
    //    the per-channel-volume layout (master carries only Mute). ─────
    private val kmHifi = hex(
            "12 01 01 02 EF 02 01 40 02 33 66 33 01 00 01 02 00 01 09 02 0E 01 03 01",
            "04 A0 32 08 0B 00 02 01 00 20 00 09 04 00 00 00 01 01 20 00 09 24 01 00",
            "02 04 40 00 00 08 24 0A 09 03 07 00 00 11 24 02 01 01 01 00 09 02 03 00",
            "00 00 00 00 00 00 12 24 06 02 01 03 00 00 00 0C 00 00 00 0C 00 00 00 00",
            "0C 24 03 03 02 03 00 02 09 00 00 00 09 04 01 00 00 01 02 20 00 09 04 01",
            "01 01 01 02 20 00 10 24 01 01 00 01 01 00 00 00 02 03 00 00 00 00 06 24",
            "02 01 02 10 07 05 01 0D C0 00 01 08 25 01 00 00 02 02 00 09 04 01 02 01",
            "01 02 20 00 10 24 01 01 00 01 01 00 00 00 02 03 00 00 00 00 06 24 02 01",
            "03 18 07 05 01 0D 20 01 01 08 25 01 00 00 02 02 00 09 04 01 03 01 01 02",
            "20 00 10 24 01 01 00 01 01 00 00 00 02 03 00 00 00 00 06 24 02 01 04 20",
            "07 05 01 0D 80 01 01 08 25 01 00 00 02 02 00 08 0B 02 01 03 00 00 00 09",
            "04 02 00 01 03 00 00 00 09 21 11 01 00 01 22 3F 00 07 05 83 03 40 00 07",
    )

    @Test
    fun btr13_parsesAsUac1FullLayout() {
        val layout = UsbAudioDescriptorParser.parse(btr13)
        assertNotNull(layout)
        layout!!
        assertEquals(UacVersion.UAC1, layout.uacVersion)
        assertEquals(0, layout.controlInterfaceId)
        assertEquals(-1, layout.clockSourceId)
        assertEquals(1, layout.streamingAlts.size)

        val alt = layout.streamingAlts[0]
        assertEquals(1, alt.interfaceId)
        assertEquals(1, alt.altSetting)
        assertEquals(2, alt.channels)
        assertEquals(2, alt.subslotSize)
        assertEquals(16, alt.bitResolution)
        assertEquals(listOf(96000, 88200, 48000, 44100), alt.sampleRates)
        assertNull(alt.continuousRateRange)
        assertEquals(0x03, alt.endpointAddress)
        assertEquals(384, alt.maxPacketSize)
        assertEquals(1, alt.interval)
        assertEquals(UsbSyncType.NONE, alt.syncType)
        assertEquals(0, alt.synchAddress)
        assertNull(alt.feedback)
        assertFalse(alt.syncType.needsFeedback)
        // CS_ENDPOINT precedes the standard EP on this device — the
        // attributes must still land on the alt setting.
        assertTrue(alt.hasSampleRateControl)
        assertFalse(alt.maxPacketsOnly)
        assertEquals(4, alt.bytesPerFrame)
        // Every advertised rate fits the 384-byte packet budget.
        for (rate in alt.sampleRates) {
            assertTrue("rate $rate must fit", alt.rateFitsMaxPacket(rate))
        }
        // 96k needs exactly wMaxPacketSize — and 176.4k must NOT fit.
        assertFalse(alt.rateFitsMaxPacket(176400))
        assertTrue(alt.supportsRate(44100))
        assertFalse(alt.supportsRate(192000))
    }

    @Test
    fun btr3k_reportsMaxPacketsOnly() {
        val layout = UsbAudioDescriptorParser.parse(btr3k)!!
        assertEquals(UacVersion.UAC1, layout.uacVersion)
        val alt = layout.streamingAlts.single()
        assertEquals(listOf(48000, 44100), alt.sampleRates)
        assertEquals(192, alt.maxPacketSize)
        assertEquals(16, alt.bitResolution)
        assertTrue(alt.hasSampleRateControl)
        assertTrue(alt.maxPacketsOnly)
        assertEquals(UsbSyncType.NONE, alt.syncType)
        assertNull(alt.feedback)
    }

    @Test
    fun dragonfly_parsesAsyncFeedbackEndpoint() {
        val layout = UsbAudioDescriptorParser.parse(dragonfly)!!
        assertEquals(UacVersion.UAC1, layout.uacVersion)
        assertEquals(-1, layout.clockSourceId)

        val alt = layout.streamingAlts.single()
        assertEquals(2, alt.channels)
        assertEquals(3, alt.subslotSize)
        assertEquals(24, alt.bitResolution)
        assertEquals(listOf(44100, 48000, 88200, 96000), alt.sampleRates)
        assertEquals(0x01, alt.endpointAddress)
        assertEquals(588, alt.maxPacketSize)
        assertEquals(UsbSyncType.ASYNC, alt.syncType)
        assertTrue(alt.syncType.needsFeedback)
        assertEquals(0x81, alt.synchAddress)

        val fb = alt.feedback
        assertNotNull(fb)
        fb!!
        assertEquals(0x81, fb.address)
        assertEquals(3, fb.maxPacketSize) // full-speed Q10.14 payload
        assertEquals(5, fb.refresh)       // feedback every 2^5 = 32 ms
        assertEquals(1, fb.interval)

        assertTrue(alt.hasSampleRateControl)
        assertFalse(alt.maxPacketsOnly)
        // 96k × 6 B/frame = 576 ≤ 588
        assertTrue(alt.rateFitsMaxPacket(96000))
    }

    @Test
    fun appleAdapter_parsesAsUac2WithClockSource() {
        val layout = UsbAudioDescriptorParser.parse(appleUac2)!!
        assertEquals(UacVersion.UAC2, layout.uacVersion)
        assertEquals(0, layout.controlInterfaceId)
        assertEquals(9, layout.clockSourceId)
        assertEquals(2, layout.streamingAlts.size)

        val alt1 = layout.streamingAlts[0]
        assertEquals(1, alt1.altSetting)
        assertEquals(2, alt1.channels)      // from UAC2 AS_GENERAL
        assertEquals(3, alt1.subslotSize)
        assertEquals(24, alt1.bitResolution)
        assertTrue(alt1.sampleRates.isEmpty()) // UAC2: rates via clock RANGE
        assertEquals(0x02, alt1.endpointAddress)
        assertEquals(288, alt1.maxPacketSize)
        assertEquals(UsbSyncType.SYNC, alt1.syncType)
        assertNull(alt1.feedback)
        assertFalse(alt1.hasSampleRateControl) // UAC2 has no per-EP rate bit
        assertFalse(alt1.maxPacketsOnly)
        assertTrue(alt1.supportsRate(48000)) // unknown → permissive

        val alt2 = layout.streamingAlts[1]
        assertEquals(2, alt2.altSetting)
        assertEquals(2, alt2.subslotSize)
        assertEquals(16, alt2.bitResolution)
        assertEquals(192, alt2.maxPacketSize)
    }

    @Test
    fun volume_btr13_masterMuteAndVolume() {
        // FU#2: bControlSize=1, bmaControls = [0x03, 0x00, 0x00] —
        // Mute+Volume on the master channel; OT#3 (Speaker) sources FU#2.
        val vol = UsbAudioDescriptorParser.parse(btr13)!!.volume
        assertNotNull(vol)
        vol!!
        assertEquals(2, vol.unitId)
        assertTrue(vol.masterVolume)
        assertTrue(vol.masterMute)
        assertEquals(listOf(0), vol.writeChannels)
    }

    @Test
    fun volume_btr3k_perChannel() {
        // FU#2: bControlSize=2, master=0x0001 (mute only),
        // ch1=ch2=0x0002 (volume) — per-channel writes required.
        val vol = UsbAudioDescriptorParser.parse(btr3k)!!.volume!!
        assertEquals(2, vol.unitId)
        assertFalse(vol.masterVolume)
        assertTrue(vol.masterMute)
        assertEquals(listOf(1, 2), vol.volumeChannels)
        assertEquals(listOf(1, 2), vol.writeChannels)
    }

    @Test
    fun volume_dragonfly_masterViaOutputTerminal() {
        // FU#5 master mute+volume; OT#2 (Speaker) sources FU#5 directly.
        val vol = UsbAudioDescriptorParser.parse(dragonfly)!!.volume!!
        assertEquals(5, vol.unitId)
        assertTrue(vol.masterVolume)
    }

    @Test
    fun volume_appleUac2_perChannelWritable() {
        // UAC2 FU#2: master bmaControls=0x00000003 (mute r/w only),
        // ch1/ch2 = 0x0000000C (volume r/w) — Apple's per-channel layout.
        val vol = UsbAudioDescriptorParser.parse(appleUac2)!!.volume!!
        assertEquals(2, vol.unitId)
        assertFalse(vol.masterVolume)
        assertTrue(vol.masterMute)
        assertEquals(listOf(1, 2), vol.writeChannels)
    }

    @Test
    fun speedHint_negativeOnFullSpeedDevices() {
        assertFalse(UsbAudioDescriptorParser.definitelyHighSpeed(btr13))
        assertFalse(UsbAudioDescriptorParser.definitelyHighSpeed(btr3k))
        assertFalse(UsbAudioDescriptorParser.definitelyHighSpeed(dragonfly))
        assertFalse(UsbAudioDescriptorParser.definitelyHighSpeed(appleUac2))
    }

    @Test
    fun speedHint_positiveOnHighSpeedOnlyValues() {
        // 512-byte bulk endpoint — illegal below high speed.
        assertTrue(UsbAudioDescriptorParser.definitelyHighSpeed(
                hex("07 05 02 02 00 02 00")))
        // Iso payload 1024 (> 1023) — high-bandwidth only.
        assertTrue(UsbAudioDescriptorParser.definitelyHighSpeed(
                hex("07 05 01 05 00 04 01")))
        // Iso mult bits set (2 transactions/microframe).
        assertTrue(UsbAudioDescriptorParser.definitelyHighSpeed(
                hex("07 05 01 05 00 0C 01")))
    }

    @Test
    fun ka17_parsesRealHighSpeedUac2Bytes() {
        val layout = UsbAudioDescriptorParser.parse(ka17)!!
        assertEquals(UacVersion.UAC2, layout.uacVersion)
        assertEquals(0, layout.controlInterfaceId)
        assertEquals(0x29, layout.clockSourceId) // the SOURCE, not selector 0x28
        assertEquals(3, layout.streamingAlts.size)

        val alt1 = layout.streamingAlts[0]
        assertEquals(1, alt1.altSetting)
        assertEquals(2, alt1.channels)
        assertEquals(4, alt1.subslotSize)
        assertEquals(32, alt1.bitResolution)
        assertEquals(776, alt1.maxPacketSize)
        assertEquals(UsbSyncType.ASYNC, alt1.syncType)
        assertTrue(alt1.sampleRates.isEmpty()) // UAC2: via clock RANGE
        val fb = alt1.feedback!!
        assertEquals(0x81, fb.address)
        assertEquals(4, fb.maxPacketSize)      // Q16.16 at high speed
        assertEquals(4, fb.interval)           // 2^(4-1) microframes = 1 ms
        assertEquals(0, fb.refresh)            // 7-byte descriptor: no bRefresh

        assertEquals(16, layout.streamingAlts[1].bitResolution)
        assertEquals(388, layout.streamingAlts[1].maxPacketSize)
        // alt3 is the DSD alt (bmFormats RAW_DATA) — bit depth parses as 32;
        // format-tag awareness is a future refinement, the best-alt picker
        // takes the FIRST highest-bits alt (alt1, PCM) either way.
        assertEquals(32, layout.streamingAlts[2].bitResolution)

        val vol = layout.volume!!
        assertEquals(10, vol.unitId)           // OT#20 (Speaker) ← FU#10
        assertTrue(vol.masterVolume)
        assertTrue(vol.masterMute)
        assertEquals(listOf(0), vol.writeChannels)

        // 776 ≤ 1023 and no HS-only invariants — descriptors alone cannot
        // prove high speed; that is exactly why GET_SPEED exists.
        assertFalse(UsbAudioDescriptorParser.definitelyHighSpeed(ka17))
    }

    @Test
    fun kmHifi_parsesSynchronousUac2WithPerChannelVolume() {
        val layout = UsbAudioDescriptorParser.parse(kmHifi)!!
        assertEquals(UacVersion.UAC2, layout.uacVersion)
        assertEquals(9, layout.clockSourceId)
        assertEquals(3, layout.streamingAlts.size)

        val bits = layout.streamingAlts.map { it.bitResolution }
        assertEquals(listOf(16, 24, 32), bits)
        val pkts = layout.streamingAlts.map { it.maxPacketSize }
        assertEquals(listOf(192, 288, 384), pkts)
        for (alt in layout.streamingAlts) {
            assertEquals(UsbSyncType.SYNC, alt.syncType)
            assertNull(alt.feedback)          // SOF-locked: nominal pacing
            assertFalse(alt.syncType.needsFeedback)
        }

        val vol = layout.volume!!
        assertEquals(2, vol.unitId)           // OT#3 (Headphones) ← FU#2
        assertFalse(vol.masterVolume)         // master carries only Mute
        assertTrue(vol.masterMute)
        assertEquals(listOf(1, 2), vol.writeChannels)
    }

    @Test
    fun garbageInput_returnsNullOrEmpty() {
        assertNull(UsbAudioDescriptorParser.parse(ByteArray(0)))
        assertNull(UsbAudioDescriptorParser.parse(hex("12 34 56")))
        // Device with no audio function at all (single HID interface).
        assertNull(UsbAudioDescriptorParser.parse(hex(
                "09 02 22 00 01 01 00 80 32",
                "09 04 00 00 01 03 00 00 00",
                "09 21 11 01 00 01 22 40 00",
                "07 05 81 03 40 00 01",
        )))
    }

    private fun hex(vararg rows: String): ByteArray =
            rows.joinToString(" ")
                    .split(Regex("\\s+"))
                    .filter { it.isNotBlank() }
                    .map { it.toInt(16).toByte() }
                    .toByteArray()
}
