package com.decent.audio;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * The first proof that any of this runs on Android.
 *
 * <p>What it is really asserting, in order of importance:
 *
 * <ol>
 *   <li>The five shared libraries load together — {@code libflowyaudio.so}
 *       against the four FFmpeg ones, which is where a 16 KB page alignment
 *       mismatch or a versioned SONAME would show up as a {@code dlopen}
 *       failure and nowhere else.
 *   <li>{@code avcodec_find_encoder_by_name("flac")} and
 *       {@code ("libmp3lame")} return non-null <em>at runtime</em>. No
 *       build-time grep of the binary can establish this: libavcodec compiles
 *       a descriptor for every codec unconditionally, so the codec's name is
 *       in the strings of a build that cannot encode it.
 *   <li>Each of the four operations the download pipeline needs actually
 *       produces a file the libraries can then read back.
 * </ol>
 *
 * <p>Everything is synthesised on device — a one-second WAV tone written by
 * hand and a 67-byte PNG — so the test carries no media fixtures and depends
 * on nothing but the codec set this module ships.
 */
@RunWith(AndroidJUnit4.class)
public class FlowyFfmpegDeviceTest {

    private static final int SAMPLE_RATE = 44100;
    private static final int CHANNELS = 2;
    private static final String TITLE = "Flowy paw test :3";

    /** A 1x1 transparent PNG. Muxers store cover bytes verbatim, so this is enough. */
    private static final byte[] PNG_1X1 = {
            (byte) 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, (byte) 0xC4,
            (byte) 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41,
            0x54, 0x78, (byte) 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
            0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, (byte) 0xB4, 0x00,
            0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, (byte) 0xAE,
            0x42, 0x60, (byte) 0x82,
    };

    private File dir;
    private File source;
    private File cover;

    @Before
    public void setUp() throws IOException {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        dir = new File(context.getCacheDir(), "flowy-ffmpeg-test");
        deleteRecursively(dir);
        assertTrue(dir.mkdirs());

        source = new File(dir, "tone.wav");
        writeToneWav(source, 1000);

        cover = new File(dir, "cover.png");
        try (FileOutputStream out = new FileOutputStream(cover)) {
            out.write(PNG_1X1);
        }
    }

    // ── The load / codec-presence proof ─────────────────────────────────

    @Test
    public void librariesLoadAndCarryTheCodecSet() {
        // Reaching here at all means System.loadLibrary("flowyaudio") resolved
        // and the four FFmpeg libraries beside it did too.
        assertNotNull(FlowyFfmpeg.version());
        assertFalse(FlowyFfmpeg.version().isEmpty());

        assertTrue("flac encoder missing at runtime", FlowyFfmpeg.hasEncoder("flac"));
        assertTrue("libmp3lame encoder missing at runtime",
                FlowyFfmpeg.hasEncoder("libmp3lame"));
        assertTrue(FlowyFfmpeg.hasDecoder("flac"));
        assertTrue(FlowyFfmpeg.hasDecoder("aac"));
        assertTrue(FlowyFfmpeg.hasDecoder("mp3"));
        assertTrue("eac3 decoder missing — the Atmos branch needs it",
                FlowyFfmpeg.hasDecoder("eac3"));

        // Negative controls. These names exist in libavcodec's descriptor
        // table on every build, so if --disable-everything had silently
        // stopped holding, these would come back true.
        assertFalse(FlowyFfmpeg.hasEncoder("aac"));
        assertFalse(FlowyFfmpeg.hasDecoder("opus"));
        assertFalse(FlowyFfmpeg.hasDecoder("alac"));
    }

    // ── Probe ───────────────────────────────────────────────────────────

