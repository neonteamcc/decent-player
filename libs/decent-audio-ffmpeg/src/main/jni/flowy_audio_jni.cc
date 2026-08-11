// JNI surface for com.decent.audio.FlowyFfmpeg.
//
// WHY THIS IS NOT AN argv WRAPPER
// -------------------------------
// The obvious shape for an ffmpeg binding is `int run(char **argv)` calling
// fftools/ffmpeg.c. That cannot be built here: this module's libraries are
// configured with --disable-avfilter (and --disable-programs), and ffmpeg's
// CLI is written on top of libavfilter — it builds a filter graph for every
// job, stream copy included. Enabling avfilter to get a command line back
// would mean carrying fftools' dozen-plus source files across ffmpeg upgrades,
// which is what ffmpeg-kit did before it was archived.
//
// The whole operation set the download pipeline needs is four things, and none
// of them is a filter graph:
//
//   * stream copy (remux)                     -> demux, avcodec_parameters_copy, mux
//   * decode -> MP3 320 via libmp3lame        -> decode, swresample, encode
//   * decode -> FLAC at a pinned bit depth    -> decode, swresample, encode
//   * stream copy + metadata + cover picture  -> the above, plus an attached_pic stream
//
// Sample format / sample rate / channel layout conversion between a decoder's
// output and an encoder's input is exactly libswresample's job, and swresample
// IS enabled. So the binding is typed instead of string-array shaped.

#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG "FlowyFfmpeg"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// Error reporting is thread-local: a download worker runs an operation and
// then reads lastError() on the same thread, and two workers must not
// overwrite each other's message.
thread_local std::string g_error;      // our own description of what failed
thread_local std::string g_avLogError; // the last ERROR-level line libav* logged

std::string errString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

void setError(int err, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    g_error.assign(msg);
    if (err != 0) {
        g_error += " (";
        g_error += errString(err);
        g_error += ")";
    }
    LOGW("%s", g_error.c_str());
}

void clearError() {
    g_error.clear();
    g_avLogError.clear();
}

// libav* logs are the only account of WHY a muxer or an encoder refused
// something, so they are captured rather than dropped on the floor. The
// callback runs on whatever thread is inside libav*, which is the same thread
// that called us — thread_local is therefore the right storage.
void logCallback(void *avcl, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char line[512];
    int prefix = 1;
    if (av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix) < 0) return;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n == 0) return;
    if (level <= AV_LOG_ERROR) g_avLogError.assign(line);
    __android_log_print(level <= AV_LOG_ERROR ? ANDROID_LOG_WARN : ANDROID_LOG_DEBUG,
                        LOG_TAG, "%s", line);
}

// ── JNI string helpers ──────────────────────────────────────────────────

// Owns the UTF chars for the lifetime of the scope; tolerates a null jstring,
// because every optional argument on the Java side (muxer, coverPath) is null
// when unused.
class JString {
public:
    JString(JNIEnv *env, jstring s) : env_(env), js_(s), c_(nullptr) {
        if (s != nullptr) c_ = env->GetStringUTFChars(s, nullptr);
    }
    ~JString() {
        if (c_ != nullptr) env_->ReleaseStringUTFChars(js_, c_);
    }
    JString(const JString &) = delete;
    JString &operator=(const JString &) = delete;
    const char *c_str() const { return c_; }
    bool empty() const { return c_ == nullptr || c_[0] == '\0'; }

private:
    JNIEnv *env_;
    jstring js_;
    const char *c_;
};

std::vector<std::string> toStringVector(JNIEnv *env, jobjectArray arr) {
    std::vector<std::string> out;
    if (arr == nullptr) return out;
    jsize n = env->GetArrayLength(arr);
    out.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; i++) {
        auto s = reinterpret_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (s == nullptr) {
            out.emplace_back();
            continue;
        }
        const char *c = env->GetStringUTFChars(s, nullptr);
        out.emplace_back(c != nullptr ? c : "");
        if (c != nullptr) env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
    }
    return out;
}

// ── Cancellation ────────────────────────────────────────────────────────
//
// This library sits in a download pipeline that cancels: the user aborts a
// queued track, the batch is paused, the process is backgrounded. A lossless
// transcode of a long track is minutes of work, so an operation that cannot be
// stopped is a stall the caller has no answer to.
//
// The cancel flag lives in NATIVE memory, not in a Java field, for one reason:
// it is read from libavformat's AVIOInterruptCB, which fires deep inside a
// blocking read on the operation's thread, and it is written from a completely
// different thread (whoever pressed cancel). A native atomic needs no JNIEnv
// on either side. Java holds an opaque handle (FlowyFfmpeg.Job).
//
// The handles are looked up through a registry rather than passed as raw
// pointers so that a Job closed while its own operation is still running
// cannot become a dangling pointer: the running operation holds a shared_ptr
// for its whole duration, and jobDestroy only drops the registry's reference.

struct Job {
    std::atomic<bool> cancelled{false};
};

std::mutex g_jobsMutex;
std::unordered_map<int64_t, std::shared_ptr<Job>> g_jobs;
int64_t g_nextJobId = 1;

