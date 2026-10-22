// MPRIS2 on the session bus: the Omarchy bar, playerctl and the media keys
// all drive the player through this.
#pragma once
#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QStringList>
#include <QVariantMap>

class MainWindow;

class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ yes)
    Q_PROPERTY(bool CanRaise READ yes)
    Q_PROPERTY(bool CanSetFullscreen READ no)
    Q_PROPERTY(bool Fullscreen READ no)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ schemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimes)
public:
    MprisRoot(QObject *holder, MainWindow *w) : QDBusAbstractAdaptor(holder), w_(w) {}
    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return "Sonata"; }
    QString desktopEntry() const { return "sonata"; }
    QStringList schemes() const { return {"file"}; }
    QStringList mimes() const;
public slots:
    void Raise();
    void Quit();

private:
    MainWindow *w_;
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canControl)
    Q_PROPERTY(bool CanGoPrevious READ canControl)
    Q_PROPERTY(bool CanPlay READ canControl)
    Q_PROPERTY(bool CanPause READ canControl)
    Q_PROPERTY(bool CanSeek READ canControl)
    Q_PROPERTY(bool CanControl READ yes)
public:
    MprisPlayer(QObject *holder, MainWindow *w);
    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &s);
    double rate() const { return 1.0; }
    void setRate(double) {}
    bool shuffle() const;
    void setShuffle(bool on);
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double v);
    qlonglong position() const;
    bool canControl() const;
    bool yes() const { return true; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &track, qlonglong posUs);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong posUs);

private:
    void notify(const QVariantMap &changed);
    MainWindow *w_;
};

void registerMpris(MainWindow *w);
