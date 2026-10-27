// The library pane: Canciones / Álbumes / Artistas / Carpetas / Géneros, and
// a page per album, artist, folder or genre with its songs.
//
// Songs are shown and played through ordinary Playlist models built on the
// fly ("contexts"), so playing, the queue, tag editing and the song menu work
// exactly as in the lists. A context stays alive while something still plays
// from it.
#pragma once
#include "library.h"
#include <QPointer>
#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QPushButton;
class QStackedWidget;
class QStandardItemModel;
class QToolButton;
class QTreeView;
class CoverView;
class GlyphButton;
class GroupModel;
class Playlist;
class PlaylistFilter;
class SongTable;

class LibraryView : public QWidget {
    Q_OBJECT
public:
    enum Section { Songs, Albums, Artists, Folders, Genres };

    explicit LibraryView(Library *lib, QWidget *parent = nullptr);

    QVector<Playlist *> contexts() const;           // every model built from the library that is alive
    void setInUse(std::function<bool(Playlist *)> f) { inUse_ = std::move(f); }
    void dropUnused();                              // contexts nothing plays from any more
    void setPlaybackMenu(std::function<void(QMenu &)> f) { playbackMenu_ = std::move(f); }
    void setNowPlaying(const QString &path);
    QString describe(Playlist *pl) const;           // to save what is playing ("album:<key>"…), empty if not ours
    Playlist *restore(const QString &desc);         // …and to build it again at start-up
    bool reveal(Playlist *pl, const QString &path); // show the page a context belongs to, at that song
    void focusSearch();
    Section section() const { return section_; }
    void setSection(Section s);
    Q_INVOKABLE void openPage(const QString &desc) { open(desc); }   // tests (SONATA_LIBOPEN)

signals:
    void playRequested(Playlist *pl, int row, bool shuffle);   // row -1: from the top (random when shuffling)
    void songMenuRequested(Playlist *pl, const QList<int> &rows, const QPoint &global);
    void settingsChanged();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    struct Detail {
        QString desc, title, subtitle, cover;
        QVector<int> tracks;
        bool showAlbum = true;
    };
    QWidget *buildDetail();
    QWidget *buildSongsPage();
    void rebuild();                           // library changed
    void refill();                            // search changed
    void open(const QString &desc);
    bool resolve(const QString &desc, Detail &d) const;
    void back();
    Playlist *makeContext(const QString &desc, const QString &name, const QVector<int> &tracks);
    void retire(Playlist *pl);
    void showStatus();
    void chooseFolders();
    void groupMenu(const QString &desc, const QPoint &global);
    QString coverFor(const QVector<int> &songs) const;

    Library *lib_;
    Section section_ = Albums;
    QVector<QToolButton *> sectionTabs_;
    bool iconTabs_ = false;
    QLineEdit *search_;
    QStackedWidget *pages_;
    QLabel *status_;
    QString report_;   // what the last ⟳ found, shown for a while

    SongTable *songsTable_;
    PlaylistFilter *songsFilter_;
    QLabel *songsCount_;
    QPointer<Playlist> songsCtx_;

    QListView *albumsView_, *artistsView_, *genresView_;
    GroupModel *albumsModel_, *artistsModel_, *genresModel_;
    QTreeView *foldersView_;
    QStandardItemModel *foldersModel_;
    QWidget *emptyPage_;

    QWidget *detailPage_;
    CoverView *detailCover_;
    QLabel *detailTitle_, *detailSub_, *detailMeta_;
    SongTable *detailTable_;
    PlaylistFilter *detailFilter_;
    QPointer<Playlist> detailCtx_;
    QString detailDesc_;

    QVector<QPointer<Playlist>> retired_;
    std::function<bool(Playlist *)> inUse_;
    std::function<void(QMenu &)> playbackMenu_;
    QString nowPath_;
};
