#include "engine.h"
#include <QLocale>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/replaygain.h>
#include <libswresample/swresample.h>
}

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

// FFmpeg channel → PipeWire position; channels without one get mixed down to stereo.
static uint32_t spaPosition(AVChannel c)
{
    switch (c) {
    case AV_CHAN_FRONT_LEFT: return SPA_AUDIO_CHANNEL_FL;
    case AV_CHAN_FRONT_RIGHT: return SPA_AUDIO_CHANNEL_FR;
    case AV_CHAN_FRONT_CENTER: return SPA_AUDIO_CHANNEL_FC;
    case AV_CHAN_LOW_FREQUENCY: return SPA_AUDIO_CHANNEL_LFE;
    case AV_CHAN_BACK_LEFT: return SPA_AUDIO_CHANNEL_RL;
    case AV_CHAN_BACK_RIGHT: return SPA_AUDIO_CHANNEL_RR;
    case AV_CHAN_FRONT_LEFT_OF_CENTER: return SPA_AUDIO_CHANNEL_FLC;
    case AV_CHAN_FRONT_RIGHT_OF_CENTER: return SPA_AUDIO_CHANNEL_FRC;
    case AV_CHAN_BACK_CENTER: return SPA_AUDIO_CHANNEL_RC;
    case AV_CHAN_SIDE_LEFT: return SPA_AUDIO_CHANNEL_SL;
    case AV_CHAN_SIDE_RIGHT: return SPA_AUDIO_CHANNEL_SR;
    case AV_CHAN_TOP_CENTER: return SPA_AUDIO_CHANNEL_TC;
    case AV_CHAN_TOP_FRONT_LEFT: return SPA_AUDIO_CHANNEL_TFL;
    case AV_CHAN_TOP_FRONT_CENTER: return SPA_AUDIO_CHANNEL_TFC;
    case AV_CHAN_TOP_FRONT_RIGHT: return SPA_AUDIO_CHANNEL_TFR;
    case AV_CHAN_TOP_BACK_LEFT: return SPA_AUDIO_CHANNEL_TRL;
    case AV_CHAN_TOP_BACK_CENTER: return SPA_AUDIO_CHANNEL_TRC;
    case AV_CHAN_TOP_BACK_RIGHT: return SPA_AUDIO_CHANNEL_TRR;
    case AV_CHAN_LOW_FREQUENCY_2: return SPA_AUDIO_CHANNEL_LFE2;
    default: return SPA_AUDIO_CHANNEL_UNKNOWN;
    }
}

// "-6.52 dB" → -6.52; NAN when missing
static float parseDb(const char *v)
{
    if (!v) return NAN;
    char *end = nullptr;
    const float f = std::strtof(v, &end);
    return end == v ? NAN : f;
}

// ───────────────────────────── Decoder ──────────────────────────────

class Decoder {
public:
    ~Decoder() { close(); }

