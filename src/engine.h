// Playback engine: FFmpeg decoder thread -> lock-free ring -> PipeWire stream.
//
// The decoder runs ahead of the speaker by at most one ring's worth of audio
// and then sleeps, so steady playback costs a wakeup every few hundred ms.
// Tracks with the same sample rate are written back to back into the ring
// (gapless); a rate change waits for the ring to drain and reopens the stream
// at the new rate, so nothing is ever resampled by us.
#pragma once
#include "eq.h"
#include "ring.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Decoder;
class Output;

class Engine : public QObject {
    Q_OBJECT
public:
    enum State { Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    void play(const QString &path, quint64 token, double startSec = 0, bool paused = false);
    void setNext(const QString &path, quint64 token);   // empty path = nothing queued
    void setPaused(bool paused);
    void togglePause() { setPaused(state_ == Playing); }
    void stop();
    void seek(double sec);

    State state() const { return state_; }
    double position() const;
    double duration() const;
    quint64 currentToken() const { return audible_; }
    QString formatInfo() const;

    void setVolume(float v);
    float volume() const { return volume_; }
    void setEq(const EqSettings &s) { dsp_.setEq(s); }
    // spectrum view: latest n mono frames as they reach the speaker; returns the sample rate
    void setScopeEnabled(bool on) { dsp_.scope().setEnabled(on); }
    int scopeSnapshot(float *out, size_t n) const;

signals:
    void trackStarted(quint64 token);
    void finished();
    void failed(quint64 token, const QString &path, const QString &message);
    void stateChanged(Engine::State state);
    void tick(double position);
    void seeked(double position);

private:
    struct Segment {
        uint64_t start;          // ring sample index of the first sample
        quint64 token;
        double startSec, duration;
        int rate, channels;
        QString path, info;
    };
    struct Command {
        enum Kind { None, Play, Seek, Stop } kind = None;
        QString path;
        quint64 token = 0;
        double sec = 0;
    };

    void run();
    void handle(const Command &c);
    bool openTrack(const QString &path, quint64 token, double startSec, bool flush);
    void pollAudible();
    void setState(State s);

    Ring ring_;
    Dsp dsp_;
    std::unique_ptr<Output> out_;
    std::unique_ptr<Decoder> dec_;
    std::vector<float> pend_;
    size_t pendOff_ = 0;
    bool fmtSwitchPending_ = false;
    Segment fmtSwitchSeg_;

    std::thread thread_;
    std::mutex mx_;
    std::condition_variable cv_;
    bool quit_ = false;
    Command cmd_;
    QString nextPath_;
    quint64 nextToken_ = 0;

    mutable std::mutex segMx_;
    std::vector<Segment> segs_;
    std::atomic<bool> eofAll_{false};

    QTimer poll_;
    State state_ = Stopped;
    quint64 audible_ = 0;
    int drainTicks_ = 0;
    float volume_ = 1;
};