std::shared_ptr<Job> lookupJob(int64_t id) {
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

// libavformat polls this while it blocks; returning non-zero makes the pending
// read or write fail with AVERROR_EXIT instead of running to completion.
int interruptCallback(void *opaque) {
    auto *job = static_cast<Job *>(opaque);
    return (job != nullptr && job->cancelled.load(std::memory_order_relaxed)) ? 1 : 0;
}

// ── Progress ────────────────────────────────────────────────────────────
//
// Reported on the calling thread — every operation runs synchronously inside
// its JNI call, so the JNIEnv the operation was entered with is valid for the
// callback and no AttachCurrentThread is needed.

class Progress {
public:
    Progress(JNIEnv *env, jobject listener) : env_(env), listener_(listener) {
        if (listener_ == nullptr) return;
        jclass cls = env->GetObjectClass(listener_);
        if (cls == nullptr) return;
        method_ = env->GetMethodID(cls, "onProgress", "(JJ)V");
        env->DeleteLocalRef(cls);
        if (method_ == nullptr) env->ExceptionClear();
    }

    void setDuration(int64_t durationMs) { durationMs_ = durationMs; }

    // Coalesced: a packet-rate callback would cross into the JVM thousands of
    // times for a single track and tell the caller nothing it can use.
    void report(int64_t positionMs) {
        if (method_ == nullptr || failed_) return;
        if (positionMs < 0) return;
        if (lastMs_ >= 0 && positionMs - lastMs_ < kMinIntervalMs) return;
        lastMs_ = positionMs;
        emit(positionMs);
    }

    // The final tick, so a caller driving a UI ends at 100% rather than at
    // whatever the last coalesced sample happened to be.
    void reportFinal() {
        if (method_ == nullptr || failed_) return;
        emit(durationMs_ > 0 ? durationMs_ : lastMs_);
    }

    // A listener that threw leaves an exception pending on this thread. Every
    // JNI call after that point is undefined, so the operation is unwound as
    // if it had been cancelled and the exception surfaces at the Java caller.
    bool failed() const { return failed_; }

private:
    static constexpr int64_t kMinIntervalMs = 250;

    void emit(int64_t positionMs) {
        env_->CallVoidMethod(listener_, method_, static_cast<jlong>(positionMs),
                             static_cast<jlong>(durationMs_));
        if (env_->ExceptionCheck()) failed_ = true;
    }

    JNIEnv *env_;
    jobject listener_;
    jmethodID method_ = nullptr;
    int64_t durationMs_ = 0;
    int64_t lastMs_ = -1;
    bool failed_ = false;
};

// The pair every operation carries. Both halves are optional: a caller that
// wants neither passes 0 / null and nothing changes.
struct Control {
    Job *job = nullptr;
    Progress *progress = nullptr;

    bool stopped() const {
        if (job != nullptr && job->cancelled.load(std::memory_order_relaxed)) return true;
        return progress != nullptr && progress->failed();
    }

    void setDuration(int64_t durationMs) const {
        if (progress != nullptr) progress->setDuration(durationMs);
    }

    void report(int64_t positionMs) const {
        if (progress != nullptr) progress->report(positionMs);
    }

    void reportFinal() const {
        if (progress != nullptr) progress->reportFinal();
    }
};

// ── Cover pictures ──────────────────────────────────────────────────────
//
// A cover is stored verbatim: the muxers write the compressed bytes into the
// container (MP4 `covr`, FLAC PICTURE, ID3v2 APIC) and never decode them, so
// no image codec has to be built into libavcodec. Only the codec id and the
// pixel dimensions are needed, and both are readable from the file's own
// header — which is why they are parsed here instead of decoded.

struct Cover {
    std::vector<uint8_t> data;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int width = 0;
    int height = 0;
};

bool readWholeFile(const char *path, std::vector<uint8_t> *out) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (64 << 20)) {
        fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(size));
    size_t got = fread(out->data(), 1, out->size(), f);
    fclose(f);
    return got == out->size();
}

// PNG: IHDR is fixed at offset 16 (8-byte signature, 4-byte length,
// 4-byte "IHDR"), width and height big-endian right after it.
bool parsePngSize(const std::vector<uint8_t> &d, int *w, int *h) {
    if (d.size() < 24) return false;
    *w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    *h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    return true;
}

// JPEG: walk the marker chain to the first SOFn (C0..CF, minus the four that
// are not frame headers) and read height/width out of it.
bool parseJpegSize(const std::vector<uint8_t> &d, int *w, int *h) {
    size_t i = 2;
    while (i + 9 < d.size()) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        size_t len = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                     marker != 0xC8 && marker != 0xCC;
        if (isSof) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return true;
        }
        if (marker == 0xDA) break; // start of scan; no frame header found
        if (len < 2) break;
        i += 2 + len;
    }
    return false;
}

bool loadCover(const char *path, Cover *cover) {
    if (!readWholeFile(path, &cover->data)) {
        setError(0, "cover: cannot read %s", path);
        return false;
    }
    const std::vector<uint8_t> &d = cover->data;
    if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        cover->codec = AV_CODEC_ID_PNG;
        parsePngSize(d, &cover->width, &cover->height);
        return true;
    }
    if (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8) {
        cover->codec = AV_CODEC_ID_MJPEG;
        parseJpegSize(d, &cover->width, &cover->height);
        return true;
    }
    setError(0, "cover: %s is neither JPEG nor PNG", path);
    return false;
}

// The mov muxer's is_cover_image() tests `disposition == AV_DISPOSITION_
// ATTACHED_PIC` for equality, not for the bit, so the disposition is assigned
// rather than or-ed. Getting that wrong turns the cover into a real video
// track, and the mp4 muxer then refuses the file for want of a video encoder.
int addCoverStream(AVFormatContext *oc, const Cover &cover) {
    AVStream *st = avformat_new_stream(oc, nullptr);
    if (st == nullptr) {
        setError(AVERROR(ENOMEM), "cover: avformat_new_stream failed");
        return AVERROR(ENOMEM);
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = cover.codec;
    st->codecpar->width = cover.width;
    st->codecpar->height = cover.height;
    st->disposition = AV_DISPOSITION_ATTACHED_PIC;
    st->time_base = AVRational{1, 1000};
    return st->index;
}

int writeCoverPacket(AVFormatContext *oc, int streamIndex, const Cover &cover) {
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) return AVERROR(ENOMEM);
    int ret = av_new_packet(pkt, static_cast<int>(cover.data.size()));
    if (ret >= 0) {
        memcpy(pkt->data, cover.data.data(), cover.data.size());
        pkt->stream_index = streamIndex;
        pkt->flags |= AV_PKT_FLAG_KEY;
        pkt->pts = 0;
        pkt->dts = 0;
        ret = av_interleaved_write_frame(oc, pkt);
    }
    av_packet_free(&pkt);
    if (ret < 0) setError(ret, "cover: writing the attached picture failed");
    return ret;
}

