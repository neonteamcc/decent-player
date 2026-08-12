/*
 * arm32 (armeabi-v7a) build smoke test — run in CI under qemu-arm.
 * Driven by arm32-smoke.sh; see that script for how it is built and run.
 *
 * WHY THIS EXISTS
 * ---------------
 * armeabi-v7a is the one shipped ABI no device available here can execute, and
 * Apple silicon cannot run AArch32 at all. It was built four times over and
 * never once *run*. This harness runs it: it links against the armeabi-v7a
 * libraries this module just produced and exercises them under emulation, on
 * the same runner that built them.
 *
 * WHAT IT PROVES
 * --------------
 * The BUILD is sound for this ABI — the code executes, the assembly the ARM
 * ports select at runtime does not corrupt anything, every symbol resolves,
 * and the codec set really is present at run time. That last one is the check
 * the README calls unfakeable: avcodec_find_*_by_name() returning non-NULL is
 * evidence a codec exists, where a grep for its long_name is not (every
 * descriptor is compiled unconditionally — see build-ffmpeg.sh).
 *
 * WHAT IT DOES NOT PROVE
 * ----------------------
 * Nothing about Android's dynamic loader: page alignment, SONAMEs and
 * DT_NEEDED resolution inside an app's lib dir are not exercised here, because
 * a static link is the only way to run a bionic binary under qemu-user (there
 * is no bionic linker to give it). Nor is the JNI wiring exercised, nor any
 * defect a shared link alone can produce. All three are largely
 * ABI-independent for this module — build-ffmpeg.sh asserts the alignment and
 * SONAME properties on the shipped .so files for all four ABIs, and
 * flowy_audio_jni.cc is one source file compiled per ABI — so the residual
 * risk is small but real. Closing it means running src/androidTest/ on a
 * physical arm32 device (Firebase Test Lab), which is a separate decision.
 *
 * The archives linked here come from the same objects as the shipped .so
 * files: one `make` compiles each object once (PIC) and both --enable-shared
 * and --enable-static are served from it. So the code under test is the code
 * that ships; only the link step differs — which is exactly the residual
 * named above, not a claim that nothing differs.
 *
 * Failure style follows build-ffmpeg.sh: every check reports, indented, on
 * stderr, and the run ends with one ERROR line and a non-zero exit — so a
 * broken build shows all of what broke, not just the first thing.
 */

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/cpu.h>
#include <libavutil/crc.h>
#include <libavutil/macros.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#define MAX_CH 8

/* The CRC of flac_in_mp4.m4a's decoded PCM.
 *
 * This is a property of the FILE, not of our build: FLAC decoding is exact
 * integer arithmetic, so every correct decoder produces these bytes. The value
 * was measured by two independent implementations — this build under qemu-arm,
 * and a host FFmpeg 8.1.2 on arm64 — which is what makes it safe to pin rather
 * than merely round-trip.
 *
 * The round trip further down does NOT subsume it, and that is not a
 * hypothetical: flipping a single byte of the fixture changes this CRC while
 * leaving the round trip perfectly bit-exact, because encoding wrong samples
 * and decoding them back returns the same wrong samples. The round trip proves
 * the encoder and the decoder agree with each other; only a pinned value
 * proves they agree with the format. */
#define FLAC_FIXTURE_CRC 0xb934fd38u

static int failed = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    failed = 1;
}

/* ------------------------------------------------------------------------ */
/* The codec set, at run time                                               */
/*                                                                          */
/* Mirrors REQUIRED_COMPONENTS / FORBIDDEN_COMPONENTS in build-ffmpeg.sh,    */
/* all 25 of them — FILE_PROTOCOL is the one that is not a codec, so it is   */
/* looked up through avio rather than avcodec (see check_codec_set).         */
/* That script asserts them on config_components.h, i.e. on what configure   */
/* said it would build; this asserts the same list on what the linked        */
/* library actually offers on this ABI.                                     */
/* ------------------------------------------------------------------------ */

