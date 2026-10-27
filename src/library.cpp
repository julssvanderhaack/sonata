#include "library.h"
#include <QCollator>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>
#include <algorithm>
#include <functional>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <QMap>

static constexpr quint32 kMagic = 0x534e4c42;   // "SNLB"
static constexpr quint32 kVersion = 1;

static QString indexFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/sonata/library.dat";
}

static QString dirOf(const QString &path) { return path.left(path.lastIndexOf('/')); }
static QString nameOf(const QString &dir) { return dir.mid(dir.lastIndexOf('/') + 1); }
static QString albumOrFolder(const TrackInfo &t) { return t.album.isEmpty() ? nameOf(dirOf(t.path)) : t.album; }
static int yearOf(const TrackInfo &t) { return t.date.left(4).toInt(); }

Library::Library(QObject *parent) : QObject(parent) {}

Library::~Library()
{
    if (cancel_) cancel_->store(true);
    if (thread_) thread_->wait();
}

void Library::setRoots(const QStringList &folders)
{
    QStringList clean;
    for (const QString &f : folders)
        if (!f.isEmpty() && !clean.contains(QDir::cleanPath(f))) clean << QDir::cleanPath(f);
    if (clean == roots_ && !tracks_.isEmpty()) return;
    const bool first = roots_.isEmpty() && tracks_.isEmpty();
    roots_ = clean;
    if (first) load();
    else setTracks({});   // other folders: start over (the scan reuses nothing)
    rescan();
}

void Library::load()
{
    QFile f(indexFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    QDataStream in(&f);
    quint32 magic = 0, version = 0;
    QStringList folders;
    in >> magic >> version >> folders;
    if (magic != kMagic || version != kVersion || folders != roots_) return;
    qint32 n = 0;
    in >> n;
    QVector<TrackInfo> tracks;
    tracks.reserve(n);
    for (int i = 0; i < n && in.status() == QDataStream::Ok; ++i) {
        TrackInfo t;
        qint32 no = 0, disc = 0;
        in >> t.path >> t.title >> t.artist >> t.album >> t.albumArtist >> t.genre >> t.date >> no >> disc >> t.duration >> t.mtime >> t.size;
        t.trackNo = no;
        t.disc = disc;
        t.scanned = true;
        tracks << t;
    }
    if (in.status() == QDataStream::Ok) setTracks(std::move(tracks));
}

void Library::save() const
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << kMagic << kVersion << roots_ << qint32(tracks_.size());
    for (const TrackInfo &t : tracks_)
        out << t.path << t.title << t.artist << t.album << t.albumArtist << t.genre << t.date << qint32(t.trackNo)
            << qint32(t.disc) << t.duration << t.mtime << t.size;
    writeFileAtomic(indexFile(), data);
}

void Library::rescanIfStale()
{
    if (!roots_.isEmpty() && QDateTime::currentMSecsSinceEpoch() - lastScan_ > 2 * 60 * 1000) rescan();
}

