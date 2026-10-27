#include "libraryview.h"
#include "playlist.h"
#include "theme.h"
#include "widgets.h"
#include <QAbstractListModel>
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

static QString count(qsizetype n, const char *one, const char *many)
{
    return n == 1 ? QString("1 %1").arg(QString::fromUtf8(one)) : QString("%1 %2").arg(n).arg(QString::fromUtf8(many));
}

static QString humanDuration(double s)
{
    const int m = int(s / 60);
    if (m >= 60) return QString("%1 h %2 min").arg(m / 60).arg(m % 60);
    return QString("%1 min").arg(m);
}

// ─────────────────────────── covers ───────────────────────────

// Album thumbnails: read on two background threads, kept on disk
// (~/.cache/sonata/thumbs) and the recently painted ones in memory. Only what
// is on screen is ever asked for.
class CoverCache : public QObject {
    Q_OBJECT
public:
    static CoverCache &get()
    {
        static CoverCache *c = new CoverCache(qApp);
        return *c;
    }

    // `px` in device pixels; a null pixmap while loading or when there is no
    // cover. `track` may hold a few paths, one per line: the first with a cover wins.
    QPixmap pixmap(const QString &track, int px)
    {
        if (track.isEmpty() || px <= 0) return {};
        const QString key = track + '@' + QString::number(px);
        if (QPixmap *p = mem_.object(key)) return *p;
        if (missing_.contains(key) || pending_.contains(key) || pending_.size() > 48) return {};
        pending_.insert(key);
        pool_.start([this, key, track, px] {
            const QString file = cacheDir("thumbs") + '/' + QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
            const QStringList paths = track.split('\n');
            const QFileInfo src(paths.front()), jpg(file + ".jpg"), none(file + ".none");
            QImage img;
            if (jpg.exists() && jpg.lastModified() >= src.lastModified()) {
                img.load(jpg.filePath());
            } else if (!(none.exists() && none.lastModified() >= src.lastModified())) {
                for (const QString &p : paths)
                    if (!(img = readCover(p)).isNull()) break;
                if (!img.isNull()) {
                    img = img.scaled(px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    img = img.copy((img.width() - px) / 2, (img.height() - px) / 2, px, px);
                    img.save(jpg.filePath(), "JPG", 85);
                } else {
                    writeFileAtomic(none.filePath(), {});
                }
            }
            QMetaObject::invokeMethod(this, [this, key, img] {
                pending_.remove(key);
                if (img.isNull()) missing_.insert(key);
                else mem_.insert(key, new QPixmap(QPixmap::fromImage(img)), int(img.sizeInBytes() / 1024) + 1);
                emit loaded(key.left(key.lastIndexOf('@')));
            });
        });
        return {};
    }

signals:
    void loaded(const QString &track);

private:
    explicit CoverCache(QObject *parent) : QObject(parent) { pool_.setMaxThreadCount(2); }
    QCache<QString, QPixmap> mem_{12 * 1024};   // KB: a few screens of covers
    QSet<QString> pending_, missing_;
    QThreadPool pool_;
};

static void paintCover(QPainter *p, const QRect &r, const QString &track, bool round, const QString &glyph = Glyph::Note)
{
    const Theme &t = Theme::get();
    const qreal dpr = p->device()->devicePixelRatioF();
    QPixmap pix = glyph == Glyph::Note ? CoverCache::get().pixmap(track, int(r.width() * dpr)) : QPixmap();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    if (round) {
        QPainterPath clip;
        clip.addEllipse(r);
        p->setClipPath(clip);
    }
    if (pix.isNull()) {
        p->fillRect(r, t.bgLight);
        p->setFont(iconFont(std::max(10, r.width() / 3)));
        p->setPen(t.muted);
        p->drawText(r, Qt::AlignCenter, glyph);
    } else {
        pix.setDevicePixelRatio(dpr);
        p->drawPixmap(r, pix);
    }
    p->restore();
}

// ─────────────────────────── album / artist / genre lists ───────────────────────────

struct GroupRow {
    QString title, subtitle, cover, desc, search;
};

class GroupModel : public QAbstractListModel {
public:
    enum { SubtitleRole = Qt::UserRole, CoverRole, DescRole };
    using QAbstractListModel::QAbstractListModel;

