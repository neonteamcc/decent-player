package com.decent.usbaudio.descriptor

import org.junit.Assert.*
import org.junit.Test

/** Synthetic descriptor fragments for format selection, not hardware captures. */
class UsbSampleFormatTest {
    private fun hex(text: String) = text.trim().split(Regex("\\s+")).map { it.toInt(16).toByte() }.toByteArray()

    private fun uac2Alt(alt: Int, formats: Int, channels: Int = 2, bits: Int = 32, slot: Int = 4): ByteArray {
        val general = hex("10 24 01 01 00 01 00 00 00 00 02 03 00 00 00 00")
        repeat(4) { general[6 + it] = (formats ushr (8 * it)).toByte() }
        general[10] = channels.toByte()
        val iface = hex("09 04 01 01 01 01 02 20 00").apply { this[3] = alt.toByte() }
        val format = hex("06 24 02 01 04 20").apply { this[4] = slot.toByte(); this[5] = bits.toByte() }
        return iface + general + format + hex("07 05 01 0d 80 01 01")
    }

    private fun uac2(configuration: Int = 1, vararg alts: ByteArray): ByteArray {
        val body = hex("09 04 00 00 00 01 01 20 00 09 24 01 00 02 01 11 00 00 08 24 0a 01 03 07 00 00 09 04 01 00 00 01 02 20 00") +
            alts.fold(byteArrayOf()) { a, b -> a + b }
        val header = hex("09 02 00 00 02 01 00 80 32")
        val total = header.size + body.size
        header[2] = total.toByte(); header[3] = (total ushr 8).toByte(); header[5] = configuration.toByte()
        return header + body
    }

    @Test fun uac2RecognizesPcmAndFloatWithoutConfusingRawData() {
        val layout = UsbAudioDescriptorParser.parse(uac2(1,
            uac2Alt(1, 1), uac2Alt(2, 4), uac2Alt(3, Int.MIN_VALUE)))!!
        assertEquals(listOf(UsbSampleFormat.PCM, UsbSampleFormat.IEEE_FLOAT, UsbSampleFormat.UNSUPPORTED),
            layout.streamingAlts.map { it.sampleFormat })
        assertEquals(1, layout.selectPlaybackAlt(1, false, 2)?.altSetting)
        assertEquals(2, layout.selectPlaybackAlt(1, true, 2)?.altSetting)
    }

    @Test fun floatOnlyDeviceIsUsableForIntegerInput() {
        val layout = UsbAudioDescriptorParser.parse(uac2(1, uac2Alt(1, 4)))!!
        assertEquals(UsbSampleFormat.IEEE_FLOAT, layout.selectPlaybackAlt(1, false, 2)?.sampleFormat)
    }

    @Test fun floatRequiresA32BitFourByteSlotAndMatchingChannels() {
        val layout = UsbAudioDescriptorParser.parse(uac2(1,
            uac2Alt(1, 4, bits = 24, slot = 3), uac2Alt(2, 4, channels = 6), uac2Alt(3, 1)))!!
        assertFalse(layout.streamingAlts[0].isSupported)
        assertEquals(3, layout.selectPlaybackAlt(1, true, 2)?.altSetting)
        assertEquals(2, layout.selectPlaybackAlt(1, true, 6)?.altSetting)
    }

    @Test fun ambiguousOrUnsupportedFormatsAreNeverSelectedAsPcm() {
        val layout = UsbAudioDescriptorParser.parse(uac2(1, uac2Alt(1, 5), uac2Alt(2, Int.MIN_VALUE)))!!
        assertNull(layout.selectPlaybackAlt(1, false, 2))
        assertNull(layout.selectPlaybackAlt(1, true, 2))
    }

    @Test fun inactiveConfigurationCannotSupplyTheFloatAlt() {
        val raw = uac2(1, uac2Alt(1, 1)) + uac2(2, uac2Alt(1, 4))
        val first = UsbAudioDescriptorParser.parse(raw)!!
        assertEquals(1, first.streamingAlts.size)
        assertEquals(UsbSampleFormat.PCM, first.selectPlaybackAlt(1, true, 2)?.sampleFormat)
        val second = UsbAudioDescriptorParser.parse(raw, 2)!!
        assertEquals(1, second.streamingAlts.size)
        assertEquals(UsbSampleFormat.IEEE_FLOAT, second.selectPlaybackAlt(1, true, 2)?.sampleFormat)
    }

    @Test fun uac1UsesTheFormatTagRatherThanTheSampleWidth() {
        fun descriptors(tag: Int): ByteArray {
            val general = hex("07 24 01 01 00 01 00").apply { this[5] = tag.toByte() }
            return hex("09 04 00 00 00 01 01 00 00 09 24 01 00 01 09 00 01 01 09 04 01 01 01 01 02 00 00") +
                general + hex("0b 24 02 01 02 04 20 01 80 bb 00 07 05 01 0d 80 01 01")
        }
        assertEquals(UsbSampleFormat.PCM, UsbAudioDescriptorParser.parse(descriptors(1))!!.streamingAlts.single().sampleFormat)
        assertEquals(UsbSampleFormat.IEEE_FLOAT, UsbAudioDescriptorParser.parse(descriptors(3))!!.streamingAlts.single().sampleFormat)
        assertEquals(UsbSampleFormat.UNSUPPORTED, UsbAudioDescriptorParser.parse(descriptors(2))!!.streamingAlts.single().sampleFormat)
    }
}
