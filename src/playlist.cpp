#include "playlist.h"
#include "theme.h"
#include "widgets.h"
#include <QCollator>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QMimeData>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <atomic>

static const char *kRowsMime = "application/x-sonata-rows";

quint64 Playlist::newUid()
{
    static std::atomic<quint64> n{1};
    return n++;
}

Playlist::Playlist(const QString &name, QObject *parent) : QAbstractTableModel(parent), name_(name)
{
    id_ = QString::number(QDateTime::currentMSecsSinceEpoch(), 36) + QString::number(newUid(), 36);
}

void Playlist::setName(const QString &n)
{
    name_ = n;
    emit changed();
}

int Playlist::rowOf(quint64 uid) const
{
    for (int i = 0; i < items_.size(); ++i)
        if (items_[i].uid == uid) return i;
    return -1;
}

const Item *Playlist::find(quint64 uid) const
{
    const int r = rowOf(uid);
    return r < 0 ? nullptr : &items_[r];
}

void Playlist::insertPaths(const QStringList &paths, int row)
{
    if (paths.isEmpty()) return;
    if (row < 0 || row > items_.size()) row = items_.size();
    beginInsertRows({}, row, row + paths.size() - 1);
    QVector<Item> add;
    add.reserve(paths.size());
    for (const QString &p : paths) {
        Item it;
        it.uid = newUid();
        it.info.path = p;
        add << it;
    }
    items_.insert(row, add.size(), Item{});
    std::copy(add.begin(), add.end(), items_.begin() + row);
    endInsertRows();
    scan(row, row + paths.size());
    emit changed();
}

// Tags are read on the global pool (one task per chunk of files) and handed
// back to the GUI thread by uid, so reordering meanwhile is harmless.
void Playlist::scan(int from, int to)
{
    QPointer<Playlist> self(this);
    constexpr int chunk = 24;
    for (int a = from; a < to; a += chunk) {
        QVector<QPair<quint64, QString>> job;
        for (int i = a; i < std::min(to, a + chunk); ++i)
            if (!items_[i].info.scanned) job.push_back({items_[i].uid, items_[i].info.path});
        if (job.isEmpty()) continue;
        QThreadPool::globalInstance()->start([self, job] {
            QVector<QPair<quint64, TrackInfo>> res;
            for (auto &[uid, path] : job) res.push_back({uid, readTrackInfo(path)});
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, res]() mutable {
                if (!self) return;
                int lo = INT_MAX, hi = -1;
                for (auto &[uid, info] : res) {
                    const int r = self->rowOf(uid);
                    if (r < 0) continue;
                    info.lyrics.clear();             // re-read when played; keeps memory small
                    self->items_[r].info = info;
                    lo = std::min(lo, r);
                    hi = std::max(hi, r);
                }
                if (hi >= 0) emit self->dataChanged(self->index(lo, 0), self->index(hi, ColCount - 1));
                emit self->scanned();
                emit self->changed();
            });
        });
    }
}

void Playlist::removeRowsList(QList<int> rows)
{
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int r : rows) {
        if (r < 0 || r >= items_.size()) continue;
        beginRemoveRows({}, r, r);
        items_.remove(r);
        endRemoveRows();
    }
    emit changed();
}

void Playlist::clear()
{
    beginResetModel();
    items_.clear();
    endResetModel();
    emit changed();
}

void Playlist::setFailed(quint64 uid, bool f)
{
    const int r = rowOf(uid);
    if (r < 0) return;
    items_[r].failed = f;
    emit dataChanged(index(r, 0), index(r, ColCount - 1));
}

void Playlist::setPlaying(quint64 uid)
{
    const int old = rowOf(playing_);
    playing_ = uid;
    const int now = rowOf(uid);
    if (old >= 0) emit dataChanged(index(old, 0), index(old, ColCount - 1));
    if (now >= 0) emit dataChanged(index(now, 0), index(now, ColCount - 1));
}