// ── Metadata ────────────────────────────────────────────────────────────

// Source metadata first, caller overrides on top: a key the caller does not
// mention survives the operation instead of being silently dropped.
void applyMetadata(AVFormatContext *oc, AVDictionary *inherited,
                   const std::vector<std::string> &pairs) {
    if (inherited != nullptr) av_dict_copy(&oc->metadata, inherited, 0);
    for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
        if (pairs[i].empty()) continue;
        // An empty value means "remove this key", which is how a caller drops
        // a tag inherited from the source.
        av_dict_set(&oc->metadata, pairs[i].c_str(),
                    pairs[i + 1].empty() ? nullptr : pairs[i + 1].c_str(), 0);
    }
}

// ── Input ───────────────────────────────────────────────────────────────

struct Input {
    AVFormatContext *fmt = nullptr;
    int streamIndex = -1;

    ~Input() {
        if (fmt != nullptr) avformat_close_input(&fmt);
    }
};

// The interrupt callback has to be on the context BEFORE it is opened, which
// means allocating it here instead of letting avformat_open_input do it —
// opening a file is itself one of the blocking steps a cancel has to reach.
int openInput(const char *path, Input *in, Job *job) {
    in->fmt = avformat_alloc_context();
    if (in->fmt == nullptr) {
        setError(AVERROR(ENOMEM), "cannot allocate a demuxer context");
        return AVERROR(ENOMEM);
    }
    if (job != nullptr) {
        in->fmt->interrupt_callback.callback = interruptCallback;
        in->fmt->interrupt_callback.opaque = job;
    }
    // On failure avformat_open_input frees the context and nulls the pointer,
    // so the destructor stays correct either way.
    int ret = avformat_open_input(&in->fmt, path, nullptr, nullptr);
    if (ret < 0) {
        setError(ret, "cannot open %s", path);
        return ret;
    }
    ret = avformat_find_stream_info(in->fmt, nullptr);
    if (ret < 0) {
        setError(ret, "no stream info in %s", path);
        return ret;
    }
    ret = av_find_best_stream(in->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ret < 0) {
        setError(ret, "no audio stream in %s", path);
        return ret;
    }
    in->streamIndex = ret;
    return 0;
}

// The stream's own duration when it has one, the container's otherwise, 0 when
// neither knows. Used both by probe() and as the denominator of progress.
int64_t durationMs(const AVFormatContext *fmt, const AVStream *st) {
    if (st != nullptr && st->duration > 0 && st->time_base.den > 0) {
        return av_rescale_q(st->duration, st->time_base, AVRational{1, 1000});
    }
    if (fmt->duration > 0) return fmt->duration / (AV_TIME_BASE / 1000);
    return 0;
}

// A packet's position in the source, in milliseconds, or -1 when it has no
// timestamp to report.
int64_t packetPositionMs(const AVPacket *pkt, AVRational timeBase) {
    int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE) return -1;
    return av_rescale_q(ts, timeBase, AVRational{1, 1000});
}

// ── Output ──────────────────────────────────────────────────────────────

struct Output {
    AVFormatContext *fmt = nullptr;
    std::string path;
    bool opened = false;    // we created the destination file
    bool completed = false; // av_write_trailer returned successfully

    // A half-written destination left behind by a failed job is worse than no
    // destination at all: it looks like a converted track to anything that
    // only stats the path. Only a file this object created is removed, so a
    // failure that never got as far as opening the output cannot delete
    // something that was already there.
    ~Output() {
        if (fmt == nullptr) return;
        if (fmt->pb != nullptr && !(fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt->pb);
        }
        avformat_free_context(fmt);
        if (opened && !completed && !path.empty()) remove(path.c_str());
    }
};

int openOutput(const char *path, const char *muxer, Output *out, Job *job) {
    int ret = avformat_alloc_output_context2(&out->fmt, nullptr, muxer, path);
    if (ret < 0 || out->fmt == nullptr) {
        setError(ret, "no muxer for %s (requested '%s')", path,
                 muxer != nullptr ? muxer : "<by extension>");
        return ret < 0 ? ret : AVERROR_MUXER_NOT_FOUND;
    }
    if (job != nullptr) {
        out->fmt->interrupt_callback.callback = interruptCallback;
        out->fmt->interrupt_callback.opaque = job;
    }
    return 0;
}

int openOutputFile(Output *out, const char *path, Job *job) {
    if (out->fmt->oformat->flags & AVFMT_NOFILE) return 0;
    // avio_open2 takes the interrupt callback the plain avio_open cannot, so a
    // cancel reaches a blocked write to a slow or full volume too.
    AVIOInterruptCB cb{interruptCallback, job};
    int ret = avio_open2(&out->fmt->pb, path, AVIO_FLAG_WRITE,
                         job != nullptr ? &cb : nullptr, nullptr);
    if (ret < 0) {
        setError(ret, "cannot write %s", path);
        return ret;
    }
    out->path = path;
    out->opened = true;
    return 0;
}