    void setRows(QVector<GroupRow> rows)
    {
        all_ = std::move(rows);
        refilter();
    }
    void setQuery(const QStringList &words)
    {
        if (words == words_) return;
        words_ = words;
        refilter();
    }
    int rowCount(const QModelIndex & = {}) const override { return shown_.size(); }
    QVariant data(const QModelIndex &i, int role) const override
    {
        if (!i.isValid() || i.row() >= shown_.size()) return {};
        const GroupRow &r = all_[shown_[i.row()]];
        switch (role) {
        case Qt::DisplayRole: return r.title;
        case Qt::ToolTipRole: return r.subtitle.isEmpty() ? r.title : r.title + "\n" + r.subtitle;
        case SubtitleRole: return r.subtitle;
        case CoverRole: return r.cover;
        case DescRole: return r.desc;
        }
        return {};
    }

private:
    void refilter()
    {
        beginResetModel();
        shown_.clear();
        for (int i = 0; i < all_.size(); ++i) {
            bool ok = true;
            for (const QString &w : words_) ok = ok && all_[i].search.contains(w);
            if (ok) shown_ << i;
        }
        endResetModel();
    }
    QVector<GroupRow> all_;
    QVector<int> shown_;
    QStringList words_;
};

// Album grid cell: cover, title, artist.
class GridDelegate : public QStyledItemDelegate {
public:
    GridDelegate(QListView *view, QObject *parent) : QStyledItemDelegate(parent), view_(view) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return view_->gridSize(); }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const Theme &t = Theme::get();
        if (o.state & (QStyle::State_MouseOver | QStyle::State_Selected)) p->fillRect(o.rect, t.bgLight);
        const QRect r = o.rect.adjusted(8, 8, -8, -6);
        const int side = r.width();
        paintCover(p, QRect(r.x(), r.y(), side, side), i.data(GroupModel::CoverRole).toString(), false);
        QFont f = o.font;
        f.setPixelSize(12);
        f.setBold(true);
        p->setFont(f);
        p->setPen(t.fg);
        const QRect title(r.x(), r.y() + side + 5, side, QFontMetrics(f).height());
        p->drawText(title, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(i.data().toString(), Qt::ElideRight, side));
        f.setPixelSize(11);
        f.setBold(false);
        p->setFont(f);
        p->setPen(t.fgDim);
        p->drawText(QRect(r.x(), title.bottom() + 2, side, QFontMetrics(f).height()), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(f).elidedText(i.data(GroupModel::SubtitleRole).toString(), Qt::ElideRight, side));
    }

private:
    QListView *view_;
};

// Artist / genre row: round thumbnail (or a glyph), name, counts.
class RowDelegate : public QStyledItemDelegate {
public:
    RowDelegate(const QString &glyph, QObject *parent) : QStyledItemDelegate(parent), glyph_(glyph) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {200, 48}; }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const Theme &t = Theme::get();
        if (o.state & QStyle::State_Selected) p->fillRect(o.rect, t.selection);
        else if (o.state & QStyle::State_MouseOver) p->fillRect(o.rect, t.bgLight);
        const QRect thumb(o.rect.x() + 12, o.rect.y() + 6, 36, 36);
        paintCover(p, thumb, i.data(GroupModel::CoverRole).toString(), true, glyph_);
        const int x = thumb.right() + 12, w = o.rect.right() - x - 10;
        QFont f = o.font;
        f.setPixelSize(13);
        p->setFont(f);
        p->setPen(t.fg);
        p->drawText(QRect(x, o.rect.y() + 6, w, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(f).elidedText(i.data().toString(), Qt::ElideRight, w));
        f.setPixelSize(11);
        p->setFont(f);
        p->setPen(t.fgDim);
        p->drawText(QRect(x, o.rect.y() + 25, w, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(f).elidedText(i.data(GroupModel::SubtitleRole).toString(), Qt::ElideRight, w));
    }

private:
    QString glyph_;
};

