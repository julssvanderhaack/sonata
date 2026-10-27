// Tags, durations and cover art, read with FFmpeg (same library that plays,
// so anything that plays also shows its tags).
#pragma once
#include <QImage>
#include <QString>
#include <QStringList>

struct TrackInfo {
    QString path, title, artist, album, albumArtist, genre, date, lyrics;
    int trackNo = 0, disc = 0;
    double duration = 0;
    qint64 mtime = 0, size = 0;    // of the file when its tags were read: changed → read them again
    bool scanned = false;

    QString displayTitle() const;
    QString displayArtist() const { return artist.isEmpty() ? albumArtist : artist; }
};

TrackInfo readTrackInfo(const QString &path);
QImage readCover(const QString &path);          // embedded picture, else an image in the folder
bool isAudioFile(const QString &path);
QStringList expandPaths(const QStringList &paths); // folders → their audio files, sorted
QString cacheDir(const QString &sub);
bool fileChanged(const TrackInfo &t);              // stat only: the file differs from when it was read
bool writeFileAtomic(const QString &path, const QByteArray &data);   // never leaves half a file
QString formatTime(double sec);
// Lower case without accents, for search and grouping ('ñ' stays: it is a letter in Spanish).
QString foldText(const QString &s);