// ── Encoder configuration ───────────────────────────────────────────────
//
// FFmpeg 8 deprecated AVCodec's sample_fmts / supported_samplerates /
// ch_layouts arrays in favour of avcodec_get_supported_config(). A NULL list
// from that call means "the encoder accepts anything", which is why every
// helper below treats "no list" as "keep what the source had".

template <typename T>
const T *supportedConfig(const AVCodec *codec, AVCodecConfig kind, int *count) {
    const void *list = nullptr;
    *count = 0;
    if (avcodec_get_supported_config(nullptr, codec, kind, 0, &list, count) < 0) {
        return nullptr;
    }
    return reinterpret_cast<const T *>(list);
}

// Preference order matters: for libmp3lame, FLTP is what LAME works in
// internally, so asking for it keeps swresample from doing a second
// conversion inside the encoder.
AVSampleFormat pickSampleFormat(const AVCodec *codec,
                                const std::vector<AVSampleFormat> &preferred) {
    int n = 0;
    const AVSampleFormat *list = supportedConfig<AVSampleFormat>(
            codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, &n);
    if (list == nullptr || n == 0) {
        return preferred.empty() ? AV_SAMPLE_FMT_NONE : preferred[0];
    }
    for (AVSampleFormat want : preferred) {
        for (int i = 0; i < n; i++) {
            if (list[i] == want) return want;
        }
    }
    return list[0];
}

bool supportsSampleFormat(const AVCodec *codec, AVSampleFormat fmt) {
    int n = 0;
    const AVSampleFormat *list = supportedConfig<AVSampleFormat>(
            codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, &n);
    if (list == nullptr || n == 0) return true;
    for (int i = 0; i < n; i++) {
        if (list[i] == fmt) return true;
    }
    return false;
}

// Keeps the source rate when the encoder takes it; otherwise picks the closest
// one it does take, which is what a 96 kHz source into libmp3lame needs.
int pickSampleRate(const AVCodec *codec, int wanted) {
    int n = 0;
    const int *list = supportedConfig<int>(codec, AV_CODEC_CONFIG_SAMPLE_RATE, &n);
    if (list == nullptr || n == 0) return wanted;
    int best = list[0];
    int bestDelta = -1;
    for (int i = 0; i < n; i++) {
        if (list[i] == wanted) return wanted;
        int delta = list[i] > wanted ? list[i] - wanted : wanted - list[i];
        if (bestDelta < 0 || delta < bestDelta) {
            bestDelta = delta;
            best = list[i];
        }
    }
    return best;
}

// Downmix rather than fail: libmp3lame is stereo at most, and a multichannel
// source reaching the MP3 branch should produce an MP3, not an error.
// swresample rematrixes without any help from libavfilter.
int pickChannelLayout(const AVCodec *codec, const AVChannelLayout *source,
                      int maxChannels, AVChannelLayout *out) {
    AVChannelLayout wanted;
    memset(&wanted, 0, sizeof(wanted));
    if (source->nb_channels > 0 && source->nb_channels <= maxChannels) {
        int ret = av_channel_layout_copy(&wanted, source);
        if (ret < 0) return ret;
    } else {
        av_channel_layout_default(&wanted, maxChannels);
    }

    int n = 0;
    const AVChannelLayout *list = supportedConfig<AVChannelLayout>(
            codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, &n);
    if (list == nullptr || n == 0) {
        int ret = av_channel_layout_copy(out, &wanted);
        av_channel_layout_uninit(&wanted);
        return ret;
    }
    for (int i = 0; i < n; i++) {
        if (av_channel_layout_compare(&list[i], &wanted) == 0) {
            av_channel_layout_uninit(&wanted);
            return av_channel_layout_copy(out, &list[i]);
        }
    }
    // Not offered verbatim — take the widest layout the encoder does offer
    // that is no wider than what we asked for.
    int chosen = -1;
    for (int i = 0; i < n; i++) {
        if (list[i].nb_channels <= wanted.nb_channels &&
            (chosen < 0 || list[i].nb_channels > list[chosen].nb_channels)) {
            chosen = i;
        }
    }
    if (chosen < 0) chosen = 0;
    av_channel_layout_uninit(&wanted);
    return av_channel_layout_copy(out, &list[chosen]);
}

struct EncoderSpec {
    const char *name;
    int64_t bitrate = 0;         // bits/s; 0 leaves the encoder's default
    int bitsPerRawSample = 0;    // 0 leaves the encoder's default
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE; // NONE = pick a supported one
    std::vector<AVSampleFormat> preferredFormats;
    int maxChannels = 8;
};

// ── Muxing a stream of encoded packets ──────────────────────────────────

int writePacket(Output *out, AVPacket *pkt, AVRational from, AVStream *st) {
    av_packet_rescale_ts(pkt, from, st->time_base);
    pkt->stream_index = st->index;
    int ret = av_interleaved_write_frame(out->fmt, pkt);
    if (ret < 0) setError(ret, "writing a packet failed");
    return ret;
}

// ── Operation: stream copy ──────────────────────────────────────────────