    bool open(const QString &path, QString &err)
    {
        const QByteArray p = path.toUtf8();
        int r = avformat_open_input(&fmt_, p.constData(), nullptr, nullptr);
        if (r < 0) return fail(r, err);
        if ((r = avformat_find_stream_info(fmt_, nullptr)) < 0) return fail(r, err);
        const AVCodec *codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) {
            err = "No hay pista de audio";
            return false;
        }
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (int(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;
        AVStream *st = fmt_->streams[stream_];
        cc_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc_, st->codecpar);
        cc_->pkt_timebase = st->time_base;
        if ((r = avcodec_open2(cc_, codec, nullptr)) < 0) return fail(r, err);

        rate = cc_->sample_rate;
        if (cc_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            int n = cc_->ch_layout.nb_channels;
            av_channel_layout_uninit(&cc_->ch_layout);
            av_channel_layout_default(&cc_->ch_layout, n);
        }
        if (rate <= 0 || cc_->ch_layout.nb_channels <= 0) {
            err = "Formato de audio desconocido";
            return false;
        }
        // Surround goes out as it is (PipeWire mixes it down for two speakers);
        // layouts PipeWire has no names for are mixed down to stereo here.
        const int inCh = cc_->ch_layout.nb_channels;
        bool native = inCh > 2 && inCh <= kMaxChannels;
        for (int i = 0; native && i < inCh; ++i)
            native = spaPosition(av_channel_layout_channel_from_index(&cc_->ch_layout, i)) != SPA_AUDIO_CHANNEL_UNKNOWN;
        positions.fill(SPA_AUDIO_CHANNEL_UNKNOWN);
        if (native) {
            channels = inCh;
            av_channel_layout_copy(&outLayout_, &cc_->ch_layout);
            for (int i = 0; i < inCh; ++i) positions[i] = spaPosition(av_channel_layout_channel_from_index(&cc_->ch_layout, i));
        } else {
            channels = inCh >= 2 ? 2 : 1;
            av_channel_layout_default(&outLayout_, channels);
            if (channels == 1) positions[0] = SPA_AUDIO_CHANNEL_MONO;
            else positions[0] = SPA_AUDIO_CHANNEL_FL, positions[1] = SPA_AUDIO_CHANNEL_FR;
        }
        if (!setupSwr(err)) return false;
        readReplayGain(st);

        if (fmt_->duration > 0) duration = fmt_->duration / double(AV_TIME_BASE);
        else if (st->duration > 0) duration = st->duration * av_q2d(st->time_base);
        tb_ = av_q2d(st->time_base);

        // "FLAC · 44,1 kHz · 24 bit" / "MP3 · 320 kbps · 44,1 kHz"
        QString name = QString::fromLatin1(avcodec_get_name(cc_->codec_id)).toUpper();
        if (name.startsWith("PCM_")) name = "PCM";
        const AVCodecDescriptor *desc = avcodec_descriptor_get(cc_->codec_id);
        const bool lossless = (desc && desc->props & AV_CODEC_PROP_LOSSLESS) || name == "FLAC" || name == "ALAC"
                              || name == "PCM" || name == "WAVPACK" || name == "APE" || name == "TTA";
        const int bits = st->codecpar->bits_per_raw_sample ? st->codecpar->bits_per_raw_sample
                                                           : st->codecpar->bits_per_coded_sample;
        int64_t br = st->codecpar->bit_rate ? st->codecpar->bit_rate : fmt_->bit_rate;
        QLocale es("es_ES");
        QStringList parts{name};
        if (!lossless && br > 0) parts << QString("%1 kbps").arg(br / 1000);
        parts << es.toString(rate / 1000.0, 'g', 4) + " kHz";
        if (lossless && bits > 0) parts << QString("%1 bit").arg(bits);
        if (inCh == 1) parts << "mono";
        else if (inCh == 2) parts << "estéreo";
        else if (native) {
            char desc[64] = {};
            av_channel_layout_describe(&cc_->ch_layout, desc, sizeof desc);
            QString d = QString::fromLatin1(desc);
            d = d.section('(', 0, 0);   // "5.1(side)" → "5.1"
            parts << (d.contains('.') ? d : QString("%1 canales").arg(inCh));
        } else parts << QString("%1 canales → estéreo").arg(inCh);
        info = parts.join(" · ");

        pkt_ = av_packet_alloc();
        frm_ = av_frame_alloc();
        return true;
    }

    bool seek(double sec)
    {
        int64_t ts = int64_t(sec * AV_TIME_BASE);
        if (avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0) < 0
            && av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(cc_);
        swr_close(swr_);
        swr_init(swr_);
        skipTo_ = sec;
        eof = flushing_ = false;
        return true;
    }