// Album grid: as many columns as fit, cells stretched to fill the width.
class AlbumGrid : public QListView {
public:
    AlbumGrid()
    {
        setViewMode(IconMode);
        setMovement(Static);
        setResizeMode(Adjust);
        setUniformItemSizes(true);
        setSelectionMode(SingleSelection);
        setVerticalScrollMode(ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMouseTracking(true);
        setWordWrap(false);
        verticalScrollBar()->setSingleStep(24);
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        const int w = viewport()->width() - 4;   // QListView wants a little slack to fit the last column
        const int cols = std::max(1, w / 150);
        const int cell = w / cols;
        setGridSize(QSize(cell, cell - 16 + 52));   // square cover + two lines of text
        QListView::resizeEvent(e);
    }
};

// ─────────────────────────── song tables ───────────────────────────

// The same table as the lists: columns that make room for the title and drop
// out when the pane gets narrow.
class SongTable : public QTableView {
public:
    explicit SongTable(PlaylistFilter *filter)
    {
        setModel(filter);
        setSelectionBehavior(SelectRows);
        setSelectionMode(ExtendedSelection);
        setShowGrid(false);
        setWordWrap(false);
        setTextElideMode(Qt::ElideRight);
        verticalHeader()->hide();
        verticalHeader()->setDefaultSectionSize(23);
        verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        horizontalHeader()->setHighlightSections(false);
        horizontalHeader()->setMinimumSectionSize(20);
        setDragEnabled(true);
        setDragDropMode(DragOnly);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    bool showAlbum = true;

    void layoutColumns()
    {
        auto *hh = horizontalHeader();
        if (hh->count() < Playlist::ColCount) return;
        hh->resizeSection(Playlist::ColState, 24);
        hh->resizeSection(Playlist::ColNo, 38);
        hh->resizeSection(Playlist::ColTime, 78);
        for (int c : {Playlist::ColState, Playlist::ColNo, Playlist::ColTime}) hh->setSectionResizeMode(c, QHeaderView::Fixed);
        hh->setSectionResizeMode(Playlist::ColTitle, QHeaderView::Stretch);
        hh->setStretchLastSection(false);
        const int w = viewport()->width();
        setColumnHidden(Playlist::ColAlbum, !showAlbum || w < 560);
        setColumnHidden(Playlist::ColArtist, w < 380);
        const int side = std::clamp(int(w * (w < 560 ? 0.30 : 0.22)), 80, 240);
        hh->resizeSection(Playlist::ColArtist, side);
        hh->resizeSection(Playlist::ColAlbum, side);
        const QString css = QString("QScrollBar:vertical { margin-top: %1px; }").arg(hh->height());
        if (verticalScrollBar()->styleSheet() != css) verticalScrollBar()->setStyleSheet(css);
    }

    QList<int> selectedSourceRows() const
    {
        auto *f = static_cast<PlaylistFilter *>(model());
        QList<int> rows;
        for (const QModelIndex &i : selectionModel()->selectedRows()) rows << f->mapToSource(i).row();
        std::sort(rows.begin(), rows.end());
        return rows;
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QTableView::resizeEvent(e);
        layoutColumns();
    }
};

// ─────────────────────────── the view ───────────────────────────

LibraryView::LibraryView(Library *lib, QWidget *parent) : QWidget(parent), lib_(lib)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *tabRow = new QFrame;
    tabRow->setProperty("role", "panel");
    auto *th = new QHBoxLayout(tabRow);
    th->setContentsMargins(6, 4, 6, 2);
    th->setSpacing(0);
    const QStringList names{"Canciones", "Álbumes", "Artistas", "Carpetas", "Géneros"};
    for (int s = 0; s < names.size(); ++s) {
        auto *b = new QToolButton;
        b->setText(names[s]);
        b->setProperty("name", names[s]);
        b->setCheckable(true);
        b->setProperty("role", "pagetab");
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, s] { setSection(Section(s)); });
        th->addWidget(b);
        sectionTabs_ << b;
    }
    th->addStretch();
    auto *refresh = new GlyphButton(Glyph::Refresh, 16, "Buscar canciones nuevas o cambiadas");
    refresh->setObjectName("refresh");
    auto *more = new GlyphButton(Glyph::Menu, 16, "Más opciones");
    th->addWidget(refresh);
    th->addWidget(more);
    v->addWidget(tabRow);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(8, 6, 8, 6);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Buscar en la biblioteca…");
    search_->setClearButtonEnabled(true);
    search_->setFocusPolicy(Qt::ClickFocus);   // Ctrl+F; not the first thing focused at start-up
    search_->installEventFilter(this);
    tools->addWidget(search_);
    v->addLayout(tools);

    pages_ = new QStackedWidget;
    pages_->addWidget(buildSongsPage());

    auto mkList = [this](QListView *view, GroupModel *&model, QStyledItemDelegate *delegate) {
        model = new GroupModel(this);
        view->setModel(model);
        view->setItemDelegate(delegate);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->setMouseTracking(true);
        view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->viewport()->installEventFilter(this);
        connect(view, &QListView::clicked, this, [this](const QModelIndex &i) { open(i.data(GroupModel::DescRole).toString()); });
        connect(view, &QListView::activated, this, [this](const QModelIndex &i) { open(i.data(GroupModel::DescRole).toString()); });
        connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint &p) {
            const QModelIndex i = view->indexAt(p);
            if (i.isValid()) groupMenu(i.data(GroupModel::DescRole).toString(), view->viewport()->mapToGlobal(p));
        });
        pages_->addWidget(view);
    };
    albumsView_ = new AlbumGrid;
    mkList(albumsView_, albumsModel_, new GridDelegate(albumsView_, this));
    artistsView_ = new QListView;
    mkList(artistsView_, artistsModel_, new RowDelegate(Glyph::Note, this));

    foldersModel_ = new QStandardItemModel(this);
    auto *folderProxy = new QSortFilterProxyModel(this);
    folderProxy->setSourceModel(foldersModel_);
    folderProxy->setRecursiveFilteringEnabled(true);
    folderProxy->setFilterRole(Qt::UserRole + 1);
    foldersView_ = new QTreeView;
    foldersView_->setModel(folderProxy);
    foldersView_->setHeaderHidden(true);
    foldersView_->setUniformRowHeights(true);
    foldersView_->setContextMenuPolicy(Qt::CustomContextMenu);
    foldersView_->setMouseTracking(true);
    foldersView_->viewport()->installEventFilter(this);
    connect(foldersView_, &QTreeView::clicked, this, [this](const QModelIndex &i) { open(i.siblingAtColumn(0).data(Qt::UserRole).toString()); });
    connect(foldersView_, &QTreeView::activated, this, [this](const QModelIndex &i) { open(i.siblingAtColumn(0).data(Qt::UserRole).toString()); });
    connect(foldersView_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const QModelIndex i = foldersView_->indexAt(p);
        if (i.isValid()) groupMenu(i.siblingAtColumn(0).data(Qt::UserRole).toString(), foldersView_->viewport()->mapToGlobal(p));
    });
    pages_->addWidget(foldersView_);

    genresView_ = new QListView;
    mkList(genresView_, genresModel_, new RowDelegate(Glyph::Tag, this));

    pages_->addWidget(buildDetail());

    emptyPage_ = new QWidget;
    {
        auto *ev = new QVBoxLayout(emptyPage_);
        ev->addStretch();
        auto *icon = new QLabel(Glyph::Album);
        icon->setFont(iconFont(56));
        icon->setAlignment(Qt::AlignCenter);
        icon->setStyleSheet(QString("color: %1;").arg(Theme::get().accent.name()));
        auto *msg = new QLabel;
        msg->setObjectName("emptyMsg");
        msg->setAlignment(Qt::AlignCenter);
        msg->setWordWrap(true);
        msg->setProperty("role", "dim");
        auto *pick = new QPushButton("Elegir carpetas de música…");
        pick->setProperty("role", "primary");
        connect(pick, &QPushButton::clicked, this, &LibraryView::chooseFolders);
        ev->addWidget(icon);
        ev->addSpacing(8);
        ev->addWidget(msg);
        ev->addSpacing(12);
        ev->addWidget(pick, 0, Qt::AlignHCenter);
        ev->addStretch();
    }
    pages_->addWidget(emptyPage_);
    v->addWidget(pages_, 1);

    status_ = new QLabel;
    status_->setProperty("role", "caption");
    status_->setContentsMargins(10, 4, 10, 5);
    v->addWidget(status_);
    // none of these may make the pane wider than a quarter-screen tile allows
    // (Wayland enforces the minimum: the window would be cut off)
    for (QWidget *w : {static_cast<QWidget *>(tabRow), static_cast<QWidget *>(pages_), static_cast<QWidget *>(status_)})
        w->setSizePolicy(QSizePolicy::Ignored, w->sizePolicy().verticalPolicy());

    connect(search_, &QLineEdit::textChanged, this, &LibraryView::refill);
    connect(refresh, &QToolButton::clicked, this, [this] { lib_->rescan(true); });
    connect(more, &QToolButton::clicked, this, [this, more] {
        QMenu m(this);
        m.addAction("Carpetas de la biblioteca…", this, &LibraryView::chooseFolders);
        m.addAction("Buscar canciones nuevas", this, [this] { lib_->rescan(true); });
        if (section_ == Albums && pages_->currentWidget() != detailPage_) {
            QMenu *sort = m.addMenu("Ordenar álbumes por");
            const QStringList by{"Título", "Artista", "Año"};
            for (int s = 0; s < by.size(); ++s) {
                QAction *a = sort->addAction(by[s], this, [this, s] {
                    lib_->setAlbumSort(Library::AlbumSort(s));
                    emit settingsChanged();
                });
                a->setCheckable(true);
                a->setChecked(lib_->albumSort() == s);
            }
        }
        if (playbackMenu_) {
            m.addSeparator();
            playbackMenu_(m);
        }
        m.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    connect(lib_, &Library::changed, this, &LibraryView::rebuild);
    connect(lib_, &Library::progress, this, &LibraryView::showStatus);
    connect(lib_, &Library::scanned, this, [this](int added, int changed, int removed, bool report) {
        if (report) {
            QStringList parts;
            if (added) parts << count(added, "nueva", "nuevas");
            if (changed) parts << count(changed, "cambiada", "cambiadas");
            if (removed) parts << count(removed, "quitada", "quitadas");
            report_ = parts.isEmpty() ? "Todo al día" : parts.join(", ");
            QTimer::singleShot(6000, this, [this] {
                report_.clear();
                showStatus();
            });
        }
        showStatus();
        if (lib_->tracks().isEmpty()) rebuild();   // the empty page's message
    });
    // repaint the views when a cover arrives (only what is visible asked for one)
    connect(&CoverCache::get(), &CoverCache::loaded, this, [this](const QString &track) {
        for (QListView *v : {albumsView_, artistsView_})
            if (v->isVisible()) v->viewport()->update();
        if (detailCover_->isVisible() && detailCover_->property("track").toString() == track) {
            const QPixmap p = CoverCache::get().pixmap(track, int(detailCover_->width() * devicePixelRatioF()));
            if (!p.isNull()) detailCover_->setImage(p.toImage());
        }
    });
    setSection(Albums);
    rebuild();
}

QWidget *LibraryView::buildSongsPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(10, 0, 8, 6);
    songsCount_ = new QLabel;
    songsCount_->setProperty("role", "dim");
    auto *play = new QPushButton("Reproducir");
    auto *shuffle = new QPushButton("Aleatorio");
    for (auto *b : {play, shuffle}) b->setCursor(Qt::PointingHandCursor);
    head->addWidget(songsCount_, 1);
    head->addWidget(play);
    head->addWidget(shuffle);
    v->addLayout(head);
    songsFilter_ = new PlaylistFilter(this);
    songsTable_ = new SongTable(songsFilter_);
    songsTable_->installEventFilter(this);
    v->addWidget(songsTable_, 1);
    connect(play, &QPushButton::clicked, this, [this] { emit playRequested(songsCtx_, -1, false); });
    connect(shuffle, &QPushButton::clicked, this, [this] { emit playRequested(songsCtx_, -1, true); });
    connect(songsTable_, &QTableView::doubleClicked, this, [this](const QModelIndex &i) {
        emit playRequested(songsCtx_, songsFilter_->mapToSource(i).row(), false);
    });
    connect(songsTable_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const QList<int> rows = songsTable_->selectedSourceRows();
        if (!rows.isEmpty()) emit songMenuRequested(songsCtx_, rows, songsTable_->viewport()->mapToGlobal(p));
    });
    // header clicks sort these songs (and so the order they play in), like in the lists
    connect(songsTable_->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int col) {
        if (songsCtx_ && col != Playlist::ColState) songsCtx_->sortBy(col == Playlist::ColNo ? Playlist::ColAlbum : col);
    });
    return page;
}

