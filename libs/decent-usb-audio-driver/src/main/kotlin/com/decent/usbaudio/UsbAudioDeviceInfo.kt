package com.decent.usbaudio

import android.hardware.usb.UsbDeviceConnection
import com.decent.usbaudio.descriptor.UacVersion
import com.decent.usbaudio.descriptor.UsbAudioDeviceLayout
import com.decent.usbaudio.descriptor.UsbBusSpeed

/**
 * Information about an opened USB audio device, ready for native I/O.
 *
 * The class/speed fields default to the pre-fork behavior (UAC2 at high
 * speed) so existing callers keep working; [UsbAudioDevice.openDevice]
 * fills them from the parsed descriptors and the GET_SPEED ioctl.
 */
data class UsbAudioDeviceInfo(
    val connection: UsbDeviceConnection,
    val fd: Int,
    val deviceName: String,
    val interfaceId: Int,
    val endpointOutAddress: Int,
    val endpointFeedbackAddress: Int,
    val maxPacketSize: Int,
    val altSettingCount: Int,
    val clockSourceId: Int,
    val bestAltSetting: Int,
    val bestBitDepth: Int,

    /** Audio class of the device's audio function. */
    val uacVersion: UacVersion = UacVersion.UAC2,

    /** Operating bus speed (drives ISO packet cadence: 1 ms vs 125 µs). */
    val busSpeed: UsbBusSpeed = UsbBusSpeed.HIGH,

    /**
     * Discrete sample rates of [bestAltSetting], from the UAC1 Format
     * Type I descriptor. Empty for UAC2 (rates live in the clock source).
     */
    val sampleRates: List<Int> = emptyList(),

    /** Full parsed descriptor layout, when parsing succeeded. */
    val layout: UsbAudioDeviceLayout? = null,
)