    // Decodes roughly one packet and appends interleaved float samples.
    void decode(std::vector<float> &out)
    {
        for (;;) {
            int r = avcodec_receive_frame(cc_, frm_);
            if (r == 0) {
                convert(out);
                av_frame_unref(frm_);
                return;
            }
            // EAGAIN or a corrupt frame (skipped, like every player does): feed more
            if (r == AVERROR_EOF || flushing_) {
                eof = true;
                return;
            }
            r = av_read_frame(fmt_, pkt_);
            if (r < 0) {
                avcodec_send_packet(cc_, nullptr);
                flushing_ = true;
                continue;
            }
            if (pkt_->stream_index == stream_) avcodec_send_packet(cc_, pkt_);
            av_packet_unref(pkt_);
        }
    }

    // ReplayGain as a linear factor (1 = untagged or off), never above what the peak allows
    float gain(ReplayGain mode) const
    {
        if (mode == ReplayGain::Off) return 1.0f;
        float db = mode == ReplayGain::Album && !std::isnan(albumDb) ? albumDb : trackDb;
        float peak = mode == ReplayGain::Album && !std::isnan(albumDb) ? albumPeak : trackPeak;
        if (std::isnan(db)) db = albumDb, peak = albumPeak;
        if (std::isnan(db)) return 1.0f;
        float g = std::pow(10.0f, db / 20.0f);
        if (peak > 0) g = std::min(g, 1.0f / peak);
        return g;
    }

    int rate = 0, channels = 2;
    std::array<uint32_t, kMaxChannels> positions{};
    double duration = 0;
    bool eof = false;
    QString info;
    float trackDb = NAN, trackPeak = 0, albumDb = NAN, albumPeak = 0;

private:
    bool fail(int r, QString &err)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(r, buf, sizeof buf);
        err = QString::fromUtf8(buf);
        return false;
    }

    bool setupSwr(QString &err)
    {
        int r = swr_alloc_set_opts2(&swr_, &outLayout_, AV_SAMPLE_FMT_FLT, rate, &cc_->ch_layout, cc_->sample_fmt,
                                    cc_->sample_rate, 0, nullptr);
        if (r < 0 || (r = swr_init(swr_)) < 0) return fail(r, err);
        return true;
    }

    // REPLAYGAIN_* tags (Vorbis, APE, ID3 TXXX, MP4 freeform), Opus R128 tags,
    // and what FFmpeg itself found (LAME header) as a last resort.
    void readReplayGain(AVStream *st)
    {
        auto get = [&](const char *key) -> const char * {
            for (AVDictionary *d : {fmt_->metadata, st->metadata})
                if (const AVDictionaryEntry *e = av_dict_get(d, key, nullptr, 0)) return e->value;
            return nullptr;
        };
        trackDb = parseDb(get("REPLAYGAIN_TRACK_GAIN"));
        trackPeak = std::isnan(parseDb(get("REPLAYGAIN_TRACK_PEAK"))) ? 0 : parseDb(get("REPLAYGAIN_TRACK_PEAK"));
        albumDb = parseDb(get("REPLAYGAIN_ALBUM_GAIN"));
        albumPeak = std::isnan(parseDb(get("REPLAYGAIN_ALBUM_PEAK"))) ? 0 : parseDb(get("REPLAYGAIN_ALBUM_PEAK"));
        // Opus: Q7.8 dB relative to -23 LUFS; ReplayGain's reference is 5 dB louder
        if (std::isnan(trackDb) && get("R128_TRACK_GAIN")) trackDb = std::atoi(get("R128_TRACK_GAIN")) / 256.0f + 5.0f;
        if (std::isnan(albumDb) && get("R128_ALBUM_GAIN")) albumDb = std::atoi(get("R128_ALBUM_GAIN")) / 256.0f + 5.0f;
        if (!std::isnan(trackDb) || !std::isnan(albumDb)) return;
        const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_REPLAYGAIN);
        if (!sd || sd->size < sizeof(AVReplayGain)) return;
        const auto *rg = reinterpret_cast<const AVReplayGain *>(sd->data);
        if (rg->track_gain != INT32_MIN) trackDb = rg->track_gain / 100000.0f, trackPeak = rg->track_peak / 100000.0f;
        if (rg->album_gain != INT32_MIN) albumDb = rg->album_gain / 100000.0f, albumPeak = rg->album_peak / 100000.0f;
    }

    void convert(std::vector<float> &out)
    {
        int n = frm_->nb_samples;
        if (n <= 0) return;
        // Some streams change format mid-way (e.g. chained Ogg): rebuild swr.
        if (frm_->format != cc_->sample_fmt || frm_->sample_rate != cc_->sample_rate) {
            cc_->sample_fmt = AVSampleFormat(frm_->format);
            QString e;
            swr_free(&swr_);
            setupSwr(e);
        }
        int drop = 0;
        if (skipTo_ >= 0) {
            int64_t ts = frm_->best_effort_timestamp;
            if (ts != AV_NOPTS_VALUE) {
                double t = ts * tb_;
                double end = t + double(n) / frm_->sample_rate;
                if (end <= skipTo_) return;
                if (t < skipTo_) drop = int((skipTo_ - t) * frm_->sample_rate);
            }
            skipTo_ = -1;
        }
        const int cap = swr_get_out_samples(swr_, n);
        const size_t base = out.size();
        out.resize(base + size_t(cap) * channels);
        uint8_t *dst = reinterpret_cast<uint8_t *>(out.data() + base);
        int got = swr_convert(swr_, &dst, cap, const_cast<const uint8_t **>(frm_->extended_data), n);
        if (got < 0) got = 0;
        drop = std::min(drop, got);
        if (drop) std::memmove(out.data() + base, out.data() + base + size_t(drop) * channels,
                               size_t(got - drop) * channels * sizeof(float));
        out.resize(base + size_t(got - drop) * channels);
    }

    void close()
    {
        av_channel_layout_uninit(&outLayout_);
        av_packet_free(&pkt_);
        av_frame_free(&frm_);
        swr_free(&swr_);
        avcodec_free_context(&cc_);
        avformat_close_input(&fmt_);
    }

    AVFormatContext *fmt_ = nullptr;
    AVCodecContext *cc_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVPacket *pkt_ = nullptr;
    AVFrame *frm_ = nullptr;
    AVChannelLayout outLayout_{};
    int stream_ = -1;
    double tb_ = 0, skipTo_ = -1;
    bool flushing_ = false;
};