QWidget *LibraryView::buildDetail()
{
    detailPage_ = new QWidget;
    auto *v = new QVBoxLayout(detailPage_);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(6, 0, 10, 8);
    head->setSpacing(12);
    auto *backBtn = new GlyphButton(Glyph::Back, 18, "Volver (Esc)");
    head->addWidget(backBtn, 0, Qt::AlignTop);
    detailCover_ = new CoverView;
    detailCover_->setFixedSize(96, 96);
    head->addWidget(detailCover_, 0, Qt::AlignTop);
    auto *info = new QVBoxLayout;
    info->setSpacing(2);
    detailTitle_ = new QLabel;
    detailTitle_->setProperty("role", "title");
    detailTitle_->setWordWrap(true);
    detailSub_ = new QLabel;
    detailSub_->setProperty("role", "artist");
    detailSub_->setWordWrap(true);
    detailMeta_ = new QLabel;
    detailMeta_->setProperty("role", "caption");
    auto *buttons = new QHBoxLayout;
    auto *play = new QPushButton("Reproducir");
    play->setProperty("role", "primary");
    auto *shuffle = new QPushButton("Aleatorio");
    for (auto *b : {play, shuffle}) b->setCursor(Qt::PointingHandCursor);
    buttons->addWidget(play);
    buttons->addWidget(shuffle);
    buttons->addStretch();
    info->addWidget(detailTitle_);
    info->addWidget(detailSub_);
    info->addWidget(detailMeta_);
    info->addSpacing(6);
    info->addLayout(buttons);
    info->addStretch();
    head->addLayout(info, 1);
    v->addLayout(head);
    detailFilter_ = new PlaylistFilter(this);
    detailTable_ = new SongTable(detailFilter_);
    detailTable_->installEventFilter(this);
    detailTable_->viewport()->installEventFilter(this);
    v->addWidget(detailTable_, 1);

    connect(backBtn, &QToolButton::clicked, this, &LibraryView::back);
    connect(play, &QPushButton::clicked, this, [this] { emit playRequested(detailCtx_, -1, false); });
    connect(shuffle, &QPushButton::clicked, this, [this] { emit playRequested(detailCtx_, -1, true); });
    connect(detailTable_, &QTableView::doubleClicked, this, [this](const QModelIndex &i) {
        emit playRequested(detailCtx_, detailFilter_->mapToSource(i).row(), false);
    });
    connect(detailTable_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const QList<int> rows = detailTable_->selectedSourceRows();
        if (!rows.isEmpty()) emit songMenuRequested(detailCtx_, rows, detailTable_->viewport()->mapToGlobal(p));
    });
    return detailPage_;
}

