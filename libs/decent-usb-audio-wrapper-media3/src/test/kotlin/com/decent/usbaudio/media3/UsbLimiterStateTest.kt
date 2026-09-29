package com.decent.usbaudio.media3

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

class UsbLimiterStateTest {
    private class Target {
        @Volatile var enabled = false
        @Volatile var released = false
    }

    @Test fun defaultsOffAndFutureStreamsInheritTheLatestExplicitSetting() {
        val owner = UsbLimiterState<Target> { target, value ->
            check(!target.released)
            target.enabled = value
        }
        val first = Target()
        owner.attach(first)
        assertFalse(first.enabled)
        owner.setEnabled(true)
        assertTrue(first.enabled)
        assertSame(first, owner.detach())
        first.released = true
        owner.setEnabled(false)
        val next = Target()
        owner.attach(next)
        assertFalse(next.enabled)
    }

    @Test fun aToggleDuringPublicationCannotBeLost() {
        val entered = CountDownLatch(1)
        val publish = CountDownLatch(1)
        val owner = UsbLimiterState<Target> { target, value ->
            if (!value) { entered.countDown(); check(publish.await(5, TimeUnit.SECONDS)) }
            target.enabled = value
        }
        val target = Target()
        val attaching = Thread { owner.attach(target) }.apply { start() }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        val toggling = Thread { owner.setEnabled(true) }.apply { start() }
        publish.countDown()
        attaching.join(5000); toggling.join(5000)
        assertFalse(attaching.isAlive); assertFalse(toggling.isAlive)
        assertSame(target, owner.current())
        assertTrue(target.enabled)
    }

    @Test fun detachWaitsForAnActiveSetterBeforeTheStreamCanBeFreed() {
        val entered = CountDownLatch(1)
        val finishSetter = CountDownLatch(1)
        val detached = CountDownLatch(1)
        val owner = UsbLimiterState<Target> { target, value ->
            if (value) { entered.countDown(); check(finishSetter.await(5, TimeUnit.SECONDS)) }
            check(!target.released)
            target.enabled = value
        }
        val target = Target()
        owner.attach(target)
        val setter = Thread { owner.setEnabled(true) }.apply { start() }
        assertTrue(entered.await(5, TimeUnit.SECONDS))
        val releasing = Thread {
            owner.detach()?.released = true
            detached.countDown()
        }.apply { start() }
        try {
            assertFalse(detached.await(50, TimeUnit.MILLISECONDS))
        } finally { finishSetter.countDown() }
        setter.join(5000); releasing.join(5000)
        assertFalse(setter.isAlive); assertFalse(releasing.isAlive)
        assertTrue(target.released)
        owner.setEnabled(false)
        assertNull(owner.current())
    }
}