// ───────────────────────────── Output ───────────────────────────────

class Output {
public:
    Output(Ring &ring, Dsp &dsp, std::function<void()> onLost) : ring_(ring), dsp_(dsp), onLost_(std::move(onLost))
    {
        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("sonata", nullptr);
        pw_thread_loop_start(loop_);
    }
    ~Output()
    {
        pw_thread_loop_lock(loop_);
        destroyStream();
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
    }

    int rate() const { return rate_.load(std::memory_order_relaxed); }
    int channels() const { return ch_.load(std::memory_order_relaxed); }
    const std::array<uint32_t, kMaxChannels> &positions() const { return pos_; }   // decoder thread only

    void open(int rate, int ch, const std::array<uint32_t, kMaxChannels> &pos)
    {
        pw_thread_loop_lock(loop_);
        destroyStream();
        rate_ = rate;
        ch_ = ch;
        pos_ = pos;
        createStream();
        pw_thread_loop_unlock(loop_);
    }

    // PipeWire went away (restarted, crashed): build the stream again, same format.
    void reopen()
    {
        pw_thread_loop_lock(loop_);
        if (rate_ > 0) {
            destroyStream();
            createStream();
        }
        pw_thread_loop_unlock(loop_);
    }

    void setActive(bool on)
    {
        pw_thread_loop_lock(loop_);
        active_ = on;
        if (stream_) pw_stream_set_active(stream_, on);
        pw_thread_loop_unlock(loop_);
    }