void LibraryView::setSection(Section s)
{
    section_ = s;
    for (int i = 0; i < sectionTabs_.size(); ++i) sectionTabs_[i]->setChecked(i == s);
    if (pages_->currentWidget() == detailPage_) {
        detailDesc_.clear();
        retire(detailCtx_);
        detailCtx_ = nullptr;
    }
    showStatus();
    refill();
    emit settingsChanged();
}

// Which page shows: the empty one until there is something to show.
void LibraryView::refill()
{
    const QString q = search_->text();
    const QStringList words = foldText(q).split(' ', Qt::SkipEmptyParts);
    songsFilter_->setQuery(q);
    detailFilter_->setQuery(q);
    for (GroupModel *m : {albumsModel_, artistsModel_, genresModel_}) m->setQuery(words);
    static_cast<QSortFilterProxyModel *>(foldersView_->model())->setFilterFixedString(foldText(q.trimmed()));
    if (!q.trimmed().isEmpty()) foldersView_->expandAll();

    if (lib_->tracks().isEmpty()) {
        auto *msg = emptyPage_->findChild<QLabel *>("emptyMsg");
        if (lib_->roots().isEmpty()) msg->setText("Elige las carpetas donde tienes la música:\nSonata las ordena por álbumes, artistas y carpetas.");
        else if (lib_->scanning()) msg->setText("Buscando canciones…");
        else msg->setText("No hay canciones en " + lib_->roots().join(", ").replace(QDir::homePath(), "~"));
        pages_->setCurrentWidget(emptyPage_);
        return;
    }
    if (!detailDesc_.isEmpty()) pages_->setCurrentWidget(detailPage_);
    else pages_->setCurrentIndex(section_);
}