static const char *const REQUIRED_DECODERS[] = {
    "flac", "aac", "mp3", "eac3", "pcm_s16le", "pcm_s24le", "pcm_f32le", NULL
};
static const char *const REQUIRED_ENCODERS[] = {
    "flac", "libmp3lame", "pcm_s16le", "pcm_s24le", NULL
};
/* av_find_input_format matches comma-separated demuxer names, so "mov"
 * matches the mov demuxer's real name "mov,mp4,m4a,3gp,3g2,mj2". */
static const char *const REQUIRED_DEMUXERS[] = {
    "mov", "flac", "mp3", "aac", "wav", NULL
};
static const char *const REQUIRED_MUXERS[] = {
    "flac", "mp4", "ipod", "mp3", "wav", NULL
};
static const enum AVCodecID REQUIRED_PARSERS[] = {
    AV_CODEC_ID_FLAC, AV_CODEC_ID_AAC, AV_CODEC_ID_MP3, AV_CODEC_ID_NONE
};

static void check_codec_set(void)
{
    int i, n = 0, was = failed;

    for (i = 0; REQUIRED_DECODERS[i]; i++, n++)
        if (!avcodec_find_decoder_by_name(REQUIRED_DECODERS[i]))
            fail("MISSING  decoder %s", REQUIRED_DECODERS[i]);
    for (i = 0; REQUIRED_ENCODERS[i]; i++, n++)
        if (!avcodec_find_encoder_by_name(REQUIRED_ENCODERS[i]))
            fail("MISSING  encoder %s", REQUIRED_ENCODERS[i]);
    for (i = 0; REQUIRED_DEMUXERS[i]; i++, n++)
        if (!av_find_input_format(REQUIRED_DEMUXERS[i]))
            fail("MISSING  demuxer %s", REQUIRED_DEMUXERS[i]);
    for (i = 0; REQUIRED_MUXERS[i]; i++, n++)
        if (!av_guess_format(REQUIRED_MUXERS[i], NULL, NULL))
            fail("MISSING  muxer %s", REQUIRED_MUXERS[i]);
    for (i = 0; REQUIRED_PARSERS[i] != AV_CODEC_ID_NONE; i++, n++) {
        AVCodecParserContext *p = av_parser_init(REQUIRED_PARSERS[i]);
        if (!p)
            fail("MISSING  parser for codec id %d", (int)REQUIRED_PARSERS[i]);
        else
            av_parser_close(p);
    }

    /* CONFIG_FILE_PROTOCOL. Every fixture below opens a bare path and so
     * exercises it implicitly, but implicitly is not by name: avio's lookup
     * searches the protocols that were actually compiled in and returns NULL
     * when file is not among them, which is the same kind of evidence as the
     * avcodec_find_*_by_name calls above. Without it this list would mirror 24
     * of build-ffmpeg.sh's 25 components while claiming to mirror all of them. */
    n++;
    if (!avio_find_protocol_name("file:"))
        fail("MISSING  protocol file");

    /* Negative controls, verbatim from build-ffmpeg.sh's FORBIDDEN_COMPONENTS.
     * These are the codecs whose descriptor long_names are in the binary
     * whatever configure did, so their absence *here* is what proves
     * --disable-everything held on this ABI too. */
    if (avcodec_find_decoder_by_name("h264"))
        fail("UNEXPECTEDLY ENABLED  decoder h264");
    if (avcodec_find_decoder_by_name("alac"))
        fail("UNEXPECTEDLY ENABLED  decoder alac");
    if (avcodec_find_decoder_by_name("opus"))
        fail("UNEXPECTEDLY ENABLED  decoder opus");
    if (avcodec_find_encoder_by_name("vorbis"))
        fail("UNEXPECTEDLY ENABLED  encoder vorbis");

    if (failed == was)
        printf("codec set OK (armeabi-v7a, at run time): %d components, "
               "4 negative controls absent\n", n);
}

/* ------------------------------------------------------------------------ */
/* Decoding                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    char     codec[32];        /* the decoder's name, for the log line */
    enum AVCodecID id;         /* what is asserted — see expect_codec() */
    char     format[64];
    int      rate;
    int      channels;
    enum AVSampleFormat fmt;
    int64_t  samples;          /* per channel */
    double   rms[MAX_CH];
    int16_t *pcm;              /* interleaved s16, only when the source is s16 */
    size_t   pcm_samples;      /* frames captured in pcm */
    uint32_t crc;              /* av_crc over pcm, 0 when pcm is NULL */
} Decoded;