    // Samples (frames × channels) handed to PipeWire, or held by the limiter, but not yet heard.
    uint64_t pendingSamples() const { return pending_.load(std::memory_order_relaxed); }

private:
    void createStream()   // loop locked
    {
        // ~23 ms quanta: imperceptible for pause/seek/EQ, few wakeups.
        const QByteArray lat = QByteArray::number(1024) + "/" + QByteArray::number(rate_.load());
        const QByteArray nodeRate = "1/" + QByteArray::number(rate_.load());
        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_APP_NAME, "Sonata", PW_KEY_APP_ID, "sonata", PW_KEY_APP_ICON_NAME, "sonata",
            PW_KEY_NODE_NAME, "sonata", PW_KEY_NODE_DESCRIPTION, "Sonata",
            PW_KEY_NODE_LATENCY, lat.constData(), PW_KEY_NODE_RATE, nodeRate.constData(), nullptr);
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = &Output::onProcess;
            e.state_changed = &Output::onState;
            return e;
        }();
        if (const QByteArray t = qgetenv("SONATA_TARGET"); !t.isEmpty())   // tests: play into a null sink
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, t.constData());
        stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "sonata", props, &events, this);
        if (!stream_) {   // no PipeWire right now: try again shortly
            onLost_();
            return;
        }

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = uint32_t(rate_.load());
        info.channels = uint32_t(ch_.load());
        for (int c = 0; c < ch_ && c < kMaxChannels; ++c) info.position[c] = pos_[c];
        const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
                                     | (active_ ? 0 : PW_STREAM_FLAG_INACTIVE));
        pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
    }

    void destroyStream()
    {
        closing_ = true;   // our own disconnect is not a lost connection
        if (stream_) pw_stream_destroy(stream_);
        closing_ = false;
        stream_ = nullptr;
        pending_ = 0;
    }

    // An unplugged or switched output is WirePlumber's job (it moves the
    // stream to the new default). Only a dead connection needs us.
    static void onState(void *data, pw_stream_state, pw_stream_state state, const char *)
    {
        auto *o = static_cast<Output *>(data);
        if (o->closing_) return;
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) o->onLost_();
    }

    static void onProcess(void *data)
    {
        auto *o = static_cast<Output *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(o->stream_);
        if (!b) return;
        spa_data &d = b->buffer->datas[0];
        auto *dst = static_cast<float *>(d.data);
        if (!dst) return;
        const int ch = o->ch_.load(std::memory_order_relaxed);
        const uint32_t stride = sizeof(float) * uint32_t(ch);
        uint32_t frames = d.maxsize / stride;
        if (b->requested) frames = std::min<uint32_t>(frames, uint32_t(b->requested));
        const unsigned live = o->dsp_.begin(frames);
        const size_t want = size_t(live) * ch;
        bool jump = false;
        const size_t got = o->ring_.read(dst, want, &jump);
        if (got < want) std::memset(dst + got, 0, (want - got) * sizeof(float));
        o->dsp_.process(dst, frames, live, jump);
        d.chunk->offset = 0;
        d.chunk->stride = int32_t(stride);
        d.chunk->size = frames * stride;

        pw_time t{};
        if (pw_stream_get_time_n(o->stream_, &t, sizeof t) == 0 && t.rate.denom) {
            const double delayFrames = double(t.delay) * t.rate.num * o->rate_ / t.rate.denom;
            o->pending_.store(uint64_t((std::max(0.0, delayFrames) + t.buffered + frames + Dsp::kLookahead) * ch),
                              std::memory_order_relaxed);
        }
        pw_stream_queue_buffer(o->stream_, b);
    }

    Ring &ring_;
    Dsp &dsp_;
    std::function<void()> onLost_;
    pw_thread_loop *loop_ = nullptr;
    pw_stream *stream_ = nullptr;
    std::atomic<int> rate_{0}, ch_{0};
    std::array<uint32_t, kMaxChannels> pos_{};
    bool active_ = false, closing_ = false;
    std::atomic<uint64_t> pending_{0};
};

