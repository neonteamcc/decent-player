package com.decent.usbaudio.media3

import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

class UsbStreamingThreadTest {
    private class Output : UsbStreamingThread.Output {
        val writes = Collections.synchronizedList(mutableListOf<Int>())
        val enteredWrite = CountDownLatch(1)
        val allowWrite = CountDownLatch(1)
        val enteredFinish = CountDownLatch(1)
        val allowFinish = CountDownLatch(1)
        override fun write(data: FloatArray) = writeRaw(byteArrayOf(data[0].toInt().toByte()), 2)
        override fun writeRaw(data: ByteArray, encoding: Int) {
            enteredWrite.countDown()
            check(allowWrite.await(5, TimeUnit.SECONDS))
            writes.add(data[0].toInt())
        }
        override fun finish(): Boolean {
            enteredFinish.countDown()
            return allowFinish.await(5, TimeUnit.SECONDS)
        }
        override fun flush() = Unit
    }

    @Test fun endWaitsForTheActiveWriteEveryQueuedBufferAndNativeDrain() {
        val out = Output()
        val worker = UsbStreamingThread(out)
        try {
            worker.start()
            assertTrue(worker.enqueueRaw(byteArrayOf(1), 2))
            assertTrue(out.enteredWrite.await(5, TimeUnit.SECONDS))
            assertTrue(worker.hasPendingData()) // the queue is empty; write is still in progress
            assertTrue(worker.enqueueRaw(byteArrayOf(2), 2))
            assertTrue(worker.enqueueRaw(byteArrayOf(3), 2))
            worker.finish()
            worker.finish()
            assertFalse(worker.isEnded())
            assertFalse(worker.enqueueRaw(byteArrayOf(4), 2))
            out.allowWrite.countDown()
            assertTrue(out.enteredFinish.await(5, TimeUnit.SECONDS))
            assertEquals(listOf(1, 2, 3), out.writes.toList())
            assertTrue(worker.hasPendingData()) // now only native URBs remain
            assertFalse(worker.isEnded())
            out.allowFinish.countDown()
            await { worker.isEnded() }
            assertFalse(worker.hasPendingData())
            assertTrue(worker.enqueueRaw(byteArrayOf(4), 2))
            assertFalse(worker.isEnded())
            worker.finish()
            await { worker.isEnded() }
            assertEquals(listOf(1, 2, 3, 4), out.writes.toList())
            assertNull(worker.failure)
        } finally {
            out.allowWrite.countDown(); out.allowFinish.countDown(); worker.stop()
        }
    }

    @Test fun aPausedStreamKeepsItsTailUntilPlay() {
        val out = Output()
        out.allowWrite.countDown(); out.allowFinish.countDown()
        val worker = UsbStreamingThread(out)
        try {
            worker.pauseStreaming(); worker.start()
            assertTrue(worker.enqueueRaw(byteArrayOf(1), 2))
            worker.finish()
            assertFalse(out.enteredWrite.await(100, TimeUnit.MILLISECONDS))
            assertFalse(worker.isEnded())
            worker.resumeStreaming()
            await { worker.isEnded() }
            assertEquals(listOf(1), out.writes.toList())
        } finally { worker.stop() }
    }

    @Test fun explicitFlushDiscardsTheQueuedGeneration() {
        val out = Output()
        out.allowWrite.countDown(); out.allowFinish.countDown()
        val worker = UsbStreamingThread(out)
        try {
            worker.pauseStreaming(); worker.start()
            worker.enqueueRaw(byteArrayOf(1), 2); worker.finish()
            worker.flush()
            assertFalse(worker.hasPendingData())
            assertFalse(worker.isEnded())
            worker.enqueueRaw(byteArrayOf(2), 2); worker.finish()
            worker.resumeStreaming()
            await { worker.isEnded() }
            assertEquals(listOf(2), out.writes.toList())
        } finally { worker.stop() }
    }

    @Test fun aDrainFinishingDuringFlushCannotEndTheNextStream() {
        val out = Output()
        out.allowWrite.countDown()
        val worker = UsbStreamingThread(out)
        val flushed = CountDownLatch(1)
        var flushThread: Thread? = null
        try {
            worker.start()
            worker.enqueueRaw(byteArrayOf(1), 2); worker.finish()
            assertTrue(out.enteredFinish.await(5, TimeUnit.SECONDS))
            flushThread = Thread { worker.flush(); flushed.countDown() }.apply { start() }
            await { !worker.hasPendingData() } // generation invalidated before waiting for native I/O
            out.allowFinish.countDown()
            assertTrue(flushed.await(5, TimeUnit.SECONDS))
            assertFalse(worker.isEnded())
            worker.enqueueRaw(byteArrayOf(2), 2); worker.finish()
            await { worker.isEnded() }
            assertEquals(listOf(1, 2), out.writes.toList())
        } finally {
            out.allowFinish.countDown(); flushThread?.join(5000); worker.stop()
        }
    }

    private fun await(predicate: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (!predicate() && System.nanoTime() < deadline) Thread.sleep(1)
        assertTrue(predicate())
    }
}
