package com.decent.usbaudio.media3

import android.util.Log
import com.decent.usbaudio.UsbAudioStream
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/** Serial owner of USB writes and end-of-stream draining. */
class UsbStreamingThread internal constructor(private val output: Output) {
    internal interface Output {
        fun write(data: FloatArray)
        fun writeRaw(data: ByteArray, encoding: Int)
        fun finish(): Boolean
        fun flush()
        /** [UsbAudioStream.stopErrno]: why the driver stopped sending, or 0. */
        fun stopErrno(): Int = 0
    }

    constructor(stream: UsbAudioStream) : this(object : Output {
        override fun write(data: FloatArray) = stream.write(data)
        override fun writeRaw(data: ByteArray, encoding: Int) = stream.writeRaw(data, encoding)
        override fun finish() = stream.finish()
        override fun flush() = stream.flush()
        override fun stopErrno() = stream.stopErrno
    })

    /**
     * The driver stopped sending audio to the DAC on its own: every write
     * since is dropped, so the DAC is certainly silent. Raised as soon as the
     * write that met it returns, instead of the track playing on in silence
     * until its end-of-stream drain failed.
     */
    class UsbStreamStoppedException(val errno: Int) :
        IllegalStateException("USB stream stopped by the driver (errno=$errno)")

    private sealed class Command(val generation: Long) {
        class FloatBuffer(val data: FloatArray, generation: Long) : Command(generation)
        class RawBuffer(val data: ByteArray, val encoding: Int, generation: Long) : Command(generation)
        class End(generation: Long) : Command(generation)
    }

    private val queue = ArrayBlockingQueue<Command>(128)
    private val stateLock = Any()
    private val ioLock = Any()
    private var generation = 0L
    private var pendingBuffers = 0 // includes the buffer currently inside native write()
    private var endRequested = false
    private var endDrained = false
    @Volatile private var running = false
    @Volatile private var paused = false
    @Volatile var failure: Exception? = null
        private set
    private var thread: Thread? = null

    fun start() {
        running = true
        thread = Thread({
            Log.i(TAG, "USB streaming thread started")
            try {
                while (running) {
                    if (paused) { Thread.sleep(50); continue }
                    val qBefore = queue.size
                    val command = queue.poll(100, TimeUnit.MILLISECONDS)
                    if (command == null) {
                        if (synchronized(stateLock) { !endRequested }) Log.w(TAG, "Queue EMPTY — poll timeout")
                        continue
                    }
                    synchronized(ioLock) {
                        if (synchronized(stateLock) { command.generation == generation }) when (command) {
                            is Command.FloatBuffer -> { output.write(command.data); throwIfDriverStopped() }
                            is Command.RawBuffer -> { output.writeRaw(command.data, command.encoding); throwIfDriverStopped() }
                            is Command.End -> {
                                // A device that is going away has nothing left
                                // to drain, and its host is already rebuilding
                                // the output; failing here put an error on the
                                // screen for an unplug at the end of a track.
                                check(output.finish() || deviceGone(output.stopErrno())) {
                                    "USB end-of-stream drain failed"
                                }
                                synchronized(stateLock) {
                                    if (command.generation == generation) endDrained = true
                                }
                            }
                        }
                    }
                    if (command !is Command.End) synchronized(stateLock) {
                        if (command.generation == generation) {
                            pendingBuffers--
                            if (qBefore <= 1 && !endRequested) Log.w(TAG, "Queue nearly empty: $qBefore before write")
                        }
                    }
                }
            } catch (e: Exception) {
                if (running) {
                    failure = e
                    Log.e(TAG, "USB streaming failed", e)
                }
            } finally {
                running = false
                Log.i(TAG, "USB streaming thread exited")
            }
        }, "UsbStreamingThread").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    /**
     * Surface a driver that gave up — except for a device that is going
     * away: an unplug is the host's to handle (it rebuilds the output on the
     * detach), and reporting it as a failure would put an error on the screen
     * for every cable pulled mid-track.
     */
    private fun throwIfDriverStopped() {
        val errno = output.stopErrno()
        if (errno != 0 && !deviceGone(errno)) throw UsbStreamStoppedException(errno)
    }

    private fun deviceGone(errno: Int) =
        errno == UsbAudioStream.ENODEV || errno == UsbAudioStream.ESHUTDOWN

    fun enqueue(data: FloatArray): Boolean = synchronized(stateLock) {
        enqueueLocked(Command.FloatBuffer(data, generation))
    }

    fun enqueueRaw(data: ByteArray, encoding: Int): Boolean = synchronized(stateLock) {
        enqueueLocked(Command.RawBuffer(data, encoding, generation))
    }

    private fun enqueueLocked(command: Command): Boolean {
        // An EOS marker must finish before a subsequent stream can enter the queue.
        if (endRequested && !endDrained) return false
        if (!queue.offer(command)) return false
        pendingBuffers++
        endRequested = false
        endDrained = false
        return true
    }

    /** Ordered after every accepted PCM buffer. Repeated calls are idempotent. */
    fun finish() = synchronized(stateLock) {
        if (!endRequested && queue.offer(Command.End(generation))) {
            endRequested = true
            endDrained = false
        }
    }

    fun isEnded(): Boolean = synchronized(stateLock) { endRequested && endDrained }
    fun hasPendingData(): Boolean = synchronized(stateLock) {
        pendingBuffers > 0 || (endRequested && !endDrained)
    }
    fun queueSize(): Int = synchronized(stateLock) { pendingBuffers }
    fun pauseStreaming() { paused = true }
    fun resumeStreaming() { paused = false }

    /** Explicit seek/flush discards queued PCM; stale drain completion cannot end the new stream. */
    fun flush() {
        invalidateQueue()
        synchronized(ioLock) { output.flush() }
    }

    private fun invalidateQueue() = synchronized(stateLock) {
        generation++
        queue.clear()
        pendingBuffers = 0
        endRequested = false
        endDrained = false
    }

    fun stop() {
        running = false
        invalidateQueue()
        thread?.join(2000)
        thread = null
    }

    private companion object { const val TAG = "UsbStreamingThread" }
}