static double sample_at(const AVFrame *f, int ch, int i)
{
    int planar = av_sample_fmt_is_planar((enum AVSampleFormat)f->format);
    int nch = f->ch_layout.nb_channels;
    const uint8_t *p = planar ? f->extended_data[ch] : f->extended_data[0];
    int idx = planar ? i : i * nch + ch;

    switch (f->format) {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return ((const int16_t *)p)[idx] / 32768.0;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return ((const int32_t *)p)[idx] / 2147483648.0;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return ((const float *)p)[idx];
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP: return ((const double *)p)[idx];
    default:                 return 0.0;
    }
}

/* Decodes the whole file. capture_pcm keeps the samples for a bit-exact
 * comparison later, and is only honoured for an s16 source. */
static int decode_file(const char *path, int capture_pcm, Decoded *out)
{
    AVFormatContext *fc = NULL;
    const AVCodec *dec;
    AVCodecContext *cc = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frm = NULL;
    double acc[MAX_CH];
    size_t cap = 0;
    int stream, ret, ch, i;

    memset(out, 0, sizeof(*out));
    memset(acc, 0, sizeof(acc));

    if ((ret = avformat_open_input(&fc, path, NULL, NULL)) < 0) {
        fail("cannot open %s (%s)", path, av_err2str(ret));
        return -1;
    }
    if ((ret = avformat_find_stream_info(fc, NULL)) < 0) {
        fail("no stream info in %s (%s)", path, av_err2str(ret));
        goto done;
    }
    stream = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (stream < 0 || !dec) {
        fail("no decodable audio stream in %s", path);
        ret = -1;
        goto done;
    }
    snprintf(out->format, sizeof(out->format), "%s", fc->iformat->name);
    snprintf(out->codec, sizeof(out->codec), "%s", dec->name);
    out->id = dec->id;

    cc = avcodec_alloc_context3(dec);
    if (!cc) { fail("out of memory"); ret = -1; goto done; }
    avcodec_parameters_to_context(cc, fc->streams[stream]->codecpar);
    if ((ret = avcodec_open2(cc, dec, NULL)) < 0) {
        fail("cannot open decoder %s (%s)", dec->name, av_err2str(ret));
        goto done;
    }

    pkt = av_packet_alloc();
    frm = av_frame_alloc();
    if (!pkt || !frm) { fail("out of memory"); ret = -1; goto done; }

    /* Reads to EOF and then flushes the decoder, so the sample counts below
     * are the whole file and not "the whole file minus whatever the decoder
     * was still holding". */
    for (int eof = 0;;) {
        int have = 0;
        if (!eof) {
            if (av_read_frame(fc, pkt) >= 0) have = 1;
            else                             eof = 1;
        }
        if (have && pkt->stream_index != stream) {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(cc, have ? pkt : NULL);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR_EOF) {
            fail("send_packet failed on %s (%s)", path, av_err2str(ret));
            goto done;
        }

        while ((ret = avcodec_receive_frame(cc, frm)) >= 0) {
            int nch = frm->ch_layout.nb_channels;
            if (nch > MAX_CH) nch = MAX_CH;
            out->rate     = frm->sample_rate;
            out->channels = frm->ch_layout.nb_channels;
            out->fmt      = (enum AVSampleFormat)frm->format;
            for (ch = 0; ch < nch; ch++)
                for (i = 0; i < frm->nb_samples; i++) {
                    double v = sample_at(frm, ch, i);
                    acc[ch] += v * v;
                }
            if (capture_pcm && frm->format == AV_SAMPLE_FMT_S16) {
                size_t need = (out->pcm_samples + frm->nb_samples) *
                              frm->ch_layout.nb_channels;
                if (need > cap) {
                    cap = need * 2;
                    out->pcm = realloc(out->pcm, cap * sizeof(int16_t));
                    if (!out->pcm) { fail("out of memory"); ret = -1; goto done; }
                }
                memcpy(out->pcm + out->pcm_samples * frm->ch_layout.nb_channels,
                       frm->extended_data[0],
                       (size_t)frm->nb_samples * frm->ch_layout.nb_channels *
                           sizeof(int16_t));
                out->pcm_samples += frm->nb_samples;
            }
            out->samples += frm->nb_samples;
            av_frame_unref(frm);
        }
        if (ret == AVERROR_EOF)
            break;
        if (ret != AVERROR(EAGAIN)) {
            fail("receive_frame failed on %s (%s)", path, av_err2str(ret));
            goto done;
        }
    }

    for (ch = 0; ch < out->channels && ch < MAX_CH; ch++)
        out->rms[ch] = out->samples ? sqrt(acc[ch] / (double)out->samples) : 0.0;
    if (out->pcm)
        out->crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0,
                          (const uint8_t *)out->pcm,
                          out->pcm_samples * out->channels * sizeof(int16_t));
    ret = 0;