int doRemux(const char *src, const char *dst, const char *muxer,
            const std::vector<std::string> &metadata, const char *coverPath,
            const Control &ctl) {
    Input in;
    int ret = openInput(src, &in, ctl.job);
    if (ret < 0) return ret;

    Output out;
    ret = openOutput(dst, muxer, &out, ctl.job);
    if (ret < 0) return ret;

    AVStream *ist = in.fmt->streams[in.streamIndex];
    ctl.setDuration(durationMs(in.fmt, ist));
    AVStream *ost = avformat_new_stream(out.fmt, nullptr);
    if (ost == nullptr) {
        setError(AVERROR(ENOMEM), "avformat_new_stream failed");
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_copy(ost->codecpar, ist->codecpar);
    if (ret < 0) {
        setError(ret, "cannot copy stream parameters");
        return ret;
    }
    // The source's container tag means nothing in the destination container;
    // leaving it set makes the muxer either reject the stream or write a tag
    // the destination does not define.
    ost->codecpar->codec_tag = 0;
    ost->time_base = ist->time_base;
    av_dict_copy(&ost->metadata, ist->metadata, 0);

    Cover cover;
    int coverIndex = -1;
    if (coverPath != nullptr) {
        if (!loadCover(coverPath, &cover)) return AVERROR(EINVAL);
        coverIndex = addCoverStream(out.fmt, cover);
        if (coverIndex < 0) return coverIndex;
    }

    applyMetadata(out.fmt, in.fmt->metadata, metadata);

    ret = openOutputFile(&out, dst, ctl.job);
    if (ret < 0) return ret;
    ret = avformat_write_header(out.fmt, nullptr);
    if (ret < 0) {
        setError(ret, "the %s muxer refused the header", out.fmt->oformat->name);
        return ret;
    }

    if (coverIndex >= 0) {
        ret = writeCoverPacket(out.fmt, coverIndex, cover);
        if (ret < 0) return ret;
    }

    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) return AVERROR(ENOMEM);
    while ((ret = av_read_frame(in.fmt, pkt)) >= 0) {
        if (pkt->stream_index != in.streamIndex) {
            av_packet_unref(pkt);
            continue;
        }
        // Checked here as well as in the interrupt callback: a local file
        // rarely blocks, so the callback alone can leave a whole track's worth
        // of copying between a cancel and the exit.
        if (ctl.stopped()) {
            av_packet_unref(pkt);
            ret = AVERROR_EXIT;
            break;
        }
        int64_t posMs = packetPositionMs(pkt, ist->time_base);
        ret = writePacket(&out, pkt, ist->time_base, ost);
        av_packet_unref(pkt);
        if (ret < 0) break;
        ctl.report(posMs);
    }
    av_packet_free(&pkt);
    if (ret == AVERROR_EOF) ret = 0;
    if (ret == AVERROR_EXIT) {
        setError(0, "cancelled");
        return ret;
    }
    if (ret < 0) {
        if (g_error.empty()) setError(ret, "reading %s failed", src);
        return ret;
    }

    ret = av_write_trailer(out.fmt);
    if (ret < 0) {
        setError(ret, "closing %s failed", dst);
        return ret;
    }
    out.completed = true;
    ctl.reportFinal();
    return 0;
}

// ── Operation: decode -> encode ─────────────────────────────────────────

int encodeAndWrite(Output *out, AVCodecContext *enc, AVStream *ost, AVFrame *frame) {
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0) {
        setError(ret, "the %s encoder rejected a frame", enc->codec->name);
        return ret;
    }
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) return AVERROR(ENOMEM);
    while (true) {
        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            setError(ret, "the %s encoder failed", enc->codec->name);
            break;
        }
        ret = writePacket(out, pkt, enc->time_base, ost);
        av_packet_unref(pkt);
        if (ret < 0) break;
    }
    av_packet_free(&pkt);
    return ret;
}

// Drains whole encoder frames out of the FIFO. `flush` also takes the final
// short frame, which every encoder accepts as its last one.
int drainFifo(AVAudioFifo *fifo, AVCodecContext *enc, Output *out, AVStream *ost,
              int64_t *pts, bool flush) {
    const int frameSize = enc->frame_size > 0 ? enc->frame_size : 4096;
    while (av_audio_fifo_size(fifo) >= (flush ? 1 : frameSize)) {
        int take = av_audio_fifo_size(fifo);
        if (take > frameSize) take = frameSize;

        AVFrame *frame = av_frame_alloc();
        if (frame == nullptr) return AVERROR(ENOMEM);
        frame->nb_samples = take;
        frame->format = enc->sample_fmt;
        frame->sample_rate = enc->sample_rate;
        int ret = av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
        if (ret >= 0) ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            av_frame_free(&frame);
            setError(ret, "cannot allocate an encoder frame");
            return ret;
        }
        if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(frame->extended_data), take) <
            take) {
            av_frame_free(&frame);
            setError(0, "short read from the sample FIFO");
            return AVERROR_UNKNOWN;
        }
        frame->pts = *pts;
        *pts += take;
        ret = encodeAndWrite(out, enc, ost, frame);
        av_frame_free(&frame);
        if (ret < 0) return ret;
    }
    return 0;
}

