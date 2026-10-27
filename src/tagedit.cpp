#include "tagedit.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

// Sonata field → TagLib's format-neutral property name (ID3v2 TPE2, Vorbis
// ALBUMARTIST, MP4 aART… are all mapped by TagLib itself)
static const QList<std::pair<const char *, const char *>> kKeys = {
    {"title", "TITLE"}, {"artist", "ARTIST"}, {"album", "ALBUM"}, {"albumArtist", "ALBUMARTIST"},
    {"genre", "GENRE"}, {"date", "DATE"},     {"track", "TRACKNUMBER"}, {"disc", "DISCNUMBER"}};

static TagLib::String tl(const QString &s) { return TagLib::String(s.toStdString(), TagLib::String::UTF8); }
static QString qs(const TagLib::String &s) { return QString::fromStdString(s.to8Bit(true)); }

static TagLib::FileRef openRef(const QString &path, bool readStyle = false)
{
    return TagLib::FileRef(QFile::encodeName(path).constData(), readStyle);
}

bool writeTags(const QString &path, const TagChanges &c, QString *error)
{
    TagLib::FileRef f = openRef(path);
    if (f.isNull() || !f.file() || !f.file()->isValid()) {
        if (error) *error = "formato que no se puede etiquetar";
        return false;
    }
    if (f.file()->readOnly()) {
        if (error) *error = "sin permiso de escritura";
        return false;
    }
    TagLib::PropertyMap pm = f.properties();
    for (auto [field, prop] : kKeys) {
        if (!c.contains(field)) continue;
        QString v = c.value(field).trimmed();
        if (v.isEmpty()) {
            pm.erase(prop);
            continue;
        }
        // "3/12": keep the total when only the number is typed
        if ((!qstrcmp(field, "track") || !qstrcmp(field, "disc")) && !v.contains('/') && pm.contains(prop)
            && !pm[prop].isEmpty()) {
            const QString old = qs(pm[prop].front());
            if (old.contains('/')) v += '/' + old.section('/', 1);
        }
        pm.replace(prop, TagLib::StringList(tl(v)));
    }
    f.setProperties(pm);
    if (!f.save()) {
        if (error) *error = "no se ha podido guardar el archivo";
        return false;
    }
    return true;
}

// ─────────────── corrections kept in Sonata only ───────────────

static QMutex overridesLock;
static QHash<QString, QJsonObject> overrides;
static bool overridesLoaded = false;

static QString overridesFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sonata/tags.json";
}

static void loadOverrides()   // lock held
{
    if (overridesLoaded) return;
    overridesLoaded = true;
    QFile f(overridesFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it) overrides.insert(it.key(), it.value().toObject());
}