// ───────────────────────────── Engine ───────────────────────────────

int Engine::scopeSnapshot(float *out, size_t n) const
{
    const int ch = std::max(1, out_->channels());
    const_cast<Dsp &>(dsp_).scope().snapshot(out, n, out_->pendingSamples() / uint64_t(ch));
    return out_->rate() > 0 ? out_->rate() : 44100;
}

Engine::Engine(QObject *parent) : QObject(parent), ring_(1u << 18)
{
    // any thread (PipeWire's, ours): rebuild the stream from the GUI thread, a little later
    out_ = std::make_unique<Output>(ring_, dsp_, [this] {
        QMetaObject::invokeMethod(this, [this] { if (!reconnect_.isActive()) reconnect_.start(); }, Qt::QueuedConnection);
    });
    poll_.setInterval(250);   // seek bar and clock; the lyrics extrapolate between ticks
    connect(&poll_, &QTimer::timeout, this, &Engine::pollAudible);
    reconnect_.setSingleShot(true);
    reconnect_.setInterval(500);
    connect(&reconnect_, &QTimer::timeout, this, [this] { out_->reopen(); });
    // the stream stops only once the fade-out has played
    idle_.setSingleShot(true);
    idle_.setInterval(120);
    connect(&idle_, &QTimer::timeout, this, [this] {
        if (state_ != Playing) out_->setActive(false);
    });
    thread_ = std::thread([this] { run(); });
}