void Playlist::updateInfo(const QString &path, const TrackInfo &info)
{
    for (int r = 0; r < items_.size(); ++r) {
        if (items_[r].info.path != path) continue;
        items_[r].info = info;
        items_[r].info.lyrics.clear();
        emit dataChanged(index(r, 0), index(r, ColCount - 1));
    }
    emit changed();
}

void Playlist::sortBy(int col)
{
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    auto key = [&](const Item &i) -> QString {
        const TrackInfo &t = i.info;
        switch (col) {
        case ColTitle: return t.displayTitle();
        case ColArtist: return t.displayArtist() + '\x1f' + t.album + '\x1f' + QString::number(t.disc * 1000 + t.trackNo).rightJustified(6, '0');
        case ColAlbum: return t.album + '\x1f' + QString::number(t.disc * 1000 + t.trackNo).rightJustified(6, '0');
        case ColTime: return QString::number(int(t.duration)).rightJustified(8, '0');
        default: return t.path;
        }
    };
    beginResetModel();
    std::stable_sort(items_.begin(), items_.end(), [&](const Item &a, const Item &b) { return coll.compare(key(a), key(b)) < 0; });
    endResetModel();
    emit changed();
}

void Playlist::removeDuplicates()
{
    QSet<QString> seen;
    QList<int> dup;
    for (int i = 0; i < items_.size(); ++i)
        if (seen.contains(items_[i].info.path)) dup << i;
        else seen.insert(items_[i].info.path);
    removeRowsList(dup);
}

double Playlist::totalDuration() const
{
    double s = 0;
    for (const Item &i : items_) s += i.info.duration;
    return s;
}

QVariant Playlist::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= items_.size()) return {};
    const Item &it = items_[idx.row()];
    const TrackInfo &t = it.info;
    const Theme &th = Theme::get();
    const bool playing = it.uid == playing_ && playing_;
    switch (role) {
    case Qt::DisplayRole:
        switch (idx.column()) {
        case ColState: return playing ? Glyph::Play : QVariant();
        case ColNo: return t.trackNo > 0 ? QVariant(t.trackNo) : QVariant(idx.row() + 1);
        case ColTitle: return t.displayTitle();
        case ColArtist: return t.displayArtist();
        case ColAlbum: return t.album;
        case ColTime: return t.duration > 0 ? formatTime(t.duration) : QString();
        }
        break;
    case Qt::ToolTipRole: return t.path;
    case Qt::ForegroundRole:
        if (it.failed) return QColor("#e06c75");
        if (playing) return th.accent;
        if (idx.column() == ColNo || idx.column() == ColTime || idx.column() == ColAlbum) return th.fgDim;
        break;
    case Qt::FontRole:
        if (idx.column() == ColState) return iconFont(12);
        if (playing && idx.column() == ColTitle) {
            QFont f;
            f.setPixelSize(12);   // the list's font-size in theme.cpp
            f.setBold(true);
            return f;
        }
        break;
    case Qt::TextAlignmentRole:
        if (idx.column() == ColNo || idx.column() == ColTime) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (idx.column() == ColState) return int(Qt::AlignCenter);
        break;
    }
    return {};
}

QVariant Playlist::headerData(int s, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal) return {};
    if (role == Qt::DisplayRole) {
        switch (s) {
        case ColNo: return "#";
        case ColTitle: return "Título";
        case ColArtist: return "Artista";
        case ColAlbum: return "Álbum";
        case ColTime: return "Duración";
        }
    }
    if (role == Qt::TextAlignmentRole && (s == ColNo || s == ColTime)) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
    return {};
}

Qt::ItemFlags Playlist::flags(const QModelIndex &idx) const
{
    Qt::ItemFlags f = Qt::ItemIsDropEnabled;
    if (idx.isValid()) f |= Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    return f;
}

QStringList Playlist::mimeTypes() const
{
    return {kRowsMime, "text/uri-list"};
}

