#include "lyrics.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>

static const char *kUserAgent = "Sonata/1.0 (https://github.com/julssvanderhaack/sonata)";

Lyrics parseLyrics(const QString &text)
{
    Lyrics l;
    static const QRegularExpression stamp("\\[(\\d{1,3}):(\\d{1,2}(?:[.:]\\d{1,3})?)\\]");
    static const QRegularExpression offsetRe("^\\[offset:\\s*([+-]?\\d+)\\]", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression metaRe("^\\[[a-zA-Z#]+:.*\\]$");
    static const QRegularExpression wordStamp("<\\d{1,3}:\\d{1,2}(?:[.:]\\d{1,3})?>");
    double offset = 0;
    QVector<LyricLine> timed, plain;
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (auto m = offsetRe.match(line); m.hasMatch()) {
            offset = m.captured(1).toDouble() / 1000.0;
            continue;
        }
        QVector<double> ts;
        int pos = 0;
        for (;;) {
            auto m = stamp.match(line, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
            if (!m.hasMatch()) break;
            QString sec = m.captured(2);
            sec.replace(':', '.');
            ts << m.captured(1).toInt() * 60 + sec.toDouble();
            pos = m.capturedEnd();
            while (pos < line.size() && line[pos].isSpace()) ++pos;
        }
        if (!ts.isEmpty()) {
            QString body = line.mid(pos).remove(wordStamp).trimmed();
            for (double t : ts) timed.push_back({t, body});
        } else if (!metaRe.match(line).hasMatch()) {
            plain.push_back({0, line});
        }
    }
    if (!timed.isEmpty()) {
        std::stable_sort(timed.begin(), timed.end(), [](auto &a, auto &b) { return a.t < b.t; });
        for (auto &t : timed) t.t -= offset;
        l.lines = timed;
        l.synced = true;
    } else {
        while (!plain.isEmpty() && plain.front().text.isEmpty()) plain.pop_front();
        while (!plain.isEmpty() && plain.back().text.isEmpty()) plain.pop_back();
        l.lines = plain;
    }
    return l;
}

static Lyrics failed()
{
    Lyrics l;
    l.networkError = true;
    return l;
}

LyricsFetcher::LyricsFetcher(QObject *parent) : QObject(parent), net_(new QNetworkAccessManager(this))
{
    net_->setTransferTimeout(12000);
}

static QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void LyricsFetcher::request(const TrackInfo &t, quint64 token, bool skipCache)
{
    // 1. sidecar .lrc / .txt
    const QFileInfo fi(t.path);
    for (const char *ext : {".lrc", ".LRC", ".txt"}) {
        const QString p = fi.dir().filePath(fi.completeBaseName() + ext);
        if (QFileInfo::exists(p)) {
            Lyrics l = parseLyrics(readFile(p));
            if (!l.isEmpty()) {
                l.source = "archivo " + QFileInfo(p).fileName();
                emit ready(token, l);
                return;
            }
        }
    }
    // 2. embedded (prefer it only if synced; otherwise an online synced copy is nicer)
    Lyrics embedded;
    if (!t.lyrics.isEmpty()) {
        embedded = parseLyrics(t.lyrics);
        embedded.source = "etiqueta del archivo";
        if (embedded.synced) {
            emit ready(token, embedded);
            return;
        }
    }
    if (t.title.isEmpty()) {
        emit ready(token, embedded);
        return;
    }
    // 3. cache
    const QByteArray key = (t.displayArtist().toLower() + '\n' + t.title.toLower() + '\n'
                            + QString::number(qRound(t.duration))).toUtf8();
    const QString cacheFile = cacheDir("lyrics") + '/' + QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex() + ".json";
    if (!skipCache) {
        const QJsonObject o = QJsonDocument::fromJson(readFile(cacheFile).toUtf8()).object();
        if (!o.isEmpty()) {
            const bool none = o.value("none").toBool();
            const qint64 age = QDateTime::currentSecsSinceEpoch() - o.value("time").toInteger();
            if (!none || age < 3 * 86400) {
                if (none && !embedded.isEmpty()) {
                    emit ready(token, embedded);
                    return;
                }
                Lyrics l = parseLyrics(o.value("synced").toString().isEmpty() ? o.value("plain").toString() : o.value("synced").toString());
                l.instrumental = o.value("instrumental").toBool();
                if (!l.isEmpty()) l.source = "lrclib.net";
                emit ready(token, l);
                return;
            }
        }
    }
    emit searching(token);
    if (!embedded.isEmpty()) {
        // show the embedded text right away, upgrade if lrclib has timings
        emit ready(token, embedded);
    }
    fetchOnline(t, token, cacheFile);
}

void LyricsFetcher::fetchOnline(const TrackInfo &t, quint64 token, const QString &cacheFile, int attempt)
{
    QUrl url("https://lrclib.net/api/get");
    QUrlQuery q;
    q.addQueryItem("artist_name", t.displayArtist());
    q.addQueryItem("track_name", t.title);
    if (!t.album.isEmpty()) q.addQueryItem("album_name", t.album);
    if (t.duration > 0) q.addQueryItem("duration", QString::number(qRound(t.duration)));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    QNetworkReply *r = net_->get(req);
    connect(r, &QNetworkReply::finished, this, [=, this] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        if (status == 200 && (!o.value("syncedLyrics").toString().isEmpty() || !o.value("plainLyrics").toString().isEmpty()
                              || o.value("instrumental").toBool())) {
            finish(token, cacheFile, o.value("syncedLyrics").toString(), o.value("plainLyrics").toString(),
                   o.value("instrumental").toBool());
        } else if (r->error() == QNetworkReply::NoError || status == 404) {
            search(t, token, cacheFile);
        } else if (attempt < 2 && (status >= 500 || r->error() == QNetworkReply::OperationCanceledError)) {
            // lrclib is sometimes briefly overloaded (503): try again shortly
            QTimer::singleShot(2500 * (attempt + 1), this, [=, this] { fetchOnline(t, token, cacheFile, attempt + 1); });
        } else {
            emit ready(token, failed());   // offline: don't cache the miss
        }
    });
}