int doTranscode(const char *src, const char *dst, const char *muxer,
                const EncoderSpec &spec, const std::vector<std::string> &metadata,
                const char *coverPath, const Control &ctl) {
    Input in;
    int ret = openInput(src, &in, ctl.job);
    if (ret < 0) return ret;

    AVStream *ist = in.fmt->streams[in.streamIndex];
    ctl.setDuration(durationMs(in.fmt, ist));
    const AVCodec *decCodec = avcodec_find_decoder(ist->codecpar->codec_id);
    if (decCodec == nullptr) {
        setError(0, "no decoder for %s in this build",
                 avcodec_get_name(ist->codecpar->codec_id));
        return AVERROR_DECODER_NOT_FOUND;
    }
    AVCodecContext *dec = avcodec_alloc_context3(decCodec);
    if (dec == nullptr) return AVERROR(ENOMEM);
    struct DecGuard {
        AVCodecContext **c;
        ~DecGuard() { avcodec_free_context(c); }
    } decGuard{&dec};

    ret = avcodec_parameters_to_context(dec, ist->codecpar);
    if (ret < 0) {
        setError(ret, "cannot configure the %s decoder", decCodec->name);
        return ret;
    }
    dec->pkt_timebase = ist->time_base;
    ret = avcodec_open2(dec, decCodec, nullptr);
    if (ret < 0) {
        setError(ret, "cannot open the %s decoder", decCodec->name);
        return ret;
    }

    const AVCodec *encCodec = avcodec_find_encoder_by_name(spec.name);
    if (encCodec == nullptr) {
        setError(0, "encoder '%s' is not present in this build", spec.name);
        return AVERROR_ENCODER_NOT_FOUND;
    }

    Output out;
    ret = openOutput(dst, muxer, &out, ctl.job);
    if (ret < 0) return ret;

    AVCodecContext *enc = avcodec_alloc_context3(encCodec);
    if (enc == nullptr) return AVERROR(ENOMEM);
    struct EncGuard {
        AVCodecContext **c;
        ~EncGuard() { avcodec_free_context(c); }
    } encGuard{&enc};

    if (spec.sampleFormat != AV_SAMPLE_FMT_NONE) {
        if (!supportsSampleFormat(encCodec, spec.sampleFormat)) {
            setError(0, "the %s encoder does not take %s samples", spec.name,
                     av_get_sample_fmt_name(spec.sampleFormat));
            return AVERROR(EINVAL);
        }
        enc->sample_fmt = spec.sampleFormat;
    } else {
        enc->sample_fmt = pickSampleFormat(encCodec, spec.preferredFormats);
    }
    enc->sample_rate = pickSampleRate(encCodec, dec->sample_rate);
    ret = pickChannelLayout(encCodec, &dec->ch_layout, spec.maxChannels, &enc->ch_layout);
    if (ret < 0) {
        setError(ret, "cannot settle a channel layout for %s", spec.name);
        return ret;
    }
    if (spec.bitrate > 0) enc->bit_rate = spec.bitrate;
    if (spec.bitsPerRawSample > 0) enc->bits_per_raw_sample = spec.bitsPerRawSample;
    enc->time_base = AVRational{1, enc->sample_rate};
    // MP4 keeps codec configuration in the sample description, not in the
    // packets, so the encoder has to be told to emit it out of band.
    if (out.fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(enc, encCodec, nullptr);
    if (ret < 0) {
        setError(ret, "cannot open the %s encoder", spec.name);
        return ret;
    }

    AVStream *ost = avformat_new_stream(out.fmt, nullptr);
    if (ost == nullptr) {
        setError(AVERROR(ENOMEM), "avformat_new_stream failed");
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_from_context(ost->codecpar, enc);
    if (ret < 0) {
        setError(ret, "cannot describe the encoded stream");
        return ret;
    }
    ost->time_base = enc->time_base;
    av_dict_copy(&ost->metadata, ist->metadata, 0);

    Cover cover;
    int coverIndex = -1;
    if (coverPath != nullptr) {
        if (!loadCover(coverPath, &cover)) return AVERROR(EINVAL);
        coverIndex = addCoverStream(out.fmt, cover);
        if (coverIndex < 0) return coverIndex;
    }

    applyMetadata(out.fmt, in.fmt->metadata, metadata);

    ret = openOutputFile(&out, dst, ctl.job);
    if (ret < 0) return ret;
    ret = avformat_write_header(out.fmt, nullptr);
    if (ret < 0) {
        setError(ret, "the %s muxer refused the header", out.fmt->oformat->name);
        return ret;
    }

    if (coverIndex >= 0) {
        ret = writeCoverPacket(out.fmt, coverIndex, cover);
        if (ret < 0) return ret;
    }

    SwrContext *swr = nullptr;
    ret = swr_alloc_set_opts2(&swr, &enc->ch_layout, enc->sample_fmt, enc->sample_rate,
                              &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
    if (ret < 0 || swr == nullptr) {
        setError(ret, "cannot set up the resampler");
        return ret < 0 ? ret : AVERROR(ENOMEM);
    }
    struct SwrGuard {
        SwrContext **s;
        ~SwrGuard() { swr_free(s); }
    } swrGuard{&swr};
    ret = swr_init(swr);
    if (ret < 0) {
        setError(ret, "cannot initialise the resampler");
        return ret;
    }

    AVAudioFifo *fifo = av_audio_fifo_alloc(enc->sample_fmt, enc->ch_layout.nb_channels, 1);
    if (fifo == nullptr) return AVERROR(ENOMEM);
    struct FifoGuard {
        AVAudioFifo *f;
        ~FifoGuard() { av_audio_fifo_free(f); }
    } fifoGuard{fifo};

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (pkt == nullptr || frame == nullptr) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    struct IoGuard {
        AVPacket **p;
        AVFrame **f;
        ~IoGuard() {
            av_packet_free(p);
            av_frame_free(f);
        }
    } ioGuard{&pkt, &frame};

    int64_t pts = 0;

    // Pushes one decoded frame (or a flush, when `decoded` is null) through
    // the resampler and into the FIFO.
    auto resampleInto = [&](AVFrame *decoded) -> int {
        int outSamples = swr_get_out_samples(
                swr, decoded != nullptr ? decoded->nb_samples : 0);
        if (outSamples <= 0) return 0;
        uint8_t **buf = nullptr;
        int lineSize = 0;
        int allocRet = av_samples_alloc_array_and_samples(
                &buf, &lineSize, enc->ch_layout.nb_channels, outSamples, enc->sample_fmt, 0);
        if (allocRet < 0) {
            setError(allocRet, "cannot allocate a resampling buffer");
            return allocRet;
        }
        int converted = swr_convert(
                swr, buf, outSamples,
                decoded != nullptr ? const_cast<const uint8_t **>(decoded->extended_data)
                                   : nullptr,
                decoded != nullptr ? decoded->nb_samples : 0);
        int rc = 0;
        if (converted < 0) {
            setError(converted, "resampling failed");
            rc = converted;
        } else if (converted > 0) {
            if (av_audio_fifo_write(fifo, reinterpret_cast<void **>(buf), converted) <
                converted) {
                setError(0, "short write into the sample FIFO");
                rc = AVERROR_UNKNOWN;
            }
        }
        if (buf != nullptr) av_freep(&buf[0]);
        av_freep(&buf);
        return rc;
    };

    auto drainDecoder = [&]() -> int {
        while (true) {
            int rc = avcodec_receive_frame(dec, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
            if (rc < 0) {
                setError(rc, "the %s decoder failed", decCodec->name);
                return rc;
            }
            rc = resampleInto(frame);
            av_frame_unref(frame);
            if (rc < 0) return rc;
            rc = drainFifo(fifo, enc, &out, ost, &pts, false);
            if (rc < 0) return rc;
        }
    };

    while ((ret = av_read_frame(in.fmt, pkt)) >= 0) {
        if (pkt->stream_index != in.streamIndex) {
            av_packet_unref(pkt);
            continue;
        }
        // A transcode is CPU-bound, not IO-bound: the interrupt callback would
        // only fire on the (rare, fast) reads, so the flag is polled here where
        // the time actually goes.
        if (ctl.stopped()) {
            av_packet_unref(pkt);
            setError(0, "cancelled");
            return AVERROR_EXIT;
        }
        int64_t posMs = packetPositionMs(pkt, ist->time_base);
        ret = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            setError(ret, "the %s decoder rejected a packet", decCodec->name);
            return ret;
        }
        ret = drainDecoder();
        if (ret < 0) return ret;
        ctl.report(posMs);
    }
    if (ret == AVERROR_EXIT) {
        setError(0, "cancelled");
        return ret;
    }
    if (ret != AVERROR_EOF && ret < 0) {
        setError(ret, "reading %s failed", src);
        return ret;
    }

    // Flush, in order: decoder, resampler, FIFO, encoder. Skipping any one of
    // them truncates the tail of the file rather than failing loudly.
    ret = avcodec_send_packet(dec, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        setError(ret, "cannot flush the %s decoder", decCodec->name);
        return ret;
    }
    ret = drainDecoder();
    if (ret < 0) return ret;
    ret = resampleInto(nullptr);
    if (ret < 0) return ret;
    ret = drainFifo(fifo, enc, &out, ost, &pts, true);
    if (ret < 0) return ret;
    ret = encodeAndWrite(&out, enc, ost, nullptr);
    if (ret < 0) return ret;

    ret = av_write_trailer(out.fmt);
    if (ret < 0) {
        setError(ret, "closing %s failed", dst);
        return ret;
    }
    out.completed = true;
    ctl.reportFinal();
    return 0;
}

} // namespace

// ── JNI ─────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    av_log_set_callback(logCallback);
    return JNI_VERSION_1_6;
}

// FlowyFfmpeg.CANCELLED has to be a compile-time constant on the Java side —
// it is what a caller compares a return value against. Pinning it here means a
// future ffmpeg that renumbered the tag breaks the build instead of turning
// every cancellation into an unrecognised error code at runtime.
static_assert(AVERROR_EXIT == -1414092869, "FlowyFfmpeg.CANCELLED no longer matches AVERROR_EXIT");

// ── Job lifecycle ───────────────────────────────────────────────────────

extern "C" JNIEXPORT jlong JNICALL
Java_com_decent_audio_FlowyFfmpeg_jobCreate(JNIEnv *, jclass) {
    auto job = std::make_shared<Job>();
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    int64_t id = g_nextJobId++;
    g_jobs.emplace(id, std::move(job));
    return static_cast<jlong>(id);
}

// Called from any thread, including while the operation this handle belongs to
// is running — which is the entire point.
extern "C" JNIEXPORT void JNICALL
Java_com_decent_audio_FlowyFfmpeg_jobCancel(JNIEnv *, jclass, jlong handle) {
    std::shared_ptr<Job> job = lookupJob(static_cast<int64_t>(handle));
    if (job) job->cancelled.store(true, std::memory_order_relaxed);
}

// Only drops the registry's reference. An operation still running with this
// handle holds its own shared_ptr and keeps reading a live flag.
extern "C" JNIEXPORT void JNICALL
Java_com_decent_audio_FlowyFfmpeg_jobDestroy(JNIEnv *, jclass, jlong handle) {
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    g_jobs.erase(static_cast<int64_t>(handle));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_decent_audio_FlowyFfmpeg_probe(JNIEnv *env, jclass, jstring jpath) {
    clearError();
    JString path(env, jpath);
    if (path.empty()) {
        setError(0, "probe: path is null or empty");
        return nullptr;
    }

    Input in;
    if (openInput(path.c_str(), &in, nullptr) < 0) return nullptr;

    AVStream *st = in.fmt->streams[in.streamIndex];
    const AVCodecParameters *par = st->codecpar;
    const char *codecName = avcodec_get_name(par->codec_id);
    int64_t duration = durationMs(in.fmt, st);

    jclass cls = env->FindClass("com/decent/audio/FlowyFfmpeg$AudioInfo");
    if (cls == nullptr) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>",
                                      "(Ljava/lang/String;IIIJLjava/lang/String;J)V");
    if (ctor == nullptr) return nullptr;

    jstring jcodec = env->NewStringUTF(codecName != nullptr ? codecName : "unknown");
    jstring jformat = env->NewStringUTF(in.fmt->iformat->name != nullptr
                                                ? in.fmt->iformat->name
                                                : "unknown");
    return env->NewObject(cls, ctor, jcodec, static_cast<jint>(par->ch_layout.nb_channels),
                          static_cast<jint>(par->sample_rate),
                          static_cast<jint>(par->bits_per_raw_sample),
                          static_cast<jlong>(duration), jformat,
                          static_cast<jlong>(par->bit_rate));
}

namespace {

// Resolves the handle once, for the whole operation. The shared_ptr is what
// makes a close() racing the running job harmless.
struct Held {
    std::shared_ptr<Job> job;
    Progress progress;

    Held(JNIEnv *env, jlong handle, jobject listener)
        : job(lookupJob(static_cast<int64_t>(handle))), progress(env, listener) {}

    Control control() { return Control{job.get(), &progress}; }

    // Cancelled before the operation started: do not open anything at all.
    bool alreadyCancelled() const {
        return job && job->cancelled.load(std::memory_order_relaxed);
    }
};

} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_decent_audio_FlowyFfmpeg_nativeRemux(JNIEnv *env, jclass, jstring jsrc, jstring jdst,
                                              jstring jmuxer, jobjectArray jmeta,
                                              jstring jcover, jlong jjob,
                                              jobject jprogress) {
    clearError();
    JString src(env, jsrc);
    JString dst(env, jdst);
    JString muxer(env, jmuxer);
    JString cover(env, jcover);
    if (src.empty() || dst.empty()) {
        setError(0, "remux: source and destination paths are required");
        return AVERROR(EINVAL);
    }
    Held held(env, jjob, jprogress);
    if (held.alreadyCancelled()) {
        setError(0, "cancelled");
        return AVERROR_EXIT;
    }
    std::vector<std::string> metadata = toStringVector(env, jmeta);
    return doRemux(src.c_str(), dst.c_str(), muxer.empty() ? nullptr : muxer.c_str(),
                   metadata, cover.empty() ? nullptr : cover.c_str(), held.control());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_decent_audio_FlowyFfmpeg_nativeEncodeMp3(JNIEnv *env, jclass, jstring jsrc,
                                                  jstring jdst, jint bitrateKbps,
                                                  jobjectArray jmeta, jstring jcover,
                                                  jlong jjob, jobject jprogress) {
    clearError();
    JString src(env, jsrc);
    JString dst(env, jdst);
    JString cover(env, jcover);
    if (src.empty() || dst.empty()) {
        setError(0, "encodeMp3: source and destination paths are required");
        return AVERROR(EINVAL);
    }
    Held held(env, jjob, jprogress);
    if (held.alreadyCancelled()) {
        setError(0, "cancelled");
        return AVERROR_EXIT;
    }
    EncoderSpec spec;
    spec.name = "libmp3lame";
    spec.bitrate = bitrateKbps > 0 ? static_cast<int64_t>(bitrateKbps) * 1000 : 0;
    spec.preferredFormats = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S16P};
    spec.maxChannels = 2; // libmp3lame is mono or stereo; anything wider is downmixed
    std::vector<std::string> metadata = toStringVector(env, jmeta);
    return doTranscode(src.c_str(), dst.c_str(), "mp3", spec, metadata,
                       cover.empty() ? nullptr : cover.c_str(), held.control());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_decent_audio_FlowyFfmpeg_nativeEncodeFlac(JNIEnv *env, jclass, jstring jsrc,
                                                   jstring jdst, jint bitsPerSample,
                                                   jobjectArray jmeta, jstring jcover,
                                                   jlong jjob, jobject jprogress) {
    clearError();
    JString src(env, jsrc);
    JString dst(env, jdst);
    JString cover(env, jcover);
    if (src.empty() || dst.empty()) {
        setError(0, "encodeFlac: source and destination paths are required");
        return AVERROR(EINVAL);
    }
    if (bitsPerSample != 16 && bitsPerSample != 24) {
        setError(0, "encodeFlac: bitsPerSample must be 16 or 24, got %d", bitsPerSample);
        return AVERROR(EINVAL);
    }
    Held held(env, jjob, jprogress);
    if (held.alreadyCancelled()) {
        setError(0, "cancelled");
        return AVERROR_EXIT;
    }
    EncoderSpec spec;
    spec.name = "flac";
    // The FLAC encoder reads 24-bit content out of S32 samples and shifts them
    // down itself; bits_per_raw_sample is what tells it to (flacenc.c pins
    // bps_code 6 for S32 with 24 declared bits). S16 is its own 16-bit case.
    spec.sampleFormat = bitsPerSample == 24 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    spec.bitsPerRawSample = bitsPerSample;
    spec.maxChannels = 8; // FLAC's own ceiling
    std::vector<std::string> metadata = toStringVector(env, jmeta);
    return doTranscode(src.c_str(), dst.c_str(), "flac", spec, metadata,
                       cover.empty() ? nullptr : cover.c_str(), held.control());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_decent_audio_FlowyFfmpeg_hasEncoder(JNIEnv *env, jclass, jstring jname) {
    JString name(env, jname);
    if (name.empty()) return JNI_FALSE;
    return avcodec_find_encoder_by_name(name.c_str()) != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_decent_audio_FlowyFfmpeg_hasDecoder(JNIEnv *env, jclass, jstring jname) {
    JString name(env, jname);
    if (name.empty()) return JNI_FALSE;
    return avcodec_find_decoder_by_name(name.c_str()) != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_decent_audio_FlowyFfmpeg_version(JNIEnv *env, jclass) {
    unsigned v = avcodec_version();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u", AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v),
             AV_VERSION_MICRO(v));
    return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_decent_audio_FlowyFfmpeg_lastError(JNIEnv *env, jclass) {
    std::string combined = g_error;
    if (!g_avLogError.empty()) {
        if (!combined.empty()) combined += " | ";
        combined += g_avLogError;
    }
    return env->NewStringUTF(combined.c_str());
}