void LibraryView::rebuild()
{
    const QVector<TrackInfo> &tracks = lib_->tracks();
    sectionTabs_[Genres]->setVisible(!lib_->genres().isEmpty());
    if (section_ == Genres && lib_->genres().isEmpty()) section_ = Albums;

    QVector<GroupRow> rows;
    for (int a = 0; a < lib_->albums().size(); ++a) {
        const Album &al = lib_->albums()[a];
        QString who = !al.artist.isEmpty() ? al.artist : al.various ? "Varios artistas" : "Artista desconocido";
        const QString sub = al.year ? who + " · " + QString::number(al.year) : who;
        rows.push_back({al.title, sub, coverFor(al.tracks), "album:" + al.key, foldText(al.title + ' ' + al.artist)});
    }
    albumsModel_->setRows(rows);
    rows.clear();
    for (const Artist &ar : lib_->artists()) {
        const QString name = ar.name.isEmpty() ? "Artista desconocido" : ar.name;
        const QString cover = ar.albums.isEmpty() ? QString() : coverFor(lib_->albums()[ar.albums.front()].tracks);
        rows.push_back({name, count(ar.albums.size(), "álbum", "álbumes") + " · " + count(ar.tracks.size(), "canción", "canciones"),
                        cover, "artist:" + ar.key, foldText(name)});
    }
    artistsModel_->setRows(rows);
    rows.clear();
    for (const Genre &g : lib_->genres())
        rows.push_back({g.name, count(g.tracks.size(), "canción", "canciones"), {}, "genre:" + foldText(g.name), foldText(g.name)});
    genresModel_->setRows(rows);

    foldersModel_->clear();
    std::function<void(QStandardItem *, int)> add = [&](QStandardItem *parent, int n) {
        const FolderNode &node = lib_->folders()[n];
        auto *item = new QStandardItem(QString("%1   ").arg(node.name));
        item->setData("folder:" + node.path, Qt::UserRole);
        item->setData(foldText(node.name), Qt::UserRole + 1);
        item->setToolTip(QString(node.path).replace(QDir::homePath(), "~") + "\n" + count(node.total, "canción", "canciones"));
        item->setEditable(false);
        auto *num = new QStandardItem(QString::number(node.total));
        num->setData(QVariant::fromValue(Theme::get().fgDim), Qt::ForegroundRole);
        num->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        num->setEditable(false);
        parent->appendRow({item, num});
        for (int c : node.children) add(item, c);
    };
    for (int r : lib_->folderRoots()) add(foldersModel_->invisibleRootItem(), r);
    foldersView_->header()->setStretchLastSection(false);
    foldersView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    foldersView_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (int r = 0; r < foldersView_->model()->rowCount(); ++r) foldersView_->expand(foldersView_->model()->index(r, 0));

    // songs: a fresh model (the old one lives on while something plays from it)
    const int scroll = songsTable_->verticalScrollBar()->value();
    Playlist *old = songsCtx_;
    songsCtx_ = makeContext("songs", "Biblioteca", lib_->songs());
    songsFilter_->setSourceModel(songsCtx_);
    songsTable_->layoutColumns();
    songsTable_->verticalScrollBar()->setValue(scroll);
    retire(old);
    songsCount_->setText(count(tracks.size(), "canción", "canciones") + " · " + humanDuration(lib_->duration(lib_->songs())));

    if (!detailDesc_.isEmpty()) {
        const QString desc = detailDesc_;
        pages_->setCurrentIndex(section_);   // so open() builds it again
        const int ds = detailTable_->verticalScrollBar()->value();
        detailDesc_.clear();
        Detail d;
        if (resolve(desc, d)) {
            open(desc);
            detailTable_->verticalScrollBar()->setValue(ds);
        } else {
            retire(detailCtx_);
            detailCtx_ = nullptr;
        }
    }
    refill();
    showStatus();
}

void LibraryView::showStatus()
{
    QString s;
    if (!report_.isEmpty()) {
        s = report_;
    } else if (lib_->scanning()) {
        s = "Buscando canciones nuevas…";
    } else {
        s = count(lib_->tracks().size(), "canción", "canciones") + " · " + count(lib_->albums().size(), "álbum", "álbumes") + " · "
            + count(lib_->artists().size(), "artista", "artistas");
    }
    status_->setText(s);
}

// Cover candidates for some songs: the first song of each folder they are in
// (a few at most), for CoverCache to try in turn.
QString LibraryView::coverFor(const QVector<int> &songs) const
{
    QStringList cover, dirs;
    for (int t : songs) {
        const QString &path = lib_->tracks()[t].path;
        const QString dir = path.left(path.lastIndexOf('/'));
        if (dirs.contains(dir)) continue;
        dirs << dir;
        cover << path;
        if (cover.size() == 3) break;
    }
    return cover.join('\n');
}

// ─────────────────────────── detail pages ───────────────────────────

