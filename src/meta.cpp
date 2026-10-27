#include "meta.h"
#include "tagedit.h"
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QUrl>

extern "C" {
#include <libavformat/avformat.h>
}

QString TrackInfo::displayTitle() const
{
    if (!title.isEmpty()) return title;
    QString base = QFileInfo(path).completeBaseName();
    // "01 - Artist - Title" / "01. Title"
    static const QRegularExpression lead("^\\s*\\d{1,3}\\s*[-._)]\\s*");
    base.remove(lead);
    const int dash = base.lastIndexOf(" - ");
    return dash > 0 && artist.isEmpty() ? base.mid(dash + 3) : base;
}

static QString tag(AVDictionary *a, AVDictionary *b, const char *key)
{
    for (AVDictionary *d : {a, b}) {
        if (const AVDictionaryEntry *e = av_dict_get(d, key, nullptr, 0))
            return QString::fromUtf8(e->value).trimmed();
    }
    return {};
}

TrackInfo readTrackInfo(const QString &path)
{
    TrackInfo t;
    t.path = path;
    t.scanned = true;
    AVFormatContext *fmt = nullptr;
    const QByteArray p = path.toUtf8();
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) return t;
    fmt->max_analyze_duration = AV_TIME_BASE / 2;
    avformat_find_stream_info(fmt, nullptr);
    AVDictionary *sm = nullptr;
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si >= 0) sm = fmt->streams[si]->metadata;
    t.title = tag(fmt->metadata, sm, "title");
    t.artist = tag(fmt->metadata, sm, "artist");
    t.album = tag(fmt->metadata, sm, "album");
    t.albumArtist = tag(fmt->metadata, sm, "album_artist");
    if (t.albumArtist.isEmpty()) t.albumArtist = tag(fmt->metadata, sm, "albumartist");
    t.genre = tag(fmt->metadata, sm, "genre");
    t.date = tag(fmt->metadata, sm, "date").left(4);
    if (t.date.isEmpty()) t.date = tag(fmt->metadata, sm, "WM/Year").left(4);   // FFmpeg leaves ASF's year unmapped
    t.trackNo = tag(fmt->metadata, sm, "track").section('/', 0, 0).toInt();
    t.disc = tag(fmt->metadata, sm, "disc").section('/', 0, 0).toInt();
    for (AVDictionary *d : {fmt->metadata, sm}) {
        if (!t.lyrics.isEmpty()) break;
        if (auto *e = av_dict_get(d, "lyrics", nullptr, AV_DICT_IGNORE_SUFFIX)) t.lyrics = QString::fromUtf8(e->value);
        else if (auto *e2 = av_dict_get(d, "unsyncedlyrics", nullptr, 0)) t.lyrics = QString::fromUtf8(e2->value);
    }
    if (fmt->duration > 0) t.duration = fmt->duration / double(AV_TIME_BASE);
    else if (si >= 0 && fmt->streams[si]->duration > 0)
        t.duration = fmt->streams[si]->duration * av_q2d(fmt->streams[si]->time_base);
    avformat_close_input(&fmt);

    // No tags: "Artist - Title.mp3"
    if (t.title.isEmpty()) {
        QString base = QFileInfo(path).completeBaseName();
        static const QRegularExpression lead("^\\s*\\d{1,3}\\s*[-._)]\\s*");
        base.remove(lead);
        const int dash = base.indexOf(" - ");
        if (dash > 0 && t.artist.isEmpty()) {
            t.artist = base.left(dash).trimmed();
            t.title = base.mid(dash + 3).trimmed();
        }
    }
    applyTagOverride(t);
    return t;
}

static const QStringList kImageExt = {"jpg", "jpeg", "png", "webp", "bmp"};

QImage readCover(const QString &path)
{
    QImage img;
    AVFormatContext *fmt = nullptr;
    const QByteArray p = path.toUtf8();
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams && img.isNull(); ++i) {
            AVStream *st = fmt->streams[i];
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC && st->attached_pic.size > 0)
                img.loadFromData(st->attached_pic.data, st->attached_pic.size);
        }
        avformat_close_input(&fmt);
    }
    if (!img.isNull()) return img;

    QDir dir = QFileInfo(path).dir();
    QStringList pics;
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files))
        if (kImageExt.contains(fi.suffix().toLower())) pics << fi.fileName();
    auto score = [](const QString &n) {
        const QString l = n.toLower();
        for (auto [k, s] : {std::pair{"cover", 0}, {"folder", 1}, {"front", 2}, {"album", 3}, {"art", 4}})
            if (l.contains(k)) return s;
        return 9;
    };
    std::stable_sort(pics.begin(), pics.end(), [&](const QString &a, const QString &b) { return score(a) < score(b); });
    for (const QString &f : pics)
        if (img.load(dir.filePath(f))) break;
    return img;
}

static const QSet<QString> kAudioExt = {
    "mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "mp4", "aac", "alac", "wav", "wv", "ape", "aiff", "aif",
    "aifc", "wma", "mpc", "dsf", "dff", "tta", "mka", "webm", "ac3", "dts", "caf", "spx", "tak", "mp2", "amr",
    "mod", "xm", "it", "s3m", "w64", "au", "snd", "ra", "3gp", "weba",
    // also playable through FFmpeg (and libopenmpt for tracker modules)
    "awb", "dtshd", "rm", "m4r", "gsm", "voc", "axa", "mp1", "mpga", "669", "med", "mo3", "mtm", "stm", "ult", "mptm"};

bool isAudioFile(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.fileName().startsWith("._")) return false;   // macOS AppleDouble junk
    return kAudioExt.contains(fi.suffix().toLower());
}

QStringList expandPaths(const QStringList &paths)
{
    QStringList out;
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QStringList found;
            QDirIterator it(fi.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext()) {
                const QString f = it.next();
                if (isAudioFile(f)) found << f;
            }
            std::sort(found.begin(), found.end(), [&](const QString &a, const QString &b) { return coll.compare(a, b) < 0; });
            out << found;
        } else if (fi.isFile()) {
            const QString suf = fi.suffix().toLower();
            if (suf == "m3u" || suf == "m3u8") {
                QFile f(fi.absoluteFilePath());
                if (f.open(QIODevice::ReadOnly)) {
                    const QDir base = fi.dir();
                    for (const QByteArray &raw : f.readAll().split('\n')) {
                        QString line = QString::fromUtf8(raw).trimmed();
                        if (line.isEmpty() || line.startsWith('#')) continue;
                        if (line.startsWith("file://")) line = QUrl(line).toLocalFile();
                        out << QDir::cleanPath(base.absoluteFilePath(line));
                    }
                }
            } else {
                out << fi.absoluteFilePath();
            }
        }
    }
    return out;
}

QString cacheDir(const QString &sub)
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/sonata/" + sub;
    QDir().mkpath(d);
    return d;
}

QString formatTime(double sec)
{
    if (!(sec >= 0)) sec = 0;
    const int s = int(sec);
    if (s >= 3600) return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