static void saveOverrides()   // lock held
{
    QJsonObject o;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) o.insert(it.key(), it.value());
    QDir().mkpath(QFileInfo(overridesFile()).path());
    QSaveFile f(overridesFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

void setTagOverride(const QString &path, const TagChanges &c)
{
    QMutexLocker l(&overridesLock);
    loadOverrides();
    QJsonObject &o = overrides[path];
    for (auto it = c.begin(); it != c.end(); ++it) o.insert(it.key(), it.value().trimmed());
    saveOverrides();
}

void dropTagOverride(const QString &path, const QStringList &fields)
{
    QMutexLocker l(&overridesLock);
    loadOverrides();
    auto it = overrides.find(path);
    if (it == overrides.end()) return;
    for (const QString &f : fields) it->remove(f);
    if (it->isEmpty()) overrides.erase(it);
    saveOverrides();
}

void applyTagOverride(TrackInfo &t)
{
    QJsonObject o;
    {
        QMutexLocker l(&overridesLock);
        loadOverrides();
        o = overrides.value(t.path);
    }
    if (o.isEmpty()) return;
    auto str = [&](const char *k, QString &dst) {
        if (o.contains(k)) dst = o.value(k).toString();
    };
    str("title", t.title);
    str("artist", t.artist);
    str("album", t.album);
    str("albumArtist", t.albumArtist);
    str("genre", t.genre);
    str("date", t.date);
    if (o.contains("track")) t.trackNo = o.value("track").toString().section('/', 0, 0).toInt();
    if (o.contains("disc")) t.disc = o.value("disc").toString().section('/', 0, 0).toInt();
}

// ─────────────── dialog ───────────────

static QString valueOf(const TrackInfo &t, const QString &key)
{
    if (key == "title") return t.title.isEmpty() ? t.displayTitle() : t.title;
    if (key == "artist") return t.artist;
    if (key == "album") return t.album;
    if (key == "albumArtist") return t.albumArtist;
    if (key == "genre") return t.genre;
    if (key == "date") return t.date;
    if (key == "track") return t.trackNo > 0 ? QString::number(t.trackNo) : QString();
    if (key == "disc") return t.disc > 0 ? QString::number(t.disc) : QString();
    return {};
}

TagDialog::TagDialog(const QVector<TrackInfo> &tracks, QWidget *parent) : QDialog(parent), multi_(tracks.size() > 1)
{
    setWindowTitle(multi_ ? QString("Editar etiquetas de %1 canciones").arg(tracks.size()) : "Editar etiquetas");
    setMinimumWidth(460);
    auto *v = new QVBoxLayout(this);

    auto *caption = new QLabel;
    caption->setProperty("role", "caption");
    caption->setWordWrap(true);
    caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (multi_)
        caption->setText("Solo se cambian los campos que edites; el resto se queda como está en cada archivo. "
                         "El título y la pista se editan canción a canción.");
    else
        caption->setText(QFileInfo(tracks.front().path).fileName());
    v->addWidget(caption);

    // which tags the single file really has (Sonata guesses title/artist from the file name otherwise)
    TagLib::PropertyMap inFile;
    if (!multi_) {
        TagLib::FileRef f = openRef(tracks.front().path);
        if (!f.isNull() && f.file()) inFile = f.properties();
    }

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const QList<std::pair<QString, QString>> labels = {
        {"title", "Título"}, {"artist", "Artista"}, {"album", "Álbum"}, {"albumArtist", "Artista del álbum"},
        {"genre", "Género"}, {"date", "Año"},       {"track", "Pista"}, {"disc", "Disco"}};
    for (int i = 0; i < labels.size(); ++i) {
        const auto &[key, label] = labels[i];
        if (multi_ && (key == "title" || key == "track")) continue;   // never one title / number for a whole album
        auto *e = new QLineEdit;
        e->setClearButtonEnabled(true);
        QString shown;
        bool same = true;
        for (int j = 0; j < tracks.size(); ++j) {
            const QString val = valueOf(tracks[j], key);
            if (j == 0) shown = val;
            else if (val != shown) same = false;
        }
        if (same) e->setText(shown);
        else {
            shown.clear();
            e->setPlaceholderText("(varios valores)");
        }
        if (key == "track" || key == "disc") e->setValidator(new QRegularExpressionValidator(QRegularExpression("\\d{0,4}(/\\d{0,4})?"), e));
        const char *prop = kKeys[i].second;
        fields_.push_back({key, e, e->text(), !multi_ && inFile.contains(prop) && !inFile[prop].isEmpty()});
        form->addRow(label, e);
    }
    v->addLayout(form);

    if (!multi_) {
        auto *tools = new QHBoxLayout;
        auto mk = [&](const QString &txt, const QString &tip) {
            auto *b = new QToolButton;
            b->setText(txt);
            b->setToolTip(tip);
            b->setProperty("role", "text");
            b->setCursor(Qt::PointingHandCursor);
            tools->addWidget(b);
            return b;
        };
        auto *swap = mk("Intercambiar título y artista", "Para canciones con el título y el artista al revés");
        auto *fromName = mk("Sacar del nombre del archivo", "«01 - Artista - Título.mp3» → pista, artista y título");
        tools->addStretch(1);
        v->addLayout(tools);
        auto set = [this](const QString &key, const QString &val) {
            QLineEdit *e = edit(key);
            e->setText(val);
            e->setModified(true);
        };
        connect(swap, &QToolButton::clicked, this, [=, this] {
            const QString t = edit("title")->text(), a = edit("artist")->text();
            set("title", a);
            set("artist", t);
        });
        const QString base = QFileInfo(tracks.front().path).completeBaseName();
        connect(fromName, &QToolButton::clicked, this, [=, this] {
            static const QRegularExpression lead("^\\s*(\\d{1,3})\\s*[-._)]\\s*");
            QString b = base;
            if (auto m = lead.match(b); m.hasMatch()) {
                set("track", QString::number(m.captured(1).toInt()));
                b = b.mid(m.capturedEnd());
            }
            const QStringList parts = b.split(" - ");
            if (parts.size() >= 2) {
                set("artist", parts.front().trimmed());
                set("title", parts.back().trimmed());
            } else {
                set("title", b.trimmed());
            }
        });
    }

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Save)->setText("Guardar");
    bb->button(QDialogButtonBox::Cancel)->setText("Cancelar");
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    edit(multi_ ? "artist" : "title")->setFocus();
}

QLineEdit *TagDialog::edit(const QString &key) const
{
    for (const Field &f : fields_)
        if (f.key == key) return f.edit;
    return nullptr;
}

TagChanges TagDialog::changes() const
{
    TagChanges c;
    for (const Field &f : fields_) {
        const QString now = f.edit->text().trimmed();
        // edited, or a guess (from the file name) the file doesn't hold yet
        if ((f.edit->isModified() && now != f.shown) || (!multi_ && !f.inFile && !now.isEmpty())) c.insert(f.key, now);
    }
    return c;
}