done:
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
    return ret < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------------ */
/* Encoding                                                                 */
/*                                                                          */
/* Feeds interleaved s16 through libswresample into whatever the encoder     */
/* wants — which is also how the shipped wrapper reaches libmp3lame, since    */
/* it takes s16p/s32p/fltp and never s16.                                    */
/* ------------------------------------------------------------------------ */

static int encode_pcm(const char *path, const char *muxer, const char *codec,
                      const int16_t *pcm, size_t frames, int rate, int channels,
                      int64_t bit_rate)
{
    AVFormatContext *oc = NULL;
    const AVCodec *enc;
    AVCodecContext *cc = NULL;
    AVStream *st;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frm = NULL;
    size_t pos = 0;
    int64_t pts = 0;
    int ret, block;

    enc = avcodec_find_encoder_by_name(codec);
    if (!enc) { fail("MISSING  encoder %s", codec); return -1; }

    if ((ret = avformat_alloc_output_context2(&oc, NULL, muxer, path)) < 0) {
        fail("no output context for muxer %s (%s)", muxer, av_err2str(ret));
        return -1;
    }
    cc = avcodec_alloc_context3(enc);
    st = avformat_new_stream(oc, NULL);
    pkt = av_packet_alloc();
    frm = av_frame_alloc();
    if (!cc || !st || !pkt || !frm) { fail("out of memory"); ret = -1; goto done; }

    /* The encoder's own first choice of sample format. flac takes s16, and
     * libmp3lame takes only planar input — which is exactly why swresample
     * sits in the middle here, as it does in the shipped wrapper. */
    {
        const enum AVSampleFormat *fmts = NULL;
        int nfmts = 0;
        cc->sample_fmt = AV_SAMPLE_FMT_S16;
        if (avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, (const void **)&fmts, &nfmts) >= 0 &&
            fmts && nfmts > 0)
            cc->sample_fmt = fmts[0];
    }
    cc->sample_rate = rate;
    cc->bit_rate    = bit_rate;
    av_channel_layout_default(&cc->ch_layout, channels);
    cc->time_base   = (AVRational){1, rate};
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(cc, enc, NULL)) < 0) {
        fail("cannot open encoder %s (%s)", codec, av_err2str(ret));
        goto done;
    }
    avcodec_parameters_from_context(st->codecpar, cc);
    st->time_base = cc->time_base;

    if (!(oc->oformat->flags & AVFMT_NOFILE))
        if ((ret = avio_open(&oc->pb, path, AVIO_FLAG_WRITE)) < 0) {
            fail("cannot write %s (%s)", path, av_err2str(ret));
            goto done;
        }
    if ((ret = avformat_write_header(oc, NULL)) < 0) {
        fail("write_header failed for %s (%s)", muxer, av_err2str(ret));
        goto done;
    }

    {
        AVChannelLayout in_layout;
        av_channel_layout_default(&in_layout, channels);
        ret = swr_alloc_set_opts2(&swr, &cc->ch_layout, cc->sample_fmt, rate,
                                  &in_layout, AV_SAMPLE_FMT_S16, rate, 0, NULL);
        av_channel_layout_uninit(&in_layout);
        if (ret < 0 || (ret = swr_init(swr)) < 0) {
            fail("swresample setup failed (%s)", av_err2str(ret));
            goto done;
        }
    }

    block = cc->frame_size > 0 ? cc->frame_size : 1024;
    while (pos < frames) {
        int n = (int)FFMIN((size_t)block, frames - pos);
        const uint8_t *in = (const uint8_t *)(pcm + pos * channels);

        frm->format      = cc->sample_fmt;
        frm->sample_rate = rate;
        frm->nb_samples  = n;
        av_channel_layout_copy(&frm->ch_layout, &cc->ch_layout);
        if ((ret = av_frame_get_buffer(frm, 0)) < 0) {
            fail("frame alloc failed (%s)", av_err2str(ret));
            goto done;
        }
        if ((ret = swr_convert(swr, frm->extended_data, n, &in, n)) < 0) {
            fail("swr_convert failed (%s)", av_err2str(ret));
            goto done;
        }
        frm->pts = pts;
        pts += n;
        pos += n;

        if ((ret = avcodec_send_frame(cc, frm)) < 0) {
            fail("send_frame to %s failed (%s)", codec, av_err2str(ret));
            goto done;
        }
        av_frame_unref(frm);
        while ((ret = avcodec_receive_packet(cc, pkt)) >= 0) {
            pkt->stream_index = st->index;
            av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
            if ((ret = av_interleaved_write_frame(oc, pkt)) < 0) {
                fail("write_frame failed (%s)", av_err2str(ret));
                goto done;
            }
        }
        if (ret != AVERROR(EAGAIN)) {
            fail("receive_packet from %s failed (%s)", codec, av_err2str(ret));
            goto done;
        }
    }

    if ((ret = avcodec_send_frame(cc, NULL)) < 0) {
        fail("flush of %s failed (%s)", codec, av_err2str(ret));
        goto done;
    }
    while ((ret = avcodec_receive_packet(cc, pkt)) >= 0) {
        pkt->stream_index = st->index;
        av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
        if ((ret = av_interleaved_write_frame(oc, pkt)) < 0) {
            fail("write_frame failed (%s)", av_err2str(ret));
            goto done;
        }
    }
    if (ret != AVERROR_EOF) {
        fail("drain of %s failed (%s)", codec, av_err2str(ret));
        goto done;
    }
    if ((ret = av_write_trailer(oc)) < 0) {
        fail("write_trailer failed for %s (%s)", muxer, av_err2str(ret));
        goto done;
    }
    ret = 0;

