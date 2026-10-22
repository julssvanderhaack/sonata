#include "engine.h"
#include <QLocale>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

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
        channels = cc_->ch_layout.nb_channels >= 2 ? 2 : 1;
        if (rate <= 0 || cc_->ch_layout.nb_channels <= 0) {
            err = "Formato de audio desconocido";
            return false;
        }
        if (!setupSwr(err)) return false;

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
        const int inCh = cc_->ch_layout.nb_channels;
        parts << (inCh == 1 ? "mono" : inCh == 2 ? "estéreo" : QString("%1 canales → estéreo").arg(inCh));
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

    int rate = 0, channels = 2;
    double duration = 0;
    bool eof = false;
    QString info;

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
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, channels);
        int r = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, rate, &cc_->ch_layout, cc_->sample_fmt,
                                    cc_->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (r < 0 || (r = swr_init(swr_)) < 0) return fail(r, err);
        return true;
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
    int stream_ = -1;
    double tb_ = 0, skipTo_ = -1;
    bool flushing_ = false;
};

// ───────────────────────────── Output ───────────────────────────────

class Output {
public:
    Output(Ring &ring, Dsp &dsp) : ring_(ring), dsp_(dsp)
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

    int rate() const { return rate_; }
    int channels() const { return ch_; }

    void open(int rate, int ch)
    {
        pw_thread_loop_lock(loop_);
        destroyStream();
        rate_ = rate;
        ch_ = ch;
        // ~23 ms quanta: imperceptible for pause/seek/EQ, few wakeups.
        const QByteArray lat = QByteArray::number(1024) + "/" + QByteArray::number(rate);
        const QByteArray nodeRate = "1/" + QByteArray::number(rate);
        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_APP_NAME, "Sonata", PW_KEY_APP_ID, "sonata", PW_KEY_APP_ICON_NAME, "sonata",
            PW_KEY_NODE_NAME, "sonata", PW_KEY_NODE_DESCRIPTION, "Sonata",
            PW_KEY_NODE_LATENCY, lat.constData(), PW_KEY_NODE_RATE, nodeRate.constData(), nullptr);
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = &Output::onProcess;
            return e;
        }();
        if (const QByteArray t = qgetenv("SONATA_TARGET"); !t.isEmpty())   // tests: play into a null sink
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, t.constData());
        stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "sonata", props, &events, this);

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = uint32_t(rate);
        info.channels = uint32_t(ch);
        if (ch == 1) info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        else {
            info.position[0] = SPA_AUDIO_CHANNEL_FL;
            info.position[1] = SPA_AUDIO_CHANNEL_FR;
        }
        const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
                                     | (active_ ? 0 : PW_STREAM_FLAG_INACTIVE));
        pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
        pw_thread_loop_unlock(loop_);
    }

    void setActive(bool on)
    {
        pw_thread_loop_lock(loop_);
        active_ = on;
        if (stream_) pw_stream_set_active(stream_, on);
        pw_thread_loop_unlock(loop_);
    }

    // Samples (frames × channels) handed to PipeWire but not yet heard.
    uint64_t pendingSamples() const { return pending_.load(std::memory_order_relaxed); }

private:
    void destroyStream()
    {
        if (stream_) pw_stream_destroy(stream_);
        stream_ = nullptr;
        pending_ = 0;
    }

    static void onProcess(void *data)
    {
        auto *o = static_cast<Output *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(o->stream_);
        if (!b) return;
        spa_data &d = b->buffer->datas[0];
        auto *dst = static_cast<float *>(d.data);
        if (!dst) return;
        const uint32_t stride = sizeof(float) * uint32_t(o->ch_);
        uint32_t frames = d.maxsize / stride;
        if (b->requested) frames = std::min<uint32_t>(frames, uint32_t(b->requested));
        const size_t want = size_t(frames) * o->ch_;
        const size_t got = o->ring_.read(dst, want);
        if (got < want) std::memset(dst + got, 0, (want - got) * sizeof(float));
        o->dsp_.process(dst, frames);
        d.chunk->offset = 0;
        d.chunk->stride = int32_t(stride);
        d.chunk->size = frames * stride;

        pw_time t{};
        if (pw_stream_get_time_n(o->stream_, &t, sizeof t) == 0 && t.rate.denom) {
            const double delayFrames = double(t.delay) * t.rate.num * o->rate_ / t.rate.denom;
            o->pending_.store(uint64_t((std::max(0.0, delayFrames) + t.buffered + frames) * o->ch_),
                              std::memory_order_relaxed);
        }
        pw_stream_queue_buffer(o->stream_, b);
    }

    Ring &ring_;
    Dsp &dsp_;
    pw_thread_loop *loop_ = nullptr;
    pw_stream *stream_ = nullptr;
    int rate_ = 0, ch_ = 0;
    bool active_ = false;
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
    out_ = std::make_unique<Output>(ring_, dsp_);
    poll_.setInterval(250);   // seek bar and clock; the lyrics extrapolate between ticks
    connect(&poll_, &QTimer::timeout, this, &Engine::pollAudible);
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
    out_->setActive(!paused);
    setState(paused ? Paused : Playing);
}

void Engine::stop()
{
    {
        std::lock_guard lk(mx_);
        cmd_ = {Command::Stop};
    }
    cv_.notify_all();
    out_->setActive(false);
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
    for (const Segment &s : segs_)
        if (s.token == audible_) return s.info;
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
    Segment seg{0, token, startSec, dec_->duration, dec_->rate, dec_->channels, path, dec_->info};
    const bool sameFmt = out_->rate() == dec_->rate && out_->channels() == dec_->channels;
    if (flush) {
        seg.start = ring_.flush();
        if (!sameFmt) {
            dsp_.setFormat(dec_->rate, dec_->channels);
            out_->open(dec_->rate, dec_->channels);
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
                out_->open(fmtSwitchSeg_.rate, fmtSwitchSeg_.channels);
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
        lk.lock();
    }
}
