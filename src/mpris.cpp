#include "mpris.h"
#include "mainwindow.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QUrl>

QStringList MprisRoot::mimes() const
{
    return {"audio/mpeg", "audio/flac", "audio/x-flac", "audio/ogg", "audio/opus", "audio/mp4", "audio/aac",
            "audio/x-wav", "audio/x-ape", "audio/x-wavpack", "audio/x-aiff", "audio/x-ms-wma"};
}

void MprisRoot::Raise() { w_->raiseWindow(); }
void MprisRoot::Quit() { w_->close(); }

MprisPlayer::MprisPlayer(QObject *holder, MainWindow *w) : QDBusAbstractAdaptor(holder), w_(w)
{
    setAutoRelaySignals(false);
    connect(w->engine(), &Engine::stateChanged, this, [this] {
        notify({{"PlaybackStatus", playbackStatus()}, {"CanPlay", canControl()}, {"CanSeek", canControl()}});
    });
    connect(w, &MainWindow::metadataChanged, this, [this] {
        notify({{"Metadata", metadata()}, {"CanGoNext", canControl()}, {"CanGoPrevious", canControl()},
                {"CanPlay", canControl()}, {"CanPause", canControl()}, {"CanSeek", canControl()}});
    });
    connect(w, &MainWindow::optionsChanged, this, [this] {
        notify({{"LoopStatus", loopStatus()}, {"Shuffle", shuffle()}, {"Volume", volume()}});
    });
    connect(w, &MainWindow::seeked, this, [this](double s) { emit Seeked(qlonglong(s * 1e6)); });
}

void MprisPlayer::notify(const QVariantMap &changed)
{
    QDBusMessage sig = QDBusMessage::createSignal("/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                                  "PropertiesChanged");
    sig << QString("org.mpris.MediaPlayer2.Player") << changed << QStringList();
    QDBusConnection::sessionBus().send(sig);
}

QString MprisPlayer::playbackStatus() const
{
    switch (w_->engine()->state()) {
    case Engine::Playing: return "Playing";
    case Engine::Paused: return "Paused";
    default: return "Stopped";
    }
}

QString MprisPlayer::loopStatus() const
{
    switch (w_->repeat()) {
    case MainWindow::RepeatAll: return "Playlist";
    case MainWindow::RepeatOne: return "Track";
    default: return "None";
    }
}

void MprisPlayer::setLoopStatus(const QString &s)
{
    w_->setRepeat(s == "Playlist" ? MainWindow::RepeatAll : s == "Track" ? MainWindow::RepeatOne : MainWindow::RepeatOff);
}

bool MprisPlayer::shuffle() const { return w_->shuffle(); }
void MprisPlayer::setShuffle(bool on) { w_->setShuffle(on); }
QVariantMap MprisPlayer::metadata() const { return w_->mprisMetadata(); }
double MprisPlayer::volume() const { return w_->volume(); }
void MprisPlayer::setVolume(double v) { w_->setVolume(v); }
qlonglong MprisPlayer::position() const { return qlonglong(w_->engine()->position() * 1e6); }
bool MprisPlayer::canControl() const { return w_->hasCurrent(); }

void MprisPlayer::Next() { w_->next(); }
void MprisPlayer::Previous() { w_->prev(); }
void MprisPlayer::Pause() { w_->pause(); }
void MprisPlayer::PlayPause() { w_->playPause(); }
void MprisPlayer::Stop() { w_->stop(); }
void MprisPlayer::Play() { w_->play(); }
void MprisPlayer::Seek(qlonglong offsetUs) { w_->seekBy(offsetUs / 1e6); }

void MprisPlayer::SetPosition(const QDBusObjectPath &track, qlonglong posUs)
{
    if (track.path() == w_->mprisMetadata().value("mpris:trackid").value<QDBusObjectPath>().path())
        w_->seekTo(posUs / 1e6);
}

void MprisPlayer::OpenUri(const QString &uri)
{
    const QUrl u(uri);
    w_->addPaths({u.isLocalFile() ? u.toLocalFile() : uri}, true);
}

void registerMpris(MainWindow *w)
{
    auto *holder = new QObject(w);
    new MprisRoot(holder, w);
    new MprisPlayer(holder, w);
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.registerObject("/org/mpris/MediaPlayer2", holder);
    bus.registerService("org.mpris.MediaPlayer2.sonata");
}
