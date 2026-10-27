// Fixing badly tagged songs: tags are written into the file itself with
// TagLib; files that can't be written (read-only media, formats TagLib
// doesn't know) can keep the correction in Sonata only (tags.json), applied
// on top of what FFmpeg reads.
#pragma once
#include "meta.h"
#include <QDialog>
#include <QMap>

class QLineEdit;

// field → new value; an empty value removes the tag.
// Fields: title artist album albumArtist genre date track disc
using TagChanges = QMap<QString, QString>;

bool writeTags(const QString &path, const TagChanges &c, QString *error);
void setTagOverride(const QString &path, const TagChanges &c);   // Sonata only
void dropTagOverride(const QString &path, const QStringList &fields);   // the file holds them now
void applyTagOverride(TrackInfo &t);                               // thread safe

class TagDialog : public QDialog {
    Q_OBJECT
public:
    TagDialog(const QVector<TrackInfo> &tracks, QWidget *parent = nullptr);
    TagChanges changes() const;   // what to write, per file it's the same set

private:
    struct Field {
        QString key;
        QLineEdit *edit;
        QString shown;      // value in the dialog when it opened
        bool inFile;        // the file really has this tag (not guessed from its name)
    };
    QVector<Field> fields_;
    bool multi_;
    QLineEdit *edit(const QString &key) const;
};