QMimeData *Playlist::mimeData(const QModelIndexList &idx) const
{
    auto *m = new QMimeData;
    QSet<int> rows;
    for (const QModelIndex &i : idx) rows.insert(i.row());
    QList<int> sorted(rows.begin(), rows.end());
    std::sort(sorted.begin(), sorted.end());
    QByteArray enc;
    QList<QUrl> urls;
    for (int r : sorted) {
        enc += QByteArray::number(items_[r].uid) + ',';
        urls << QUrl::fromLocalFile(items_[r].info.path);
    }
    m->setData(kRowsMime, enc);
    m->setUrls(urls);
    return m;
}

bool Playlist::dropMimeData(const QMimeData *m, Qt::DropAction, int row, int, const QModelIndex &parent)
{
    if (row < 0) row = parent.isValid() ? parent.row() : items_.size();
    if (m->hasFormat(kRowsMime)) {
        QVector<Item> moving;
        for (const QByteArray &s : m->data(kRowsMime).split(',')) {
            if (s.isEmpty()) continue;
            const int r = rowOf(s.toULongLong());
            if (r >= 0) moving << items_[r];
        }
        if (!moving.isEmpty()) {
            // Dropped from this very list: move. From another tab: copy with fresh uids.
            const bool local = rowOf(moving.front().uid) >= 0 && m->property("sonataSource").isNull();
            beginResetModel();
            if (local) {
                int before = 0;
                for (const Item &it : moving)
                    if (rowOf(it.uid) < row) ++before;
                QSet<quint64> ids;
                for (const Item &it : moving) ids.insert(it.uid);
                items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item &i) { return ids.contains(i.uid); }), items_.end());
                row -= before;
            }
            row = std::clamp(row, 0, int(items_.size()));
            for (int i = 0; i < moving.size(); ++i) items_.insert(row + i, moving[i]);
            endResetModel();
            emit changed();
            return true;
        }
    }
    if (m->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : m->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        paths = expandPaths(paths);
        QStringList audio;
        for (const QString &p : paths)
            if (isAudioFile(p)) audio << p;
        insertPaths(audio, row);
        return !audio.isEmpty();
    }
    return false;
}

QJsonObject Playlist::toJson() const
{
    QJsonArray arr;
    for (const Item &i : items_) {
        const TrackInfo &t = i.info;
        QJsonObject o{{"p", t.path}};
        if (t.scanned) {
            o["t"] = t.title;
            o["a"] = t.artist;
            o["al"] = t.album;
            o["aa"] = t.albumArtist;
            o["n"] = t.trackNo;
            o["dn"] = t.disc;
            o["d"] = t.duration;
            o["y"] = t.date;
        }
        arr << o;
    }
    return {{"name", name_}, {"id", id_}, {"items", arr}};
}

Playlist *Playlist::fromJson(const QJsonObject &o, QObject *parent)
{
    auto *pl = new Playlist(o.value("name").toString("Lista"), parent);
    if (o.contains("id")) pl->id_ = o.value("id").toString();
    for (const QJsonValue &v : o.value("items").toArray()) {
        const QJsonObject j = v.toObject();
        Item it;
        it.uid = newUid();
        it.info.path = j.value("p").toString();
        if (j.contains("d")) {
            it.info.scanned = true;
            it.info.title = j.value("t").toString();
            it.info.artist = j.value("a").toString();
            it.info.album = j.value("al").toString();
            it.info.albumArtist = j.value("aa").toString();
            it.info.trackNo = j.value("n").toInt();
            it.info.disc = j.value("dn").toInt();
            it.info.duration = j.value("d").toDouble();
            it.info.date = j.value("y").toString();
        }
        pl->items_ << it;
    }
    pl->scan(0, pl->items_.size());
    return pl;
}

bool Playlist::exportM3u(const QString &file) const
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write("#EXTM3U\n");
    for (const Item &i : items_) {
        const TrackInfo &t = i.info;
        f.write(QString("#EXTINF:%1,%2 - %3\n").arg(int(t.duration)).arg(t.displayArtist(), t.displayTitle()).toUtf8());
        f.write(t.path.toUtf8() + '\n');
    }
    return true;
}
