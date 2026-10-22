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
    bool scanned = false;

    QString displayTitle() const;
    QString displayArtist() const { return artist.isEmpty() ? albumArtist : artist; }
};

TrackInfo readTrackInfo(const QString &path);
QImage readCover(const QString &path);          // embedded picture, else an image in the folder
bool isAudioFile(const QString &path);
QStringList expandPaths(const QStringList &paths); // folders → their audio files, sorted
QString cacheDir(const QString &sub);
QString formatTime(double sec);