void LyricsFetcher::search(const TrackInfo &t, quint64 token, const QString &cacheFile, int attempt)
{
    QUrl url("https://lrclib.net/api/search");
    QUrlQuery q;
    q.addQueryItem("track_name", t.title);
    if (!t.displayArtist().isEmpty()) q.addQueryItem("artist_name", t.displayArtist());
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    QNetworkReply *r = net_->get(req);
    connect(r, &QNetworkReply::finished, this, [=, this] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (attempt < 2 && (status >= 500 || r->error() == QNetworkReply::OperationCanceledError))
                QTimer::singleShot(2500 * (attempt + 1), this, [=, this] { search(t, token, cacheFile, attempt + 1); });
            else emit ready(token, failed());
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
        QJsonObject best;
        double bestScore = 1e9;
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const bool synced = !o.value("syncedLyrics").toString().isEmpty();
            if (!synced && o.value("plainLyrics").toString().isEmpty() && !o.value("instrumental").toBool()) continue;
            double diff = t.duration > 0 ? std::abs(o.value("duration").toDouble() - t.duration) : 0;
            if (diff > 15) continue;                 // a different version of the song
            const double score = diff + (synced ? 0 : 30);
            if (score < bestScore) {
                bestScore = score;
                best = o;
            }
        }
        if (best.isEmpty()) {
            QJsonObject none{{"none", true}, {"time", QDateTime::currentSecsSinceEpoch()}};
            QFile f(cacheFile);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(none).toJson(QJsonDocument::Compact));
            emit ready(token, {});
            return;
        }
        finish(token, cacheFile, best.value("syncedLyrics").toString(), best.value("plainLyrics").toString(),
               best.value("instrumental").toBool());
    });
}

void LyricsFetcher::finish(quint64 token, const QString &cacheFile, const QString &synced, const QString &plain, bool instrumental)
{
    QJsonObject o{{"synced", synced}, {"plain", plain}, {"instrumental", instrumental}, {"time", QDateTime::currentSecsSinceEpoch()}};
    QFile f(cacheFile);
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    Lyrics l = parseLyrics(synced.isEmpty() ? plain : synced);
    l.instrumental = instrumental;
    l.source = "lrclib.net";
    emit ready(token, l);
}
