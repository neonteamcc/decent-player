package com.decent.usbaudio.descriptor

/**
 * Map a track rate onto a playback alt setting.
 *
 * Acceptance for a candidate rate: advertised by the alt setting
 * (UAC1 Format Type I list) AND by the device (UAC2 clock RANGE list,
 * [deviceRates], when known) AND fits the wMaxPacketSize budget at the bus
 * speed AND, on full-speed MaxPacketsOnly devices, produces exactly
 * wMaxPacketSize-sized nominal packets (such firmwares — FiiO BTR3 class —
 * misbehave at every other rate; that is the historical BTR3 44.1 kHz Linux
 * bug).
 *
 * Preference order: exact → in-family ÷2/÷4 (half-band, cheapest) →
 * smallest acceptable rate above the track (content-preserving upsample,
 * e.g. 44.1k → 48k) → largest acceptable below. No acceptable candidate →
 * passthrough attempt.
 */
object UsbRateChoice {

    /** @return Pair(usbRate, conversionInputRate) — input 0 = passthrough. */
    fun choose(
        alt: StreamingAltSetting,
        deviceRates: List<Int>,
        busSpeed: UsbBusSpeed,
        trackRate: Int,
    ): Pair<Int, Int> {
        fun ok(r: Int): Boolean {
            if (!alt.supportsRate(r)) return false
            if (deviceRates.isNotEmpty() && r !in deviceRates) return false
            if (!alt.rateFitsMaxPacket(r, busSpeed.packetsPerSecond)) return false
            if (busSpeed == UsbBusSpeed.FULL && alt.maxPacketsOnly &&
                (r % 1000 != 0 || (r / 1000) * alt.bytesPerFrame != alt.maxPacketSize)
            ) return false
            return true
        }
        if (ok(trackRate)) return trackRate to 0
        if (trackRate % 2 == 0 && ok(trackRate / 2)) return trackRate / 2 to trackRate
        if (trackRate % 4 == 0 && ok(trackRate / 4)) return trackRate / 4 to trackRate
        val candidates = (alt.sampleRates + deviceRates).distinct().filter { ok(it) }
        val target = candidates.filter { it > trackRate }.minOrNull()
            ?: candidates.filter { it < trackRate }.maxOrNull()
            ?: return trackRate to 0 // nothing known — passthrough attempt
        return target to trackRate
    }
}