// A thread walks the folders and compares size and date with the index; only
// new or changed files are opened, two at a time so playback never notices.
void Library::rescan(bool report)
{
    if (roots_.isEmpty()) return;
    if (scanning_) {
        again_ = true;
        againReport_ |= report;
        return;
    }
    scanning_ = true;
    lastScan_ = QDateTime::currentMSecsSinceEpoch();
    emit progress(0, 0);
    QHash<QString, TrackInfo> old;
    for (const TrackInfo &t : tracks_) old.insert(t.path, t);
    const QStringList roots = roots_;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = cancel_;
    QPointer<Library> self(this);
    thread_ = QThread::create([self, roots, old, report, cancel] {
        QVector<TrackInfo> keep;
        QStringList toRead;
        int changed = 0;
        QSet<QString> seen;
        for (const QString &root : roots) {
            QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && !*cancel) {
                const QFileInfo fi = it.nextFileInfo();
                const QString p = fi.absoluteFilePath();
                if (!isAudioFile(p) || seen.contains(p)) continue;
                seen.insert(p);
                const auto o = old.constFind(p);
                if (o != old.cend() && o->mtime == fi.lastModified().toMSecsSinceEpoch() && o->size == fi.size()) {
                    keep << *o;
                } else {
                    toRead << p;
                    if (o != old.cend()) ++changed;
                }
            }
        }
        const int removed = int(old.size()) - int(keep.size()) - changed;

        QThreadPool pool;
        pool.setMaxThreadCount(2);
        QVector<TrackInfo> read;
        const int total = int(toRead.size());
        const bool firstScan = old.isEmpty();
        for (int i = 0; i < total && !*cancel; i += 48) {
            const QStringList chunk = toRead.mid(i, 48);
            read += QtConcurrent::blockingMapped<QList<TrackInfo>>(&pool, chunk, [](const QString &p) {
                TrackInfo t = readTrackInfo(p);
                t.lyrics.clear();
                return t;
            });
            const int done = i + int(chunk.size());
            // the first time, show what is there while the rest is read (every ~500 songs)
            QVector<TrackInfo> partial;
            if (firstScan && (done / 480 != (done - int(chunk.size())) / 480)) partial = keep + read;
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, done, total, partial, roots] {
                if (!self) return;
                if (!partial.isEmpty() && self->roots_ == roots) self->setTracks(partial);
                emit self->progress(done, total);
            });
        }
        if (*cancel) return;
        pool.waitForDone();
#ifdef __GLIBC__
        malloc_trim(0);   // what the tag readers allocated goes back to the system, not idle in their arenas
#endif
        QVector<TrackInfo> all = keep + read;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, all, total, changed, removed, report, roots] {
            if (!self) return;
            self->scanning_ = false;
            if (self->roots_ != roots) {   // the folders changed meanwhile: this scan is void
                self->again_ = false;
                self->rescan(report);
                return;
            }
            if (total || removed || self->tracks_.size() != all.size()) {
                self->setTracks(all);
                self->save();
            }
            emit self->scanned(total - changed, changed, std::max(0, removed), report);
            if (self->again_) {
                const bool rep = self->againReport_;
                self->again_ = self->againReport_ = false;
                self->rescan(rep);
            }
        });
    });
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    thread_->start(QThread::LowPriority);
}

void Library::updateInfo(const QVector<TrackInfo> &edited)
{
    bool any = false;
    for (const TrackInfo &t : edited) {
        const int i = indexOf(t.path);
        if (i < 0) continue;
        tracks_[i] = t;
        tracks_[i].lyrics.clear();
        any = true;
    }
    if (!any) return;
    regroup();
    save();
    emit changed();
}

void Library::setAlbumSort(AlbumSort s)
{
    if (s == albumSort_) return;
    albumSort_ = s;
    regroup();   // indexes into albums() change
    emit changed();
}

void Library::setTracks(QVector<TrackInfo> tracks)
{
    tracks_ = std::move(tracks);
    byPath_.clear();
    byPath_.reserve(tracks_.size());
    for (int i = 0; i < tracks_.size(); ++i) byPath_.insert(tracks_[i].path, i);
    regroup();
    emit changed();
}

QVector<int> Library::folderTracks(int node) const
{
    QVector<int> out;
    QVector<int> todo{node};
    while (!todo.isEmpty()) {
        const FolderNode &n = nodes_[todo.takeLast()];
        out += n.tracks;
        todo += n.children;
    }
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&](int a, int b) { return coll.compare(tracks_[a].path, tracks_[b].path) < 0; });
    return out;
}

double Library::duration(const QVector<int> &tracks) const
{
    double d = 0;
    for (int i : tracks) d += tracks_[i].duration;
    return d;
}

