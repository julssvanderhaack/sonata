// The music library: every song under the folders the user picked, grouped
// into albums, artists, folders and genres the way Sonata for Android does.
//
// Kept light: an index in ~/.cache/sonata/library.dat (tags + size + date of
// each file), a rescan that only stats files and reads the tags of new or
// changed ones, and no file watching — it rescans when asked, at start-up and
// when the window comes back after a few minutes.
#pragma once
#include "meta.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <atomic>
#include <memory>

class QThread;

struct Album {
    QString key, title, artist;   // artist empty = unknown
    bool various = false;         // no album artist and several artists
    int year = 0;
    QVector<int> tracks;          // indexes into Library::tracks(), disc and track order
    double duration = 0;
};

struct Artist {
    QString key, name;            // name empty = unknown
    QVector<int> albums;          // newest first
    QVector<int> tracks;          // by album, disc, track
};

struct Genre {
    QString name;
    QVector<int> tracks;
};

struct FolderNode {
    QString path, name;           // name may be "Rock/Metallica" when folders with nothing else are merged
    QVector<int> children;        // indexes into Library::folders()
    QVector<int> tracks;          // songs right in this folder
    int total = 0;                // songs here and below
};

class Library : public QObject {
    Q_OBJECT
public:
    enum AlbumSort { ByTitle, ByArtist, ByYear };

    explicit Library(QObject *parent = nullptr);
    ~Library() override;

    QStringList roots() const { return roots_; }
    void setRoots(const QStringList &folders);   // loads the index, then rescans
    void rescan(bool report = false);
    void rescanIfStale();                       // the window came back
    bool scanning() const { return scanning_; }
    void updateInfo(const QVector<TrackInfo> &edited);   // tags edited in Sonata
    void setAlbumSort(AlbumSort s);
    AlbumSort albumSort() const { return albumSort_; }

    const QVector<TrackInfo> &tracks() const { return tracks_; }
    const QVector<Album> &albums() const { return albums_; }
    const QVector<Artist> &artists() const { return artists_; }
    const QVector<Genre> &genres() const { return genres_; }   // empty when too few songs say theirs
    const QVector<FolderNode> &folders() const { return nodes_; }
    const QVector<int> &folderRoots() const { return rootNodes_; }
    const QVector<int> &songs() const { return songs_; }       // every song: artist, album, disc, track
    int albumOfTrack(int track) const { return trackAlbum_.value(track, -1); }
    int indexOf(const QString &path) const { return byPath_.value(path, -1); }
    QVector<int> folderTracks(int node) const;                 // recursively, in path order
    double duration(const QVector<int> &tracks) const;

signals:
    void changed();
    void progress(int done, int total);                         // reading tags
    void scanned(int added, int changed, int removed, bool report);

private:
    void load();
    void save() const;
    void setTracks(QVector<TrackInfo> tracks);
    void regroup();
    void sortAlbums();

    QStringList roots_;
    QVector<TrackInfo> tracks_;
    QHash<QString, int> byPath_;
    QVector<Album> albums_;
    QVector<Artist> artists_;
    QVector<Genre> genres_;
    QVector<FolderNode> nodes_;
    QVector<int> rootNodes_, songs_;
    QHash<int, int> trackAlbum_;
    AlbumSort albumSort_ = ByArtist;

    bool scanning_ = false, again_ = false, againReport_ = false;
    qint64 lastScan_ = 0;
    QPointer<QThread> thread_;
    std::shared_ptr<std::atomic<bool>> cancel_;
};
