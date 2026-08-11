package com.decent.audio;

/**
 * The download pipeline's on-device audio converter.
 *
 * <p>This is a <em>typed</em> surface over libavformat / libavcodec /
 * libswresample, not an {@code ffmpeg} command line. The CLI is not available:
 * the libraries this module ships are configured {@code --disable-programs
 * --disable-avfilter}, and ffmpeg's {@code fftools} entry point cannot be
 * linked without libavfilter. Every operation the pipeline needs — stream copy,
 * MP3 encode, FLAC encode, and metadata/cover attachment — is expressible
 * without a filter graph, so nothing is lost by not having one. Sample format,
 * sample rate and channel layout conversion are done by libswresample.
 *
 * <p>All methods are safe to call from any thread, but {@link #lastError()} is
 * <em>thread-local</em>: it describes the last failure on the calling thread.
 * Call it on the same thread that ran the operation.
 *
 * <p>Paths are filesystem paths, not content URIs — the only protocol built
 * into these libraries is {@code file}.
 */
public final class FlowyFfmpeg {

    private FlowyFfmpeg() {
    }

    static {
        System.loadLibrary("flowyaudio");
    }

    /** Every operation returns this on success. Anything else is a failure. */
    public static final int OK = 0;

    // ── Probe ────────────────────────────────────────────────────────────

    /** What {@link #probe} found about a file's first audio stream. */
    public static final class AudioInfo {
        /** ffmpeg's own codec name: {@code "flac"}, {@code "aac"}, {@code "mp3"}, {@code "eac3"}, {@code "pcm_s16le"}, … */
        public final String codecName;
        /** Channel count of the audio stream. */
        public final int channels;
        /** Sample rate in Hz. */
        public final int sampleRate;
        /** Bits per raw sample as declared by the stream, or {@code 0} when the codec does not say. */
        public final int bitsPerRawSample;
        /** Duration in milliseconds, or {@code 0} when the container does not say. */
        public final long durationMs;
        /** The demuxer's name, e.g. {@code "mov,mp4,m4a,3gp,3g2,mj2"}, {@code "flac"}, {@code "wav"}. */
        public final String formatName;
        /** Stream bitrate in bits per second, or {@code 0} when unknown. */
        public final long bitRate;

        AudioInfo(String codecName, int channels, int sampleRate, int bitsPerRawSample,
                  long durationMs, String formatName, long bitRate) {
            this.codecName = codecName;
            this.channels = channels;
            this.sampleRate = sampleRate;
            this.bitsPerRawSample = bitsPerRawSample;
            this.durationMs = durationMs;
            this.formatName = formatName;
            this.bitRate = bitRate;
        }

        @Override
        public String toString() {
            return "AudioInfo{" + codecName + " " + channels + "ch " + sampleRate + "Hz "
                    + bitsPerRawSample + "bit " + durationMs + "ms in " + formatName + "}";
        }
    }

    /**
     * Opens {@code path} and describes its first audio stream.
     *
     * @return the description, or {@code null} if the file cannot be opened or
     *         holds no audio stream — {@link #lastError()} says which.
     */
    public static native AudioInfo probe(String path);

    /** @return ffmpeg's codec name of the first audio stream, or {@code null}. */
    public static String probeCodec(String path) {
        AudioInfo info = probe(path);
        return info == null ? null : info.codecName;
    }

    /** @return the first audio stream's channel count, or {@code 0} on failure. */
    public static int probeChannels(String path) {
        AudioInfo info = probe(path);
        return info == null ? 0 : info.channels;
    }

    /** @return the first audio stream's sample rate in Hz, or {@code 0} on failure. */
    public static int probeSampleRate(String path) {
        AudioInfo info = probe(path);
        return info == null ? 0 : info.sampleRate;
    }

    // ── Operations ───────────────────────────────────────────────────────
    //
    // metadata is a flat [key, value, key, value, …] array and may be null.
    // Keys are ffmpeg metadata keys ("title", "artist", "album",
    // "album_artist", "track", "date", …); the muxer maps them to whatever
    // the container spells them as. Source metadata is copied first, so a
    // key that is not overridden survives the operation.
    //
    // coverPath may be null. When set it must point at a JPEG or PNG file;
    // it is attached as a cover picture (MP4 `covr`, FLAC PICTURE block,
    // MP3 ID3v2 APIC). No image codec is involved — the bytes are stored
    // verbatim, which is why no image encoder needs to be built in.

    /**
     * Stream copy: demux the first audio stream of {@code srcPath} and mux it,
     * bit-identical, into {@code dstPath}. No decode, no re-encode.
     *
     * @param muxer ffmpeg muxer name — {@code "flac"}, {@code "mp4"},
     *              {@code "ipod"} (M4A), {@code "mp3"}, {@code "wav"} — or
     *              {@code null} to let ffmpeg guess from the destination's
     *              extension.
     * @return {@link #OK}, or non-zero on failure.
     */
    public static native int remux(String srcPath, String dstPath, String muxer,
                                   String[] metadata, String coverPath);

    /**
     * Decode {@code srcPath} and re-encode it as MP3 with libmp3lame at a
     * constant bitrate. More than two channels are downmixed to stereo and a
     * sample rate libmp3lame cannot take is resampled, both by libswresample.
     *
     * @param bitrateKbps e.g. {@code 320}.
     * @return {@link #OK}, or non-zero on failure.
     */
    public static native int encodeMp3(String srcPath, String dstPath, int bitrateKbps,
                                       String[] metadata, String coverPath);

    /**
     * Decode {@code srcPath} and re-encode it as FLAC at a pinned bit depth.
     *
     * @param bitsPerSample {@code 16} or {@code 24}. 24 encodes through
     *                      {@code AV_SAMPLE_FMT_S32} with
     *                      {@code bits_per_raw_sample = 24}, which is what the
     *                      FLAC encoder wants for a 24-bit stream.
     * @return {@link #OK}, or non-zero on failure.
     */
    public static native int encodeFlac(String srcPath, String dstPath, int bitsPerSample,
                                        String[] metadata, String coverPath);

    // ── Introspection and errors ─────────────────────────────────────────

    /**
     * Whether an encoder is present in the libraries actually loaded on this
     * device. This is the only honest check that the codec set survived the
     * build: a build-time grep of the binary cannot distinguish a compiled
     * encoder from ffmpeg's unconditional descriptor table.
     */
    public static native boolean hasEncoder(String name);

    /** Same question for a decoder. */
    public static native boolean hasDecoder(String name);

    /** libavcodec's version string, e.g. {@code "62.11.100"}. */
    public static native String version();

    /**
     * A human-readable description of the last failure on the calling thread —
     * our own message plus the last error libav* logged, when there was one.
     * Never {@code null}; empty when nothing has failed on this thread.
     */
    public static native String lastError();
}