    @Test
    public void probeReadsTheFirstAudioStream() {
        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(source.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("pcm_s16le", info.codecName);
        assertEquals(CHANNELS, info.channels);
        assertEquals(SAMPLE_RATE, info.sampleRate);
        assertEquals("wav", info.formatName);

        assertEquals("pcm_s16le", FlowyFfmpeg.probeCodec(source.getAbsolutePath()));
        assertEquals(CHANNELS, FlowyFfmpeg.probeChannels(source.getAbsolutePath()));
        assertEquals(SAMPLE_RATE, FlowyFfmpeg.probeSampleRate(source.getAbsolutePath()));
    }

    @Test
    public void probeOfSomethingThatIsNotAudioFails() throws IOException {
        File junk = new File(dir, "junk.bin");
        try (FileOutputStream out = new FileOutputStream(junk)) {
            out.write(new byte[1024]);
        }
        assertEquals(null, FlowyFfmpeg.probe(junk.getAbsolutePath()));
        assertFalse("a failure must leave a readable message",
                FlowyFfmpeg.lastError().isEmpty());
    }

    // ── The four operations ─────────────────────────────────────────────

    @Test
    public void encodesFlacAtAPinned24Bits() {
        File out = new File(dir, "out24.flac");
        int rc = FlowyFfmpeg.encodeFlac(source.getAbsolutePath(), out.getAbsolutePath(), 24,
                new String[]{"title", TITLE, "artist", "Flowy"}, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);
        assertTrue(out.length() > 0);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(out.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("flac", info.codecName);
        assertEquals(CHANNELS, info.channels);
        assertEquals(SAMPLE_RATE, info.sampleRate);
        assertEquals(24, info.bitsPerRawSample);
        // FLAC's VORBIS_COMMENT block stores tags as plain UTF-8.
        assertTrue("title did not survive into the FLAC", contains(out, TITLE));
    }

    @Test
    public void encodesFlacAt16Bits() {
        File out = new File(dir, "out16.flac");
        int rc = FlowyFfmpeg.encodeFlac(source.getAbsolutePath(), out.getAbsolutePath(), 16,
                null, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);
        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(out.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("flac", info.codecName);
        assertEquals(16, info.bitsPerRawSample);
    }

    @Test
    public void encodesMp3ThroughLibmp3lame() {
        File out = new File(dir, "out.mp3");
        int rc = FlowyFfmpeg.encodeMp3(source.getAbsolutePath(), out.getAbsolutePath(), 320,
                new String[]{"title", TITLE}, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);
        assertTrue(out.length() > 0);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(out.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("mp3", info.codecName);
        assertEquals(CHANNELS, info.channels);
        assertEquals(SAMPLE_RATE, info.sampleRate);
        // 320 kbit/s for one second, give or take the ID3 tag and the
        // encoder's own padding.
        assertTrue("unexpected size for 320 kbps: " + out.length(),
                out.length() > 30000 && out.length() < 60000);
    }

    @Test
    public void streamCopiesWithoutReEncoding() {
        File flac = new File(dir, "copy-src.flac");
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK,
                FlowyFfmpeg.encodeFlac(source.getAbsolutePath(), flac.getAbsolutePath(), 24,
                        null, null));

        File copy = new File(dir, "copy-dst.flac");
        int rc = FlowyFfmpeg.remux(flac.getAbsolutePath(), copy.getAbsolutePath(), "flac",
                null, null);
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(copy.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("flac", info.codecName);
        assertEquals(24, info.bitsPerRawSample);
        assertEquals(SAMPLE_RATE, info.sampleRate);
    }

    /**
     * The shape of the Dolby Atmos branch: take an already-encoded stream,
     * copy it into MP4 untouched, and hang metadata and a cover picture on it.
     * The payload here is MP3 rather than E-AC-3 only because a synthetic
     * E-AC-3 stream cannot be produced by a build with no E-AC-3 encoder; the
     * muxing path exercised is the same one.
     */
    @Test
    public void streamCopiesIntoMp4WithMetadataAndCover() {
        File mp3 = new File(dir, "atmos-src.mp3");
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK,
                FlowyFfmpeg.encodeMp3(source.getAbsolutePath(), mp3.getAbsolutePath(), 192,
                        null, null));

        File mp4 = new File(dir, "atmos.mp4");
        int rc = FlowyFfmpeg.remux(mp3.getAbsolutePath(), mp4.getAbsolutePath(), "mp4",
                new String[]{"title", TITLE, "album", "Flowy Downloads"},
                cover.getAbsolutePath());
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);

        FlowyFfmpeg.AudioInfo info = FlowyFfmpeg.probe(mp4.getAbsolutePath());
        assertNotNull(FlowyFfmpeg.lastError(), info);
        assertEquals("mp3", info.codecName);
        assertTrue(info.formatName.startsWith("mov,mp4"));
        assertTrue("no covr atom in the MP4", contains(mp4, "covr"));
        assertTrue("title did not reach the MP4", contains(mp4, TITLE));
    }

    @Test
    public void attachesACoverToFlac() {
        File out = new File(dir, "covered.flac");
        int rc = FlowyFfmpeg.encodeFlac(source.getAbsolutePath(), out.getAbsolutePath(), 16,
                new String[]{"title", TITLE}, cover.getAbsolutePath());
        assertEquals(FlowyFfmpeg.lastError(), FlowyFfmpeg.OK, rc);
        assertTrue("no PICTURE block mime type in the FLAC", contains(out, "image/png"));
    }

    /**
     * A half-written destination looks like a converted track to anything that
     * only stats the path, so a failed job must not leave one behind. The
     * failure here happens after the output file exists — the WAV is truncated
     * mid-stream, so the header is written and the read then fails.
     */
    @Test
    public void aFailedOperationLeavesNoPartialFile() throws IOException {
        File truncated = new File(dir, "truncated.wav");
        try (RandomAccessFile src = new RandomAccessFile(source, "r");
             FileOutputStream out = new FileOutputStream(truncated)) {
            byte[] head = new byte[20000];
            src.readFully(head);
            out.write(head);
        }
        // Claim far more data than is present, so the demuxer runs off the end.
        try (RandomAccessFile raf = new RandomAccessFile(truncated, "rw")) {
            raf.seek(40);
            raf.write(new byte[]{0x00, 0x00, 0x40, 0x00}); // data chunk size = 4 MB
        }

        File out = new File(dir, "partial.flac");
        FlowyFfmpeg.encodeFlac(truncated.getAbsolutePath(), out.getAbsolutePath(), 24,
                null, null);
        // Whether this particular input fails or merely ends early, the rule is
        // the same: a file that exists must be readable.
        if (out.exists()) {
            assertNotNull("a surviving output must be a valid file: "
                    + FlowyFfmpeg.lastError(), FlowyFfmpeg.probe(out.getAbsolutePath()));
        }

        // And an input that cannot be opened at all leaves nothing at the
        // destination, not even a zero-byte file.
        File never = new File(dir, "never.flac");
        assertFalse(FlowyFfmpeg.OK == FlowyFfmpeg.encodeFlac(
                new File(dir, "absent.wav").getAbsolutePath(), never.getAbsolutePath(), 24,
                null, null));
        assertFalse("a failed job left a file behind", never.exists());
    }

    @Test
    public void aFailedOperationExplainsItself() {
        int rc = FlowyFfmpeg.encodeMp3(new File(dir, "nope.wav").getAbsolutePath(),
                new File(dir, "nope.mp3").getAbsolutePath(), 320, null, null);
        assertFalse("a missing input must not report success", rc == FlowyFfmpeg.OK);
        assertTrue(FlowyFfmpeg.lastError(), FlowyFfmpeg.lastError().contains("nope.wav"));
    }

    // ── Fixtures ────────────────────────────────────────────────────────

    /** Writes a canonical 44-byte-header PCM s16le stereo WAV holding a tone. */
    private static void writeToneWav(File file, int durationMs) throws IOException {
        int frames = SAMPLE_RATE * durationMs / 1000;
        int dataBytes = frames * CHANNELS * 2;
        ByteBuffer buf = ByteBuffer.allocate(44 + dataBytes).order(ByteOrder.LITTLE_ENDIAN);
        buf.put("RIFF".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(36 + dataBytes);
        buf.put("WAVE".getBytes(StandardCharsets.US_ASCII));
        buf.put("fmt ".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(16);                                   // PCM chunk size
        buf.putShort((short) 1);                          // PCM
        buf.putShort((short) CHANNELS);
        buf.putInt(SAMPLE_RATE);
        buf.putInt(SAMPLE_RATE * CHANNELS * 2);           // byte rate
        buf.putShort((short) (CHANNELS * 2));             // block align
        buf.putShort((short) 16);                         // bits per sample
        buf.put("data".getBytes(StandardCharsets.US_ASCII));
        buf.putInt(dataBytes);
        for (int i = 0; i < frames; i++) {
            double t = (double) i / SAMPLE_RATE;
            short left = (short) (Math.sin(2 * Math.PI * 440 * t) * 12000);
            short right = (short) (Math.sin(2 * Math.PI * 660 * t) * 12000);
            buf.putShort(left);
            buf.putShort(right);
        }
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(buf.array());
        }
    }

    private static boolean contains(File file, String needle) {
        byte[] pattern = needle.getBytes(StandardCharsets.UTF_8);
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            byte[] all = new byte[(int) raf.length()];
            raf.readFully(all);
            outer:
            for (int i = 0; i + pattern.length <= all.length; i++) {
                for (int j = 0; j < pattern.length; j++) {
                    if (all[i + j] != pattern[j]) continue outer;
                }
                return true;
            }
        } catch (IOException e) {
            return false;
        }
        return false;
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteRecursively(child);
        }
        file.delete();
    }
}