bool LibraryView::resolve(const QString &desc, Detail &d) const
{
    const QString kind = desc.section(':', 0, 0), key = desc.section(':', 1);
    const QVector<TrackInfo> &tracks = lib_->tracks();
    d = {};
    d.desc = desc;
    if (kind == "songs") {
        d.title = "Todas las canciones";
        d.tracks = lib_->songs();
    } else if (kind == "album") {
        for (const Album &a : lib_->albums()) {
            if (a.key != key) continue;
            d.title = a.title;
            d.subtitle = !a.artist.isEmpty() ? a.artist : a.various ? "Varios artistas" : "Artista desconocido";
            d.tracks = a.tracks;
            d.showAlbum = false;
            break;
        }
    } else if (kind == "artist") {
        for (const Artist &a : lib_->artists()) {
            if (a.key != key) continue;
            d.title = a.name.isEmpty() ? "Artista desconocido" : a.name;
            d.subtitle = count(a.albums.size(), "álbum", "álbumes");
            d.tracks = a.tracks;
            break;
        }
    } else if (kind == "genre") {
        for (const Genre &g : lib_->genres()) {
            if (foldText(g.name) != key) continue;
            d.title = g.name;
            d.subtitle = "Género";
            d.tracks = g.tracks;
            break;
        }
    } else if (kind == "folder") {
        for (int n = 0; n < lib_->folders().size(); ++n) {
            if (lib_->folders()[n].path != key) continue;
            d.title = lib_->folders()[n].name.section('/', -1);
            // the folder it is in, inside the library ("Queen" for …/Música/Queen/A Night at the Opera)
            for (const QString &root : lib_->roots())
                if (key.startsWith(root + '/')) {
                    const QString inside = key.mid(root.size() + 1).section('/', 0, -2);
                    d.subtitle = inside.isEmpty() ? QFileInfo(root).fileName() : inside;
                }
            d.tracks = lib_->folderTracks(n);
            break;
        }
    }
    if (d.tracks.isEmpty()) return false;
    d.cover = coverFor(d.tracks);
    return true;
}

void LibraryView::open(const QString &desc)
{
    if (desc == detailDesc_ && pages_->currentWidget() == detailPage_) return;   // the second click of a double click
    Detail d;
    if (!resolve(desc, d)) return;
    Playlist *old = detailCtx_;
    detailCtx_ = makeContext(desc, d.title, d.tracks);
    detailFilter_->setSourceModel(detailCtx_);
    detailTable_->showAlbum = d.showAlbum;
    detailTable_->layoutColumns();
    retire(old);
    detailDesc_ = desc;

    detailTitle_->setText(d.title);
    detailSub_->setText(d.subtitle);
    detailSub_->setVisible(!d.subtitle.isEmpty());
    int year = 0;
    for (int t : d.tracks) year = std::max(year, lib_->tracks()[t].date.left(4).toInt());
    QStringList meta;
    if (desc.startsWith("album:") && year) meta << QString::number(year);
    meta << count(d.tracks.size(), "canción", "canciones") << humanDuration(lib_->duration(d.tracks));
    detailMeta_->setText(meta.join(" · "));
    detailCover_->setProperty("track", d.cover);
    detailCover_->setImage(CoverCache::get().pixmap(d.cover, int(detailCover_->width() * devicePixelRatioF())).toImage());
    detailTable_->scrollToTop();
    pages_->setCurrentWidget(detailPage_);
    refill();
}

void LibraryView::back()
{
    if (detailDesc_.isEmpty()) return;
    detailDesc_.clear();
    retire(detailCtx_);
    detailCtx_ = nullptr;
    refill();
}

void LibraryView::groupMenu(const QString &desc, const QPoint &global)
{
    Detail d;
    if (!resolve(desc, d)) return;
    Playlist *ctx = makeContext(desc, d.title, d.tracks);
    QList<int> rows;
    for (int i = 0; i < ctx->count(); ++i) rows << i;
    emit songMenuRequested(ctx, rows, global);
    retire(ctx);   // kept only if the menu started playing it or queued it
}

// ─────────────────────────── contexts ───────────────────────────

Playlist *LibraryView::makeContext(const QString &desc, const QString &name, const QVector<int> &tracks)
{
    auto *pl = new Playlist(name, this);
    QVector<TrackInfo> infos;
    infos.reserve(tracks.size());
    for (int t : tracks) infos << lib_->tracks()[t];
    pl->setTracks(infos);
    pl->setProperty("sonataLib", desc);
    for (int r = 0; r < pl->count(); ++r)
        if (pl->at(r).info.path == nowPath_) {
            pl->setPlaying(pl->at(r).uid);
            break;
        }
    return pl;
}

void LibraryView::retire(Playlist *pl)
{
    if (!pl) return;
    if (inUse_ && inUse_(pl)) {
        if (!retired_.contains(pl)) retired_ << pl;
    } else {
        retired_.removeAll(pl);
        pl->deleteLater();
    }
}

void LibraryView::dropUnused()
{
    for (const QPointer<Playlist> &p : QVector<QPointer<Playlist>>(retired_))
        if (p && !(inUse_ && inUse_(p))) {
            retired_.removeAll(p);
            p->deleteLater();
        }
    retired_.removeAll(nullptr);
}

QVector<Playlist *> LibraryView::contexts() const
{
    QVector<Playlist *> out;
    for (Playlist *p : {songsCtx_.data(), detailCtx_.data()})
        if (p) out << p;
    for (const QPointer<Playlist> &p : retired_)
        if (p && !out.contains(p)) out << p;
    return out;
}

