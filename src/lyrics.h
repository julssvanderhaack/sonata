// Lyrics: .lrc next to the file → embedded tag → disk cache → lrclib.net.
#pragma once
#include "meta.h"
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

struct LyricLine {
    double t = 0;
    QString text;
};

struct Lyrics {
    QVector<LyricLine> lines;
    bool synced = false;
    bool instrumental = false;
    QString source;
    bool networkError = false;
    bool isEmpty() const { return lines.isEmpty() && !instrumental; }
};

Lyrics parseLyrics(const QString &text);   // LRC if it has timestamps, plain otherwise

class LyricsFetcher : public QObject {
    Q_OBJECT
public:
    explicit LyricsFetcher(QObject *parent = nullptr);
    void request(const TrackInfo &t, quint64 token, bool skipCache = false);

signals:
    void searching(quint64 token);
    void ready(quint64 token, const Lyrics &lyrics);

private:
    void fetchOnline(const TrackInfo &t, quint64 token, const QString &cacheFile, int attempt = 0);
    void search(const TrackInfo &t, quint64 token, const QString &cacheFile, int attempt = 0);
    void finish(quint64 token, const QString &cacheFile, const QString &synced, const QString &plain, bool instrumental);
    QNetworkAccessManager *net_;
};
