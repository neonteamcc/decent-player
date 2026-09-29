package com.decent.usbaudio.media3

/** Serializes a setting with stream publication/removal. A setter cannot use
 * a detached stream, and a stream created during a toggle gets the latest value.
 */
internal class UsbLimiterState<T : Any>(private val apply: (T, Boolean) -> Unit) {
    private var enabled = false
    private var stream: T? = null

    @Synchronized fun setEnabled(value: Boolean) {
        enabled = value
        stream?.let { apply(it, value) }
    }

    @Synchronized fun attach(value: T) {
        apply(value, enabled)
        stream = value
    }

    @Synchronized fun detach(): T? = stream.also { stream = null }
    @Synchronized fun current(): T? = stream
}