void LibraryView::setNowPlaying(const QString &path)
{
    nowPath_ = path;
    for (Playlist *pl : {songsCtx_.data(), detailCtx_.data()}) {
        if (!pl) continue;
        quint64 uid = 0;
        for (int r = 0; r < pl->count() && !uid; ++r)
            if (pl->at(r).info.path == path) uid = pl->at(r).uid;
        pl->setPlaying(uid);
    }
}

QString LibraryView::describe(Playlist *pl) const
{
    return pl ? pl->property("sonataLib").toString() : QString();
}

Playlist *LibraryView::restore(const QString &desc)
{
    if (desc == "songs") return songsCtx_;
    Detail d;
    if (!resolve(desc, d)) return nullptr;
    Playlist *pl = makeContext(desc, d.title, d.tracks);
    retired_ << pl;   // lives while it plays
    return pl;
}

bool LibraryView::reveal(Playlist *pl, const QString &path)
{
    const QString desc = describe(pl);
    if (desc.isEmpty()) return false;
    SongTable *table = songsTable_;
    PlaylistFilter *filter = songsFilter_;
    Playlist *shown = songsCtx_;
    if (desc == "songs") {
        setSection(Songs);
    } else {
        if (detailDesc_ != desc) open(desc);
        table = detailTable_;
        filter = detailFilter_;
        shown = detailCtx_;
    }
    for (int r = 0; shown && r < shown->count(); ++r)
        if (shown->at(r).info.path == path) {
            const QModelIndex i = filter->mapFromSource(shown->index(r, 0));
            table->scrollTo(i, QAbstractItemView::PositionAtCenter);
            table->selectRow(i.row());
            break;
        }
    return true;
}

void LibraryView::focusSearch()
{
    search_->setFocus();
    search_->selectAll();
}

// ─────────────────────────── folders ───────────────────────────

void LibraryView::chooseFolders()
{
    QDialog d(this);
    d.setWindowTitle("Carpetas de la biblioteca");
    auto *v = new QVBoxLayout(&d);
    auto *note = new QLabel("Sonata busca canciones en estas carpetas y en las de dentro.");
    note->setProperty("role", "dim");
    note->setWordWrap(true);
    auto *list = new QListWidget;
    for (const QString &f : lib_->roots()) list->addItem(f);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton("Añadir carpeta…");
    auto *remove = new QPushButton("Quitar");
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText("Aceptar");
    box->button(QDialogButtonBox::Cancel)->setText("Cancelar");
    v->addWidget(note);
    v->addWidget(list);
    v->addLayout(row);
    v->addWidget(box);
    d.resize(480, 300);
    connect(add, &QPushButton::clicked, &d, [&] {
        const QString start = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
        const QString f = QFileDialog::getExistingDirectory(&d, "Añadir carpeta", start);
        if (!f.isEmpty() && list->findItems(f, Qt::MatchExactly).isEmpty()) list->addItem(QDir::cleanPath(f));
    });
    connect(remove, &QPushButton::clicked, &d, [list] { delete list->currentItem(); });
    connect(box, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QStringList roots;
    for (int i = 0; i < list->count(); ++i) roots << list->item(i)->text();
    lib_->setRoots(roots);
    emit settingsChanged();
    rebuild();
}

// A narrow pane (quarter-screen tile) shows the sections as icons.
void LibraryView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    findChild<QWidget *>("refresh")->setVisible(width() >= 270);   // also in the ⋯ menu
    const bool icons = width() < 420;
    if (icons == iconTabs_) return;
    iconTabs_ = icons;
    const QStringList glyphs{Glyph::Note, Glyph::Album, Glyph::Artist, Glyph::FolderMusic, Glyph::Tag};
    for (int s = 0; s < sectionTabs_.size(); ++s) {
        QToolButton *b = sectionTabs_[s];
        const QString name = b->property("name").toString();
        b->setText(icons ? glyphs[s] : name);
        b->setToolTip(icons ? name : QString());
        b->setStyleSheet(icons ? QString("font-family: '%1'; font-size: 16px; padding: 2px 5px;").arg(Theme::get().iconFont) : QString());
    }
}

bool LibraryView::eventFilter(QObject *o, QEvent *e)
{
    // mouse "back" button anywhere in the views
    if (e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(e)->button() == Qt::BackButton) {
        back();
        return true;
    }
    if (e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (o == search_ && k->key() == Qt::Key_Escape && !search_->text().isEmpty()) {
            search_->clear();
            return true;
        }
        if ((o == detailTable_ || o == search_) && (k->key() == Qt::Key_Escape || (o == detailTable_ && k->key() == Qt::Key_Backspace))
            && !detailDesc_.isEmpty()) {
            back();
            return true;
        }
        if ((o == songsTable_ || o == detailTable_) && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)) {
            auto *t = static_cast<SongTable *>(o);
            const QModelIndex i = static_cast<PlaylistFilter *>(t->model())->mapToSource(t->currentIndex());
            if (i.isValid()) emit playRequested(t == songsTable_ ? songsCtx_ : detailCtx_, i.row(), false);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

#include "libraryview.moc"
