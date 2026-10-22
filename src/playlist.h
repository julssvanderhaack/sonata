// One playlist (a tab). Tags are filled in by background scans so adding a
// thousand files never blocks the window.
#pragma once
#include "meta.h"
#include <QAbstractTableModel>
#include <QJsonObject>
#include <QVector>

struct Item {
    quint64 uid = 0;
    TrackInfo info;
    bool failed = false;
};

class Playlist : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Col { ColState, ColNo, ColTitle, ColArtist, ColAlbum, ColTime, ColCount };

    explicit Playlist(const QString &name, QObject *parent = nullptr);

    QString name() const { return name_; }
    void setName(const QString &n);
    QString fileId() const { return id_; }

    int count() const { return items_.size(); }
    const Item &at(int row) const { return items_[row]; }
    int rowOf(quint64 uid) const;
    const Item *find(quint64 uid) const;
    void insertPaths(const QStringList &paths, int row = -1);   // returns via rowsInserted
    void removeRowsList(QList<int> rows);
    void clear();
    void setFailed(quint64 uid, bool f);
    void setPlaying(quint64 uid);
    void sortBy(int col);
    void removeDuplicates();
    double totalDuration() const;

    QJsonObject toJson() const;
    static Playlist *fromJson(const QJsonObject &o, QObject *parent);
    bool exportM3u(const QString &file) const;

    static quint64 newUid();

    // QAbstractTableModel
    int rowCount(const QModelIndex & = {}) const override { return items_.size(); }
    int columnCount(const QModelIndex & = {}) const override { return ColCount; }
    QVariant data(const QModelIndex &idx, int role) const override;
    QVariant headerData(int s, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &idx) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &idx) const override;
    bool canDropMimeData(const QMimeData *, Qt::DropAction, int, int, const QModelIndex &) const override { return true; }
    bool dropMimeData(const QMimeData *m, Qt::DropAction a, int row, int col, const QModelIndex &parent) override;

signals:
    void changed();        // anything that should be saved / re-plan "next"
    void scanned();        // some tags arrived

private:
    void scan(int from, int to);
    QString name_, id_;
    QVector<Item> items_;
    quint64 playing_ = 0;
};
