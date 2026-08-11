package com.decent.audio;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.content.res.AssetManager;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The two operations the route change was made for, and the two controls the
 * pipeline drives them with.
 *
 * <p>{@link FlowyFfmpegDeviceTest} can only exercise what this build's own
 * encoders can synthesise, which leaves the two most important inputs
 * unreachable: FLAC inside MP4 (nothing here muxes FLAC into MP4 from scratch)
 * and E-AC-3 (this build has the decoder and no encoder). Both were previously
 * asserted by reading FFmpeg's sources — the same kind of claim the device
 * proof exists to replace — so both now ride as committed fixtures. See
 * {@code src/androidTest/assets/README.md} for how they were generated.
 *
 * <p>The cancellation and progress tests are here rather than beside the codec
 * assertions because they need a source long enough to still be running when
 * the cancel arrives, which the fixtures are not.
 */
@RunWith(AndroidJUnit4.class)
public class FlowyFfmpegFixtureTest {

    /** Long enough to hold several progress ticks and an interrupted encode. */
    private static final int LONG_SOURCE_SECONDS = 20;
    private static final int SAMPLE_RATE = 44100;
    private static final int CHANNELS = 2;

    private File dir;

    @Before
    public void setUp() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        dir = new File(context.getCacheDir(), "flowy-ffmpeg-fixtures");
        deleteRecursively(dir);
        assertTrue(dir.mkdirs());
    }

    // ── FLAC in MP4 → FLAC, by stream copy ──────────────────────────────

    /**
     * The operation the whole typed route leads with. It works only if the MP4
     * demuxer's {@code dfLa} extradata is what the FLAC muxer expects to write
     * as a STREAMINFO block — if it is not, the output is a file that muxes
     * without complaint and then decodes to nothing.
     *
     * <p>So the assertion is not "the muxer returned 0": the remuxed file is
     * fed back through a full decode, and the decoded length is checked against
     * the source's. A truncated or unreadable stream fails there.
     */
    @Test
    public void flacInMp4StreamCopiesIntoAFlacThatStillDecodes() throws IOException {
        File src = asset("flac_in_mp4.m4a");

        FlowyFfmpeg.AudioInfo srcInfo = FlowyFfmpeg.probe(src.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), srcInfo);
        assertEquals("flac", srcInfo.codecName);
        assertTrue("fixture is not in MP4: " + srcInfo.formatName,
                srcInfo.formatName.startsWith("mov,mp4"));
        assertEquals(44100, srcInfo.sampleRate);
        assertEquals(2, srcInfo.channels);

        File copied = new File(dir, "from-mp4.flac");
        int rc = FlowyFfmpeg.remux(src.getAbsolutePath(), copied.getAbsolutePath(), "flac",
                new String[]{"title", "Flowy paw copy :3"}, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        FlowyFfmpeg.AudioInfo copiedInfo = FlowyFfmpeg.probe(copied.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), copiedInfo);
        assertEquals("flac", copiedInfo.codecName);
        assertEquals("flac", copiedInfo.formatName);
        assertEquals(44100, copiedInfo.sampleRate);
        assertEquals(2, copiedInfo.channels);
        assertEquals("the STREAMINFO the mp4 carried did not survive the copy",
                16, copiedInfo.bitsPerRawSample);

        // The real test: every frame through the FLAC decoder. A re-encode is
        // the way to force that with this API — it decodes the whole stream and
        // would report a decoder error rather than a short file.
        File decoded = new File(dir, "from-mp4-reencoded.flac");
        int decodeRc = FlowyFfmpeg.encodeFlac(copied.getAbsolutePath(),
                decoded.getAbsolutePath(), 16, null, null);
        assertEquals("the remuxed FLAC does not decode: " + FlowyFfmpeg.lastError(),
                FlowyFfmpeg.OK, decodeRc);

        FlowyFfmpeg.AudioInfo decodedInfo = FlowyFfmpeg.probe(decoded.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), decodedInfo);
        // ~2 s in, ~2 s out. A stream that decoded to silence-after-the-first-
        // frame would land far short of this.
        assertTrue("decoded duration " + decodedInfo.durationMs + " ms is not the source's",
                Math.abs(decodedInfo.durationMs - 2000) < 400);
    }

    // ── E-AC-3 5.1 → 24-bit FLAC ────────────────────────────────────────

    /**
     * The Dolby branch. Six channels have to reach the FLAC encoder as six
     * channels — the layout the E-AC-3 decoder emits is not spelled the same
     * way as any entry in flacenc's list, and a channel-layout negotiation that
     * gives up would silently produce a stereo downmix instead.
     */
    @Test
    public void eac3FiveOneTranscodesToTwentyFourBitSixChannelFlac() throws IOException {
        File src = asset("eac3_51.mp4");

        FlowyFfmpeg.AudioInfo srcInfo = FlowyFfmpeg.probe(src.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), srcInfo);
        assertEquals("eac3", srcInfo.codecName);
        assertEquals(6, srcInfo.channels);
        assertEquals(48000, srcInfo.sampleRate);

        File out = new File(dir, "eac3-to.flac");
        int rc = FlowyFfmpeg.encodeFlac(src.getAbsolutePath(), out.getAbsolutePath(), 24,
                new String[]{"title", "Flowy 5.1 OwO"}, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(out.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("flac", info.codecName);
        assertEquals("5.1 was downmixed instead of preserved", 6, info.channels);
        assertEquals(24, info.bitsPerRawSample);
        assertEquals(48000, info.sampleRate);
        assertTrue("output duration " + info.durationMs + " ms is not the source's",
                Math.abs(info.durationMs - 2000) < 400);
    }

    /**
     * The same source into MP4 by stream copy — no decode, no re-encode.
     *
     * <p>The muxer name matters and is not interchangeable: {@code "ipod"}, the
     * usual choice for a {@code .m4a}, carries a short tag table
     * ({@code codec_ipod_tags} in movenc.c) that has AC-3 and no E-AC-3, so it
     * refuses this stream outright. {@code "mp4"} takes it. The negative half
     * of this test is asserted deliberately: a caller that reaches for "ipod"
     * because the destination ends in .m4a gets a failed conversion for a file
     * that is perfectly copyable, and the file extension is ours to choose
     * independently of the muxer.
     */
    @Test
    public void eac3StreamCopiesIntoMp4ButNotIntoIpod() throws IOException {
        File src = asset("eac3_51.mp4");

        File refused = new File(dir, "eac3-ipod.m4a");
        assertFalse("the ipod muxer now accepts E-AC-3 — the mp4-only note can go",
                FlowyFfmpeg.OK == FlowyFfmpeg.remux(src.getAbsolutePath(),
                        refused.getAbsolutePath(), "ipod", null, null));
        assertFalse("a refused muxing left a file behind", refused.exists());

        File out = new File(dir, "eac3-copy.m4a");
        int rc = FlowyFfmpeg.remux(src.getAbsolutePath(), out.getAbsolutePath(), "mp4",
                new String[]{"title", "Flowy Atmos"}, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(out.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("eac3", info.codecName);
        assertEquals(6, info.channels);
        assertEquals(48000, info.sampleRate);
    }

    // ── Cancellation ────────────────────────────────────────────────────

    @Test
    public void aJobCancelledBeforeTheOperationStartsOpensNothing() throws IOException {
        File src = asset("eac3_51.mp4");
        File out = new File(dir, "never.flac");
        try (FlowyFfmpeg.Job job = new FlowyFfmpeg.Job()) {
            job.cancel();
            int rc = FlowyFfmpeg.encodeFlac(src.getAbsolutePath(), out.getAbsolutePath(), 24,
                    null, null, job, null);
            assertEquals(FlowyFfmpeg.CANCELLED, rc);
        }
        assertFalse("a cancelled job left a file behind", out.exists());
    }

    /**
     * The case that actually matters: the cancel arrives from a different
     * thread while the encode is running.
     *
     * <p>Nothing here is a race. The progress callback — which runs on the
     * encoding thread, inside the operation — parks on the first tick until
     * this thread has cancelled, so the operation is provably still in flight
     * when {@link FlowyFfmpeg.Job#cancel()} is called, on a fast device as much
     * as on a slow one.
     */
    @Test
    public void aRunningTranscodeStopsWhenAnotherThreadCancels() throws Exception {
        File src = new File(dir, "long.wav");
        writeToneWav(src, LONG_SOURCE_SECONDS * 1000);
        File out = new File(dir, "cancelled.flac");

        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch cancelIssued = new CountDownLatch(1);
        AtomicInteger result = new AtomicInteger(Integer.MIN_VALUE);

        try (FlowyFfmpeg.Job job = new FlowyFfmpeg.Job()) {
            Thread worker = new Thread(() -> result.set(FlowyFfmpeg.encodeFlac(
                    src.getAbsolutePath(), out.getAbsolutePath(), 24, null, null,
                    job, (positionMs, durationMs) -> {
                        started.countDown();
                        try {
                            cancelIssued.await(30, TimeUnit.SECONDS);
                        } catch (InterruptedException ignored) {
                            Thread.currentThread().interrupt();
                        }
                    })));
            worker.start();

            assertTrue("the encode never reported any progress",
                    started.await(30, TimeUnit.SECONDS));
            job.cancel();
            cancelIssued.countDown();
            worker.join(TimeUnit.SECONDS.toMillis(30));
            assertFalse("the encode did not stop after cancel()", worker.isAlive());
        }

        assertEquals("a cancelled encode must report CANCELLED",
                FlowyFfmpeg.CANCELLED, result.get());
        assertFalse("a cancelled job left a partial file behind", out.exists());
    }

    /** Closing the handle while its own operation runs must not crash it. */
    @Test
    public void closingAJobWhileItsOperationRunsIsHarmless() throws Exception {
        File src = new File(dir, "closing.wav");
        writeToneWav(src, 5000);
        File out = new File(dir, "closing.flac");

        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch closed = new CountDownLatch(1);
        AtomicInteger result = new AtomicInteger(Integer.MIN_VALUE);
        FlowyFfmpeg.Job job = new FlowyFfmpeg.Job();
        Thread worker = new Thread(() -> result.set(FlowyFfmpeg.encodeFlac(
                src.getAbsolutePath(), out.getAbsolutePath(), 24, null, null,
                job, (positionMs, durationMs) -> {
                    started.countDown();
                    try {
                        closed.await(30, TimeUnit.SECONDS);
                    } catch (InterruptedException ignored) {
                        Thread.currentThread().interrupt();
                    }
                })));
        worker.start();
        assertTrue(started.await(30, TimeUnit.SECONDS));
        job.close();
        closed.countDown();
        worker.join(TimeUnit.SECONDS.toMillis(60));
        assertFalse(worker.isAlive());
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, result.get());
    }

    // ── Progress ────────────────────────────────────────────────────────

    @Test
    public void progressIsReportedAgainstTheSourceTimeline() throws IOException {
        File src = new File(dir, "progress.wav");
        writeToneWav(src, 10_000);
        File out = new File(dir, "progress.flac");

        List<long[]> ticks = new ArrayList<>();
        int rc = FlowyFfmpeg.encodeFlac(src.getAbsolutePath(), out.getAbsolutePath(), 16,
                null, null, null,
                (positionMs, durationMs) -> ticks.add(new long[]{positionMs, durationMs}));
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        assertFalse("no progress at all for a 10 s source", ticks.isEmpty());
        long previous = -1;
        for (long[] tick : ticks) {
            assertTrue("progress went backwards: " + tick[0] + " after " + previous,
                    tick[0] >= previous);
            previous = tick[0];
            assertEquals("the source's duration was not reported", 10_000, tick[1], 200);
        }
        // Coalesced to 250 ms, so a 10 s source is tens of callbacks, not
        // thousands of packets.
        assertTrue("suspiciously many callbacks: " + ticks.size(), ticks.size() < 200);
        assertEquals("the last tick must be the end of the file",
                10_000, previous, 200);
    }

    /** A listener that throws must abort the operation, not be swallowed. */
    @Test
    public void aThrowingListenerStopsTheOperation() throws IOException {
        File src = new File(dir, "throwing.wav");
        writeToneWav(src, 10_000);
        File out = new File(dir, "throwing.flac");
        try {
            FlowyFfmpeg.encodeFlac(src.getAbsolutePath(), out.getAbsolutePath(), 24,
                    null, null, null, (positionMs, durationMs) -> {
                        throw new IllegalStateException("nope");
                    });
            // The exception surfaces at the Java call site; reaching here means
            // it was swallowed somewhere, which would leave the operation
            // running with a pending JNI exception.
            throw new AssertionError("the listener's exception did not propagate");
        } catch (IllegalStateException expected) {
            // expected
        }
        assertFalse("an aborted operation left a partial file behind", out.exists());
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    private File asset(String name) throws IOException {
        AssetManager assets =
                InstrumentationRegistry.getInstrumentation().getContext().getAssets();
        File file = new File(dir, name);
        try (InputStream in = assets.open(name);
             FileOutputStream out = new FileOutputStream(file)) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        assertTrue(name + " did not land in the test APK", file.length() > 0);
        return file;
    }

    /** Same canonical 44-byte-header WAV as the other test, at any length. */
    private static void writeToneWav(File file, int durationMs) throws IOException {
        // 64-bit intermediate: a minute at 44.1 kHz overflows an int here.
        int frames = (int) ((long) SAMPLE_RATE * durationMs / 1000);
        int dataBytes = frames * CHANNELS * 2;
        ByteBuffer buf = ByteBuffer.allocate(44 + dataBytes).order(ByteOrder.LITTLE_ENDIAN);
        buf.put("RIFF".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(36 + dataBytes);
        buf.put("WAVE".getBytes(StandardCharsets.US_ASCII));
        buf.put("fmt ".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(16);
        buf.putShort((short) 1);
        buf.putShort((short) CHANNELS);
        buf.putInt(SAMPLE_RATE);
        buf.putInt(SAMPLE_RATE * CHANNELS * 2);
        buf.putShort((short) (CHANNELS * 2));
        buf.putShort((short) 16);
        buf.put("data".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(dataBytes);
        for (int i = 0; i < frames; i++) {
            double t = (double) i / SAMPLE_RATE;
            buf.putShort((short) (Math.sin(2 * Math.PI * 440 * t) * 12000));
            buf.putShort((short) (Math.sin(2 * Math.PI * 660 * t) * 12000));
        }
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(buf.array());
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteRecursively(child);
        }
        file.delete();
    }
}