// Same rules as Sonata for Android (Grouping.kt).
void Library::regroup()
{
    QCollator coll(QLocale("es_ES"));
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto less = [&](const QString &a, const QString &b) {
        if (a.isEmpty() != b.isEmpty()) return b.isEmpty();   // unknown last
        return coll.compare(a, b) < 0;
    };
    auto byDiscTrack = [&](int a, int b) {
        const TrackInfo &x = tracks_[a], &y = tracks_[b];
        if (x.disc != y.disc) return x.disc < y.disc;
        if (x.trackNo != y.trackNo) return x.trackNo < y.trackNo;
        return coll.compare(x.displayTitle(), y.displayTitle()) < 0;
    };

    // Albums: first an album name inside one folder, so an album where only some
    // songs carry an album artist stays whole; then folders that share name and
    // album artist (CD1/, CD2/) merge. Without an album artist the folder tells
    // same-named albums apart ("Greatest Hits").
    QHash<QString, QVector<int>> byFolder;
    QStringList folderOrder;
    for (int i = 0; i < tracks_.size(); ++i) {
        const QString k = foldText(albumOrFolder(tracks_[i])) + QChar(0) + dirOf(tracks_[i].path);
        auto it = byFolder.find(k);
        if (it == byFolder.end()) {
            folderOrder << k;
            it = byFolder.insert(k, {});
        }
        it->push_back(i);
    }
    QHash<QString, QVector<int>> groups;
    QStringList groupOrder;
    for (const QString &fk : folderOrder) {
        const QVector<int> &ts = byFolder[fk];
        QString aa;
        for (int i : ts)
            if (!tracks_[i].albumArtist.isEmpty()) {
                aa = tracks_[i].albumArtist;
                break;
            }
        const QString key = aa.isEmpty() ? fk : foldText(albumOrFolder(tracks_[ts.front()])) + QChar(0) + foldText(aa);
        if (!groups.contains(key)) groupOrder << key;
        groups[key] += ts;
    }
    albums_.clear();
    albums_.reserve(groupOrder.size());
    for (const QString &key : groupOrder) {
        QVector<int> ts = groups[key];
        std::sort(ts.begin(), ts.end(), byDiscTrack);
        Album a;
        a.key = key;
        a.title = albumOrFolder(tracks_[ts.front()]);
        QString aa;
        QSet<QString> artists;
        QString oneArtist;
        for (int i : ts) {
            const TrackInfo &t = tracks_[i];
            if (aa.isEmpty()) aa = t.albumArtist;
            if (!t.artist.isEmpty() && !artists.contains(foldText(t.artist))) {
                artists.insert(foldText(t.artist));
                oneArtist = t.artist;
            }
            a.year = std::max(a.year, yearOf(t));
            a.duration += t.duration;
        }
        a.artist = !aa.isEmpty() ? aa : artists.size() == 1 ? oneArtist : QString();
        a.various = aa.isEmpty() && artists.size() > 1;
        a.tracks = ts;
        albums_ << a;
    }
    sortAlbums();
    trackAlbum_.clear();
    for (int a = 0; a < albums_.size(); ++a)
        for (int t : albums_[a].tracks) trackAlbum_.insert(t, a);

    // Artists: a song takes its album's album artist, so tagged and untagged
    // songs of one album stay together.
    QHash<QString, int> artistIdx;
    artists_.clear();
    for (int a = 0; a < albums_.size(); ++a) {
        const Album &al = albums_[a];
        QString aaOfAlbum;
        for (int t : al.tracks)
            if (!tracks_[t].albumArtist.isEmpty()) {
                aaOfAlbum = tracks_[t].albumArtist;
                break;
            }
        for (int t : al.tracks) {
            const QString name = !aaOfAlbum.isEmpty() ? aaOfAlbum : tracks_[t].artist;
            const QString key = foldText(name);
            auto it = artistIdx.find(key);
            if (it == artistIdx.end()) {
                it = artistIdx.insert(key, int(artists_.size()));
                artists_.push_back({key, name, {}, {}});
            }
            Artist &ar = artists_[*it];
            if (!ar.albums.contains(a)) ar.albums << a;
            ar.tracks << t;
        }
    }
    for (Artist &ar : artists_) {
        std::sort(ar.albums.begin(), ar.albums.end(), [&](int x, int y) {
            if (albums_[x].year != albums_[y].year) return albums_[x].year > albums_[y].year;
            return less(albums_[x].title, albums_[y].title);
        });
        const QSet<int> mine(ar.tracks.begin(), ar.tracks.end());
        QVector<int> ts;
        for (int a : ar.albums)
            for (int t : albums_[a].tracks)
                if (mine.contains(t)) ts << t;
        ar.tracks = ts;
    }
    std::sort(artists_.begin(), artists_.end(), [&](const Artist &a, const Artist &b) { return less(a.name, b.name); });

    // All songs: artist, album, disc, track
    songs_.clear();
    for (const Artist &ar : artists_) songs_ += ar.tracks;

    // Genres, only when a fair share of the library has one
    genres_.clear();
    int tagged = 0;
    for (const TrackInfo &t : tracks_) tagged += !t.genre.isEmpty();
    if (!tracks_.isEmpty() && tagged * 10 >= tracks_.size()) {
        QHash<QString, int> gi;
        for (int i : songs_) {
            const QString &g = tracks_[i].genre;
            if (g.isEmpty()) continue;
            const QString k = foldText(g);
            auto it = gi.find(k);
            if (it == gi.end()) {
                it = gi.insert(k, int(genres_.size()));
                genres_.push_back({g, {}});
            }
            genres_[*it].tracks << i;
        }
        std::sort(genres_.begin(), genres_.end(), [&](const Genre &a, const Genre &b) { return less(a.name, b.name); });
    }

    // Folder tree under each library folder; a folder with no songs of its own
    // and a single subfolder is merged into it ("Rock/Metallica").
    struct Raw {
        QString path;
        QMap<QString, int> children;
        QVector<int> tracks;
    };
    QVector<Raw> raw;
    QHash<QString, int> rawIdx;
    QVector<int> rawRoots;
    for (const QString &r : roots_) {
        rawIdx.insert(r, int(raw.size()));
        rawRoots << int(raw.size());
        raw.push_back({r, {}, {}});
    }
    for (int i = 0; i < tracks_.size(); ++i) {
        const QString dir = dirOf(tracks_[i].path);
        QString root;
        for (const QString &r : roots_)
            if ((dir == r || dir.startsWith(r + '/')) && r.size() > root.size()) root = r;
        if (root.isEmpty()) continue;
        int node = rawIdx[root];
        QString path = root;
        for (const QString &seg : dir.mid(root.size()).split('/', Qt::SkipEmptyParts)) {
            path += '/' + seg;
            auto it = rawIdx.find(path);
            if (it == rawIdx.end()) {
                it = rawIdx.insert(path, int(raw.size()));
                raw[node].children.insert(seg, int(raw.size()));
                raw.push_back({path, {}, {}});
            }
            node = *it;
        }
        raw[node].tracks << i;
    }
    nodes_.clear();
    rootNodes_.clear();
    std::function<int(int, const QString &)> freeze = [&](int r, const QString &name) -> int {
        int cur = r;
        QString shown = name;
        while (raw[cur].tracks.isEmpty() && raw[cur].children.size() == 1) {
            cur = raw[cur].children.first();
            shown += '/' + nameOf(raw[cur].path);
        }
        const int me = int(nodes_.size());
        nodes_.push_back({raw[cur].path, shown, {}, raw[cur].tracks, int(raw[cur].tracks.size())});
        QVector<int> kids;
        for (auto it = raw[cur].children.cbegin(); it != raw[cur].children.cend(); ++it) kids << freeze(it.value(), it.key());
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return less(nodes_[a].name, nodes_[b].name); });
        for (int k : kids) nodes_[me].total += nodes_[k].total;
        nodes_[me].children = kids;
        return me;
    };
    for (int r : rawRoots) {
        if (raw[r].tracks.isEmpty() && raw[r].children.isEmpty()) continue;
        rootNodes_ << freeze(r, nameOf(raw[r].path));
    }
}

void Library::sortAlbums()
{
    QCollator coll(QLocale("es_ES"));
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto cmp = [&](const QString &a, const QString &b) {
        if (a.isEmpty() != b.isEmpty()) return a.isEmpty() ? 1 : -1;
        return coll.compare(a, b);
    };
    std::stable_sort(albums_.begin(), albums_.end(), [&](const Album &a, const Album &b) {
        switch (albumSort_) {
        case ByArtist:
            if (int c = cmp(a.artist, b.artist)) return c < 0;
            if (a.year != b.year) return a.year < b.year;
            return cmp(a.title, b.title) < 0;
        case ByYear:
            if (a.year != b.year) return a.year > b.year;
            return cmp(a.title, b.title) < 0;
        default:
            return cmp(a.title, b.title) < 0;
        }
    });
}