done:
    swr_free(&swr);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    if (oc && !(oc->oformat->flags & AVFMT_NOFILE) && oc->pb)
        avio_closep(&oc->pb);
    avformat_free_context(oc);
    return ret < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------------ */

static void expect_int(const char *what, int64_t got, int64_t want)
{
    if (got != want)
        fail("%s: got %" PRId64 ", expected %" PRId64, what, got, want);
}

static void expect_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want))
        fail("%s: got '%s', expected '%s'", what, got, want);
}

/* Asserts the codec ID rather than the decoder's name. A codec can have more
 * than one decoder — MP3 has the fixed-point "mp3" and the floating-point
 * "mp3float", and which of them avcodec_find_decoder() hands back depends on
 * which were enabled. The ID is the invariant; the name is only for the log. */
static void expect_codec(const char *what, const Decoded *d, enum AVCodecID want)
{
    if (d->id != want)
        fail("%s: got %s (decoder '%s'), expected %s", what,
             avcodec_get_name(d->id), d->codec, avcodec_get_name(want));
}

int main(int argc, char **argv)
{
    const char *fixtures = argc > 1 ? argv[1] : ".";
    const char *work     = argc > 2 ? argv[2] : ".";
    char path[1024], flac_out[1024], mp3_out[1024];
    Decoded src = {0}, rt = {0}, mp3 = {0}, eac3 = {0};
    int cpu = av_get_cpu_flags();
    int ch;

    /* The six amplitudes the E-AC-3 fixture's `pan` filter wrote, relative to
     * the first channel; see src/androidTest/assets/README.md for the exact
     * command. All six survive the encode — measured on the fixture with
     * `ffmpeg -af astats`, every channel lands within 0.0005 of its nominal
     * ratio, LFE included. A channel that goes missing, gets duplicated or
     * lands in the wrong slot moves one of these by at least 0.1. */
    static const double EAC3_RATIO[MAX_CH] = { 1.00, 0.80, 0.60, 0.20, 0.50, 0.40 };

    printf("=== decent-audio-ffmpeg arm32 smoke ===\n");
    printf("ffmpeg %s, libavcodec %d.%d.%d / libavformat %d.%d.%d\n",
           av_version_info(),
           LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
           LIBAVCODEC_VERSION_MICRO,
           LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR,
           LIBAVFORMAT_VERSION_MICRO);
    printf("configuration: %.200s\n", avcodec_configuration());
    printf("cpu flags: neon=%d vfp=%d vfpv3=%d armv6t2=%d\n",
           !!(cpu & AV_CPU_FLAG_NEON), !!(cpu & AV_CPU_FLAG_VFP),
           !!(cpu & AV_CPU_FLAG_VFPV3), !!(cpu & AV_CPU_FLAG_ARMV6T2));

    /* Not a claim about the build — av_get_cpu_flags() ORs the build-time
     * flags with the runtime hwcap, so this cannot tell them apart. It is a
     * claim about the RUN: without NEON advertised, every ARM SIMD path stays
     * dormant and this harness would be exercising the plain-C fallbacks while
     * reporting success. If it ever trips, the emulator changed, not us. */
    if (!(cpu & AV_CPU_FLAG_NEON))
        fail("no NEON at run time — the ARM SIMD paths would not be exercised");

    check_codec_set();

    /* 1. FLAC inside MP4 — mov demuxer + flac decoder, the pair the whole
     *    "stream copy instead of re-encode" route rests on. */
    snprintf(path, sizeof(path), "%s/flac_in_mp4.m4a", fixtures);
    if (decode_file(path, 1, &src) == 0) {
        expect_codec("flac_in_mp4 codec", &src, AV_CODEC_ID_FLAC);
        expect_int("flac_in_mp4 sample rate", src.rate, 44100);
        expect_int("flac_in_mp4 channels", src.channels, 2);
        expect_int("flac_in_mp4 samples", src.samples, 88200);
        expect_int("flac_in_mp4 captured samples", src.pcm_samples, 88200);
        if (src.fmt != AV_SAMPLE_FMT_S16)
            fail("flac_in_mp4 sample format: got %s, expected s16",
                 av_get_sample_fmt_name(src.fmt));
        /* The decode, pinned exactly. Everything else about this fixture is a
         * sanity bound; this is the assertion that says the samples are RIGHT
         * rather than merely plausible. */
        if (src.crc != FLAC_FIXTURE_CRC)
            fail("flac_in_mp4 decoded to crc %08" PRIx32 ", expected %08"
                 PRIx32 " — the decode is wrong, not just different",
                 src.crc, FLAC_FIXTURE_CRC);
        /* The fixture's tone measures -24.08 dBFS RMS, i.e. 0.0625, on both
         * channels (cross-checked with `ffmpeg -af astats`). Deliberately
         * loose: this exists to name silence or garbage in a readable way
         * before the CRC reports it as eight hex digits. */
        for (ch = 0; ch < src.channels; ch++)
            if (!(src.rms[ch] > 0.05 && src.rms[ch] < 0.08))
                fail("flac_in_mp4 channel %d RMS %.4f outside 0.05..0.08",
                     ch, src.rms[ch]);
        printf("flac_in_mp4.m4a: %s -> %s %d Hz %dch %s, %" PRId64
               " samples, crc %08" PRIx32 "\n",
               src.format, src.codec, src.rate, src.channels,
               av_get_sample_fmt_name(src.fmt), src.samples, src.crc);
    }

    /* 2. FLAC encoder + FLAC demuxer/decoder, bit-exact.
     *    Lossless means the round trip must return the identical PCM — an
     *    assertion with no tolerance and no magic number in it. Broken
     *    arithmetic anywhere in the FLAC path fails this and nothing else has
     *    to be tuned to notice. */
    if (src.pcm) {
        snprintf(flac_out, sizeof(flac_out), "%s/smoke.flac", work);
        if (encode_pcm(flac_out, "flac", "flac", src.pcm, src.pcm_samples,
                       src.rate, src.channels, 0) == 0 &&
            decode_file(flac_out, 1, &rt) == 0) {
            expect_str("round trip container", rt.format, "flac");
            expect_codec("round trip codec", &rt, AV_CODEC_ID_FLAC);
            expect_int("round trip samples", rt.samples, src.samples);
            if (rt.crc != src.crc)
                fail("round trip is not bit-exact: crc %08" PRIx32
                     " in, %08" PRIx32 " out", src.crc, rt.crc);
            else
                printf("flac round trip: bit-exact, %" PRId64
                       " samples, crc %08" PRIx32 "\n", rt.samples, rt.crc);
        }
    }

    /* 3. libmp3lame — the one component whose presence build-ffmpeg.sh can
     *    only infer from a "LAME3.100" string in libavcodec.so. Here it runs.
     *    Lossy, so the check is on level rather than on bytes. */
    if (src.pcm) {
        snprintf(mp3_out, sizeof(mp3_out), "%s/smoke.mp3", work);
        if (encode_pcm(mp3_out, "mp3", "libmp3lame", src.pcm, src.pcm_samples,
                       src.rate, src.channels, 320000) == 0 &&
            decode_file(mp3_out, 0, &mp3) == 0) {
            expect_str("mp3 container", mp3.format, "mp3");
            expect_codec("mp3 codec", &mp3, AV_CODEC_ID_MP3);
            expect_int("mp3 sample rate", mp3.rate, 44100);
            expect_int("mp3 channels", mp3.channels, 2);
            /* Relative, not absolute: the fixture's tone is quiet (0.0625),
             * so an absolute window would be satisfied by silence. */
            for (ch = 0; ch < mp3.channels; ch++)
                if (fabs(mp3.rms[ch] - src.rms[ch]) > 0.10 * src.rms[ch])
                    fail("mp3 channel %d RMS %.4f, source %.4f (>10%% off)",
                         ch, mp3.rms[ch], src.rms[ch]);
            printf("libmp3lame 320k: %" PRId64 " samples back, RMS %.4f/%.4f "
                   "(source %.4f/%.4f)\n", mp3.samples, mp3.rms[0], mp3.rms[1],
                   src.rms[0], src.rms[1]);
        }
    }

    /* 4. E-AC-3 5.1 — the Dolby branch's input, and the only fixture with
     *    more than two channels. Each channel was written at a different
     *    amplitude precisely so that a lost or duplicated one is visible. */
    snprintf(path, sizeof(path), "%s/eac3_51.mp4", fixtures);
    if (decode_file(path, 0, &eac3) == 0) {
        expect_codec("eac3_51 codec", &eac3, AV_CODEC_ID_EAC3);
        expect_int("eac3_51 sample rate", eac3.rate, 48000);
        expect_int("eac3_51 channels", eac3.channels, 6);
        /* -21.10 dBFS on the fixture's first channel. */
        if (!(eac3.rms[0] > 0.07 && eac3.rms[0] < 0.11))
            fail("eac3_51 channel 0 RMS %.4f outside 0.07..0.11", eac3.rms[0]);
        for (ch = 0; ch < 6 && eac3.rms[0] > 0; ch++) {
            double got = eac3.rms[ch] / eac3.rms[0];
            if (fabs(got - EAC3_RATIO[ch]) > 0.02)
                fail("eac3_51 channel %d level %.3f of channel 0, expected %.2f",
                     ch, got, EAC3_RATIO[ch]);
        }
        printf("eac3_51.mp4: %s -> %s %d Hz %dch, %" PRId64 " samples, "
               "RMS %.4f %.4f %.4f %.4f %.4f %.4f\n",
               eac3.format, eac3.codec, eac3.rate, eac3.channels, eac3.samples,
               eac3.rms[0], eac3.rms[1], eac3.rms[2],
               eac3.rms[3], eac3.rms[4], eac3.rms[5]);
    }

    free(src.pcm);
    free(rt.pcm);

    if (failed) {
        fprintf(stderr, "\nERROR: armeabi-v7a smoke failed (see above)\n");
        return 1;
    }
    printf("armeabi-v7a smoke OK\n");
    return 0;
}