Engine::~Engine()
{
    {
        std::lock_guard lk(mx_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
    out_.reset();
}

void Engine::play(const QString &path, quint64 token, double startSec, bool paused)
{
    {
        std::lock_guard lk(mx_);
        cmd_ = {Command::Play, path, token, startSec};
    }
    cv_.notify_all();
    eofAll_ = false;
    drainTicks_ = 0;
    idle_.stop();
    dsp_.setPaused(paused);
    out_->setActive(!paused);
    setState(paused ? Paused : Playing);
}

void Engine::setNext(const QString &path, quint64 token)
{
    {
        std::lock_guard lk(mx_);
        nextPath_ = path;
        nextToken_ = token;
    }
    cv_.notify_all();
}

void Engine::setPaused(bool paused)
{
    if (state_ == Stopped) return;
    dsp_.setPaused(paused);
    if (paused) idle_.start();
    else {
        idle_.stop();
        out_->setActive(true);
    }
    setState(paused ? Paused : Playing);
}

void Engine::setReplayGain(ReplayGain mode)
{
    if (rgMode_.exchange(mode) == mode) return;
    // what is already decoded carries the old gain: decode again from where we are
    if (state_ != Stopped) seek(position());
}

void Engine::stop()
{
    {
        std::lock_guard lk(mx_);
        cmd_ = {Command::Stop};
    }
    cv_.notify_all();
    dsp_.setPaused(true);
    idle_.start();
    setState(Stopped);
}

void Engine::seek(double sec)
{
    if (state_ == Stopped) return;
    quint64 tok;
    QString path;
    {
        std::lock_guard sl(segMx_);
        auto it = std::find_if(segs_.begin(), segs_.end(), [&](const Segment &s) { return s.token == audible_; });
        if (it == segs_.end()) return;
        sec = std::clamp(sec, 0.0, std::max(0.0, it->duration - 0.25));
        tok = it->token;
        path = it->path;
    }
    {
        std::lock_guard lk(mx_);
        cmd_ = {Command::Seek, path, tok, sec};
    }
    cv_.notify_all();
    eofAll_ = false;
    drainTicks_ = 0;
    emit seeked(sec);
}

void Engine::setVolume(float v)
{
    volume_ = std::clamp(v, 0.0f, 1.0f);
    dsp_.setVolume(volume_);
}

void Engine::setState(State s)
{
    if (s == state_) return;
    state_ = s;
    if (s == Playing) poll_.start();   // nothing moves while paused
    else poll_.stop();
    emit stateChanged(s);
}

double Engine::position() const
{
    const uint64_t r = ring_.readIndex();
    const uint64_t pend = out_->pendingSamples();
    std::lock_guard sl(segMx_);
    const Segment *cur = nullptr;
    for (const Segment &s : segs_)
        if (s.token == audible_) cur = &s;
    if (!cur) return 0;
    const double heard = (double(r) - double(cur->start) - double(pend)) / (double(cur->rate) * cur->channels);
    return std::clamp(cur->startSec + std::max(0.0, heard), 0.0, cur->duration > 0 ? cur->duration : 1e9);
}

double Engine::duration() const
{
    std::lock_guard sl(segMx_);
    for (const Segment &s : segs_)
        if (s.token == audible_) return s.duration;
    return 0;
}

QString Engine::formatInfo() const
{
    std::lock_guard sl(segMx_);
    for (const Segment &s : segs_) {
        if (s.token != audible_) continue;
        if (s.gain == 1.0f) return s.info;
        const double db = 20 * std::log10(double(s.gain));
        return s.info + " · " + QLocale("es_ES").toString(db, 'f', 1).replace('-', QChar(0x2212)) + " dB";
    }
    return {};
}

// Main thread, every 250 ms while playing: which segment is the speaker on?
void Engine::pollAudible()
{
    const uint64_t r = ring_.readIndex();
    const uint64_t pend = out_->pendingSamples();
    const uint64_t heard = r > pend ? r - pend : 0;
    quint64 tok = 0;
    {
        std::lock_guard sl(segMx_);
        for (const Segment &s : segs_)
            if (s.start <= heard || &s == &segs_.front()) tok = s.token;
        // forget segments that are completely behind us
        while (segs_.size() > 1 && segs_[1].start <= heard) segs_.erase(segs_.begin());
    }
    if (tok && tok != audible_) {
        audible_ = tok;
        emit trackStarted(tok);
    }
    if (state_ == Playing) emit tick(position());

    if (eofAll_ && ring_.filled() == 0 && state_ == Playing) {
        // let PipeWire play out what it already has
        if (++drainTicks_ * poll_.interval() > 1000.0 * pend / std::max(1, out_->rate() * out_->channels()) + 100) {
            drainTicks_ = 0;
            stop();
            emit finished();
        }
    }
}

// ─────────────── decoder thread ───────────────

bool Engine::openTrack(const QString &path, quint64 token, double startSec, bool flush)
{
    auto d = std::make_unique<Decoder>();
    QString err;
    if (!d->open(path, err) || (startSec > 0 && !d->seek(startSec))) {
        if (err.isEmpty()) err = "No se puede abrir";
        QMetaObject::invokeMethod(this, [this, token, path, err] {
            emit failed(token, path, err);
        }, Qt::QueuedConnection);
        return false;
    }
    dec_ = std::move(d);
    pend_.clear();
    pendOff_ = 0;
    Segment seg{0, token, startSec, dec_->duration, dec_->rate, dec_->channels, path, dec_->info,
                dec_->gain(rgMode_.load()), dec_->positions};
    const bool sameFmt = out_->rate() == dec_->rate && out_->channels() == dec_->channels && out_->positions() == dec_->positions;
    if (flush) {
        seg.start = ring_.flush();
        if (!sameFmt) {
            dsp_.setFormat(dec_->rate, dec_->channels);
            out_->open(dec_->rate, dec_->channels, dec_->positions);
        }
        std::lock_guard sl(segMx_);
        segs_ = {seg};
    } else if (sameFmt) {                          // gapless
        seg.start = ring_.writeIndex();
        std::lock_guard sl(segMx_);
        segs_.push_back(seg);
    } else {                                       // wait for the ring to drain first
        fmtSwitchPending_ = true;
        fmtSwitchSeg_ = seg;
    }
    return true;
}

void Engine::handle(const Command &c)
{
    fmtSwitchPending_ = false;
    switch (c.kind) {
    case Command::Play:
        if (!openTrack(c.path, c.token, c.sec, true)) {
            dec_.reset();
            ring_.flush();
        }
        break;
    case Command::Seek: {
        Segment seg;
        bool inPlace = false;
        {
            std::lock_guard sl(segMx_);
            if (dec_ && !segs_.empty() && segs_.back().token == c.token) {
                seg = segs_.back();
                inPlace = true;
            }
        }
        if (inPlace && dec_->seek(c.sec)) {
            pend_.clear();
            pendOff_ = 0;
            seg.start = ring_.flush();
            seg.startSec = c.sec;
            seg.gain = dec_->gain(rgMode_.load());
            std::lock_guard sl(segMx_);
            segs_ = {seg};
        } else {
            openTrack(c.path, c.token, c.sec, true);
        }
        break;
    }
    case Command::Stop:
        dec_.reset();
        pend_.clear();
        pendOff_ = 0;
        ring_.flush();
        {
            std::lock_guard sl(segMx_);
            segs_.clear();
        }
        break;
    case Command::None:
        break;
    }
}

void Engine::run()
{
    std::unique_lock lk(mx_);
    while (!quit_) {
        if (cmd_.kind != Command::None) {
            Command c = cmd_;
            cmd_ = {};
            lk.unlock();
            handle(c);
            lk.lock();
            continue;
        }
        if (!dec_) {
            cv_.wait(lk);
            continue;
        }
        // Sleep long when the ring is full: ~half a ring of audio.
        const auto napMs = std::chrono::milliseconds(
            std::clamp<int>(int(500.0 * ring_.capacity() / std::max(1, dec_->rate * dec_->channels)), 20, 1500));

        if (fmtSwitchPending_) {
            if (ring_.filled() == 0) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(120));   // let PipeWire play the tail
                dsp_.setFormat(fmtSwitchSeg_.rate, fmtSwitchSeg_.channels);
                out_->open(fmtSwitchSeg_.rate, fmtSwitchSeg_.channels, fmtSwitchSeg_.positions);
                fmtSwitchSeg_.start = ring_.flush();
                {
                    std::lock_guard sl(segMx_);
                    segs_.push_back(fmtSwitchSeg_);
                }
                fmtSwitchPending_ = false;
                lk.lock();
            } else {
                cv_.wait_for(lk, std::chrono::milliseconds(40));
            }
            continue;
        }

        if (pendOff_ < pend_.size()) {
            pendOff_ += ring_.write(pend_.data() + pendOff_, pend_.size() - pendOff_);
            if (pendOff_ < pend_.size()) {
                cv_.wait_for(lk, napMs);
                continue;
            }
            pend_.clear();
            pendOff_ = 0;
        }

        if (dec_->eof) {
            if (nextPath_.isEmpty()) {
                eofAll_ = true;
                cv_.wait(lk);
                continue;
            }
            const QString path = nextPath_;
            const quint64 tok = nextToken_;
            nextPath_.clear();
            lk.unlock();
            eofAll_ = false;
            if (!openTrack(path, tok, 0, false)) eofAll_ = true;   // GUI will offer another next
            lk.lock();
            continue;
        }

        if (ring_.space() < ring_.capacity() / 4) {
            cv_.wait_for(lk, napMs);
            continue;
        }
        lk.unlock();
        dec_->decode(pend_);
        // ReplayGain here rather than in the realtime callback: exact at track boundaries, free when off
        if (const float g = dec_->gain(rgMode_.load()); g != 1.0f)
            for (float &x : pend_) x *= g;
        lk.lock();
    }
}
