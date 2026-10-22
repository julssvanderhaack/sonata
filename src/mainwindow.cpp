#include "mainwindow.h"
#include "eqpanel.h"
#include "lyricsview.h"
#include "playlist.h"
#include "spectrum.h"
#include "theme.h"
#include "widgets.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDBusObjectPath>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QRandomGenerator>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextLayout>
#include <QWheelEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

static QString dataDir(const QString &sub = {})
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/sonata";
    if (!sub.isEmpty()) d += '/' + sub;
    QDir().mkpath(d);
    return d;
}

static QString stateFile()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/sonata";
    QDir().mkpath(d);
    return d + "/state.json";
}

static QString humanDuration(double s)
{
    const int m = int(s / 60);
    if (m >= 60) return QString("%1 h %2 min").arg(m / 60).arg(m % 60);
    return QString("%1 min").arg(m);
}

// Sorting is done by the model itself (the play order *is* the list order),
// so the proxy only filters.
class FilterProxy : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int row, const QModelIndex &) const override
    {
        const QString f = filterRegularExpression().pattern();
        if (f.isEmpty()) return true;
        auto *pl = static_cast<Playlist *>(sourceModel());
        const TrackInfo &t = pl->at(row).info;
        const QString hay = t.displayTitle() + ' ' + t.displayArtist() + ' ' + t.album + ' ' + t.path;
        for (const QString &w : f.split(' ', Qt::SkipEmptyParts))
            if (!hay.contains(w, Qt::CaseInsensitive)) return false;
        return true;
    }
};

// ─────────────────────────── construction ───────────────────────────

MainWindow::MainWindow()
{
    setWindowTitle("Sonata");
    setAcceptDrops(true);
    setMinimumSize(420, 360);
    resize(1100, 720);
    engine_ = new Engine(this);
    fetcher_ = new LyricsFetcher(this);

    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    split_ = new QSplitter(Qt::Horizontal);
    split_->setHandleWidth(1);
    split_->setChildrenCollapsible(true);
    split_->addWidget(listPane_ = buildPlaylistPane());
    split_->addWidget(nowPane_ = buildNowPane());
    split_->setStretchFactor(0, 3);
    split_->setStretchFactor(1, 2);
    v->addWidget(split_, 1);
    v->addWidget(buildTransport());
    setCentralWidget(central);

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(1500);
    connect(saveTimer_, &QTimer::timeout, this, &MainWindow::saveState);

    connect(engine_, &Engine::trackStarted, this, &MainWindow::onTrackStarted);
    connect(engine_, &Engine::failed, this, &MainWindow::onFailed);
    connect(engine_, &Engine::stateChanged, this, [this](Engine::State s) {
        updatePlayButton();
        lyrics_->setPlaying(s == Engine::Playing);
        if (s == Engine::Paused) scheduleSave();
    });
    connect(engine_, &Engine::finished, this, [this] {
        seek_->setValue(0);
        elapsed_->setText("0:00");
        lyrics_->setPosition(0, 0);
        resumePos_ = -1;
        emit metadataChanged();
    });
    connect(engine_, &Engine::tick, this, [this](double pos) {
        const double dur = engine_->duration();
        seek_->setRange(dur > 0 ? dur : 1);
        if (!seek_->dragging()) {
            seek_->setValue(pos);
            elapsed_->setText(formatTime(pos));
        }
        total_->setText(formatTime(dur));
        if (format_->text().isEmpty()) format_->setText(engine_->formatInfo());
        // 4 Hz is enough: the lyrics keep their own clock between ticks
        if (pages_->currentWidget() == lyrics_) lyrics_->setPosition(pos, dur);
    });
    connect(engine_, &Engine::seeked, this, [this](double s) {
        seek_->setValue(s);
        elapsed_->setText(formatTime(s));
        lyrics_->setPosition(s, engine_->duration());
        emit seeked(s);
    });
    connect(fetcher_, &LyricsFetcher::searching, this, [this](quint64 tok) {
        if (tok != lyricsToken_) return;
        lyrics_->setStatus("Buscando la letra…");
        lyricsSource_->setText("buscando…");
    });
    connect(fetcher_, &LyricsFetcher::ready, this, [this](quint64 tok, const Lyrics &l) {
        if (tok != lyricsToken_) return;
        lyrics_->setLyrics(l);
        lyrics_->setStatus(l.networkError ? "No se ha podido consultar lrclib.net.\nPulsa «Buscar otra vez» en un rato."
                                          : "No se ha encontrado la letra de esta canción");
        lyricsSource_->setText(l.isEmpty() ? QString() : l.source + (l.synced ? "" : " · sin sincronizar"));
        lyrics_->setPosition(engine_->position(), engine_->duration());
    });

    // shortcuts (QLineEdit keeps its own keys, so typing in search still works)
    auto sc = [this](const QKeySequence &k, auto fn) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, fn);
    };
    sc(Qt::Key_Space, [this] { playPause(); });
    sc(Qt::Key_MediaPlay, [this] { playPause(); });
    sc(Qt::Key_Right, [this] { seekBy(5); });
    sc(Qt::Key_Left, [this] { seekBy(-5); });
    sc(Qt::SHIFT | Qt::Key_Right, [this] { seekBy(30); });
    sc(Qt::SHIFT | Qt::Key_Left, [this] { seekBy(-30); });
    sc(Qt::CTRL | Qt::Key_Right, [this] { next(); });
    sc(Qt::CTRL | Qt::Key_Left, [this] { prev(); });
    sc(Qt::CTRL | Qt::Key_Up, [this] { setVolume(volume() + 0.05); });
    sc(Qt::CTRL | Qt::Key_Down, [this] { setVolume(volume() - 0.05); });
    sc(Qt::CTRL | Qt::Key_O, [this] { openFilesDialog(false); });
    sc(Qt::CTRL | Qt::SHIFT | Qt::Key_O, [this] { openFilesDialog(true); });
    sc(Qt::CTRL | Qt::Key_F, [this] { search_->setFocus(); search_->selectAll(); });
    sc(Qt::CTRL | Qt::Key_T, [this] { addList(new Playlist(QString("Lista %1").arg(lists_.size() + 1), this), true); });
    sc(Qt::CTRL | Qt::Key_W, [this] { closeList(tabs_->currentIndex()); });
    sc(Qt::CTRL | Qt::Key_L, [this] { setExtrasVisible(true); lyricsTab_->click(); });
    sc(Qt::CTRL | Qt::Key_E, [this] { setExtrasVisible(true); eqTab_->click(); });
    sc(Qt::CTRL | Qt::Key_G, [this] { setExtrasVisible(true); specTab_->click(); });
    sc(Qt::CTRL | Qt::Key_K, [this] { setExtrasVisible(!extrasWanted_); });
    sc(Qt::CTRL | Qt::Key_J, [this] {   // jump to what's playing
        if (!playingList_) return;
        tabs_->setCurrentIndex(lists_.indexOf(playingList_));
        const int r = playingList_->rowOf(playingUid_);
        if (r >= 0) table_->scrollTo(proxy_->mapFromSource(playingList_->index(r, 0)), QAbstractItemView::PositionAtCenter);
    });
    sc(Qt::CTRL | Qt::Key_Q, [this] { close(); });

    // live theme switching
    auto *watch = new QFileSystemWatcher(this);
    const QString themeDir = QDir::homePath() + "/.local/state/omarchy/current";
    auto rewatch = [watch, themeDir] {
        for (const QString &p : {themeDir + "/theme.name", Theme::colorsPath(), themeDir})
            if (!p.isEmpty() && !watch->files().contains(p) && !watch->directories().contains(p)) watch->addPath(p);
    };
    rewatch();
    auto *themeTimer = new QTimer(this);
    themeTimer->setSingleShot(true);
    themeTimer->setInterval(400);
    connect(themeTimer, &QTimer::timeout, this, [this, rewatch] {
        rewatch();
        reloadTheme();
    });
    connect(watch, &QFileSystemWatcher::fileChanged, themeTimer, qOverload<>(&QTimer::start));
    connect(watch, &QFileSystemWatcher::directoryChanged, themeTimer, qOverload<>(&QTimer::start));

    loadState();
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildPlaylistPane()
{
    auto *pane = new QWidget;
    auto *v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *tabRow = new QFrame;
    tabRow->setProperty("role", "panel");
    auto *th = new QHBoxLayout(tabRow);
    th->setContentsMargins(0, 0, 6, 0);
    th->setSpacing(0);
    tabs_ = new QTabBar;
    tabs_->setMovable(true);
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->setUsesScrollButtons(true);
    tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    tabs_->installEventFilter(this);
    th->addWidget(tabs_, 1);
    auto *newTab = new GlyphButton(Glyph::Plus, 16, "Nueva lista (Ctrl+T)");
    th->addWidget(newTab);
    v->addWidget(tabRow);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(8, 6, 8, 6);
    tools->setSpacing(4);
    auto *addFiles = new GlyphButton(Glyph::Plus, 18, "Añadir archivos (Ctrl+O)");
    auto *addDir = new GlyphButton(Glyph::Folder, 18, "Añadir carpeta (Ctrl+Mayús+O)");
    search_ = new QLineEdit;
    search_->setPlaceholderText("Buscar en la lista…");
    search_->setClearButtonEnabled(true);
    auto *more = new GlyphButton(Glyph::Menu, 18, "Más opciones");
    tools->addWidget(addFiles);
    tools->addWidget(addDir);
    tools->addWidget(search_, 1);
    tools->addWidget(more);
    v->addLayout(tools);

    table_ = new QTableView;
    proxy_ = new FilterProxy(this);
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideRight);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(23);
    table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->horizontalHeader()->setMinimumSectionSize(20);
    table_->setDragEnabled(true);
    table_->setAcceptDrops(true);
    table_->setDropIndicatorShown(true);
    table_->setDragDropMode(QAbstractItemView::DragDrop);
    table_->setDefaultDropAction(Qt::MoveAction);
    table_->setDragDropOverwriteMode(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table_->installEventFilter(this);
    table_->viewport()->installEventFilter(this);
    table_->horizontalHeader()->installEventFilter(this);   // keeps the scrollbar below it
    v->addWidget(table_, 1);

    status_ = new QLabel;
    status_->setProperty("role", "caption");
    status_->setContentsMargins(10, 4, 10, 5);
    v->addWidget(status_);

    connect(newTab, &QToolButton::clicked, this, [this] { addList(new Playlist(QString("Lista %1").arg(lists_.size() + 1), this), true); });
    connect(addFiles, &QToolButton::clicked, this, [this] { openFilesDialog(false); });
    connect(addDir, &QToolButton::clicked, this, [this] { openFilesDialog(true); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString &s) {
        proxy_->setFilterRegularExpression(QRegularExpression(QRegularExpression::escape(s.trimmed()).replace("\\ ", " ")));
        proxy_->invalidate();
        table_->setDragEnabled(s.trimmed().isEmpty());
        updateStatus();
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        if (proxy_->rowCount() == 0) return;
        const QModelIndex src = proxy_->mapToSource(proxy_->index(0, 0));
        playItem(currentList(), currentList()->at(src.row()).uid);
    });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex &i) {
        const QModelIndex src = proxy_->mapToSource(i);
        if (src.isValid()) playItem(currentList(), currentList()->at(src.row()).uid);
    });
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int col) {
        if (auto *pl = currentList(); pl && col != Playlist::ColState) pl->sortBy(col == Playlist::ColNo ? Playlist::ColAlbum : col);
    });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0 || i >= lists_.size()) return;
        proxy_->setSourceModel(lists_[i]);
        updateStatus();
        scheduleSave();
    });
    connect(tabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        lists_.move(from, to);
        scheduleSave();
    });
    connect(tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int i) {
        if (i < 0) addList(new Playlist(QString("Lista %1").arg(lists_.size() + 1), this), true);
        else renameList(i);
    });
    connect(tabs_, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const int i = tabs_->tabAt(p);
        if (i < 0) return;
        QMenu m(this);
        m.addAction("Renombrar…", this, [this, i] { renameList(i); });
        m.addAction("Exportar como M3U…", this, [this, i] {
            const QString f = QFileDialog::getSaveFileName(this, "Exportar lista", QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/" + lists_[i]->name() + ".m3u8", "Listas M3U (*.m3u8 *.m3u)");
            if (!f.isEmpty()) lists_[i]->exportM3u(f);
        });
        m.addSeparator();
        m.addAction("Cerrar lista", this, [this, i] { closeList(i); });
        m.exec(tabs_->mapToGlobal(p));
    });
    connect(more, &QToolButton::clicked, this, [this, more] {
        QMenu m(this);
        Playlist *pl = currentList();
        m.addAction("Añadir archivos…", this, [this] { openFilesDialog(false); });
        m.addAction("Añadir carpeta…", this, [this] { openFilesDialog(true); });
        m.addAction("Importar lista M3U…", this, [this] {
            const QString f = QFileDialog::getOpenFileName(this, "Importar lista", QDir::homePath(), "Listas M3U (*.m3u8 *.m3u)");
            if (f.isEmpty()) return;
            auto *pl = addList(new Playlist(QFileInfo(f).completeBaseName(), this), true);
            pl->insertPaths(expandPaths({f}));
        });
        m.addAction("Exportar lista como M3U…", this, [this, pl] {
            const QString f = QFileDialog::getSaveFileName(this, "Exportar lista", QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/" + pl->name() + ".m3u8", "Listas M3U (*.m3u8 *.m3u)");
            if (!f.isEmpty()) pl->exportM3u(f);
        });
        m.addSeparator();
        QMenu *sort = m.addMenu("Ordenar por");
        sort->addAction("Título", this, [pl] { pl->sortBy(Playlist::ColTitle); });
        sort->addAction("Artista", this, [pl] { pl->sortBy(Playlist::ColArtist); });
        sort->addAction("Álbum y pista", this, [pl] { pl->sortBy(Playlist::ColAlbum); });
        sort->addAction("Duración", this, [pl] { pl->sortBy(Playlist::ColTime); });
        sort->addAction("Ruta del archivo", this, [pl] { pl->sortBy(-1); });
        m.addAction("Quitar duplicados", this, [pl] { pl->removeDuplicates(); });
        m.addAction("Quitar los que no existen", this, [pl] {
            QList<int> gone;
            for (int i = 0; i < pl->count(); ++i)
                if (!QFileInfo::exists(pl->at(i).info.path) || pl->at(i).failed) gone << i;
            pl->removeRowsList(gone);
        });
        m.addSeparator();
        m.addAction("Nueva lista", this, [this] { addList(new Playlist(QString("Lista %1").arg(lists_.size() + 1), this), true); });
        m.addAction("Renombrar lista…", this, [this] { renameList(tabs_->currentIndex()); });
        m.addAction("Vaciar lista", this, [pl] { pl->clear(); });
        m.addAction("Cerrar lista", this, [this] { closeList(tabs_->currentIndex()); });
        m.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    return pane;
}

QWidget *MainWindow::buildNowPane()
{
    auto *pane = new QWidget;
    auto *v = new QVBoxLayout(pane);
    v->setContentsMargins(20, 18, 20, 8);
    v->setSpacing(12);

    auto *head = new QHBoxLayout;
    head->setSpacing(18);
    cover_ = new CoverView;
    cover_->setFixedSize(200, 200);
    head->addWidget(cover_, 0, Qt::AlignTop);
    auto *info = new QVBoxLayout;
    info->setSpacing(4);
    title_ = new QLabel(fullTitle_ = "Nada sonando");
    title_->setProperty("role", "title");
    title_->setWordWrap(true);
    artist_ = new QLabel("Abre canciones con Ctrl+O, arrástralas aquí o haz clic en ellas en el explorador de archivos");
    artist_->setProperty("role", "artist");
    artist_->setWordWrap(true);
    album_ = new QLabel;
    album_->setProperty("role", "dim");
    album_->setWordWrap(true);
    format_ = new QLabel;
    format_->setProperty("role", "caption");
    for (QLabel *l : {title_, artist_, album_, format_}) l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    title_->installEventFilter(this);   // re-elides on width changes
    info->addStretch();
    info->addWidget(title_);
    info->addWidget(artist_);
    info->addWidget(album_);
    info->addSpacing(6);
    info->addWidget(format_);
    info->addStretch();
    head->addLayout(info, 1);
    v->addLayout(head);

    extras_ = new QWidget;
    auto *ev = new QVBoxLayout(extras_);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(12);
    auto *modeRow = new QHBoxLayout;
    modeRow->setSpacing(0);
    lyricsTab_ = new QToolButton;
    lyricsTab_->setText("Letra");
    eqTab_ = new QToolButton;
    eqTab_->setText("Ecualizador");
    specTab_ = new QToolButton;
    specTab_->setText("Espectro");
    specTab_->setToolTip("Amplitud por frecuencias en cada momento (Ctrl+G)");
    auto *grp = new QButtonGroup(this);
    for (QToolButton *b : {lyricsTab_, eqTab_, specTab_}) {
        b->setCheckable(true);
        b->setProperty("role", "pagetab");
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        grp->addButton(b);
        modeRow->addWidget(b);
    }
    lyricsTab_->setChecked(true);
    lyricsSource_ = new QLabel;
    lyricsSource_->setProperty("role", "caption");
    lyricsSource_->setAlignment(Qt::AlignCenter);
    modeRow->addWidget(lyricsSource_, 1);   // takes the free space, elided rather than widening the row
    modeRow->addStretch(0);
    auto mkSmall = [&](const QString &txt, const QString &tip) {
        auto *b = new QToolButton;
        b->setText(txt);
        b->setToolTip(tip);
        b->setProperty("role", "text");
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        modeRow->addWidget(b);
        return b;
    };
    auto *earlier = mkSmall("−0,5 s", "La letra va retrasada: adelantarla");
    auto *later = mkSmall("+0,5 s", "La letra va adelantada: retrasarla");
    auto *refetch = mkSmall("Buscar otra vez", "Volver a buscar la letra en internet");
    lyricsTools_ = {earlier, later, refetch};
    // narrow windows: the same three tools behind "⋯" so the row never
    // forces the window wider than its tile
    lyricsMore_ = mkSmall("⋯", "Ajustar la letra");
    auto *moreMenu = new QMenu(lyricsMore_);
    moreMenu->addAction("Adelantar la letra 0,5 s", this, [this] { lyrics_->setOffset(lyrics_->offset() + 0.5); });
    moreMenu->addAction("Retrasar la letra 0,5 s", this, [this] { lyrics_->setOffset(lyrics_->offset() - 0.5); });
    moreMenu->addAction("Buscar otra vez", refetch, &QToolButton::click);
    lyricsMore_->setMenu(moreMenu);
    lyricsMore_->setPopupMode(QToolButton::InstantPopup);
    lyricsMore_->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");
    lyricsSource_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ev->addLayout(modeRow);

    pages_ = new QStackedWidget;
    lyrics_ = new LyricsView;
    lyrics_->setStatus("La letra aparecerá aquí");
    eq_ = new EqPanel;
    pages_->addWidget(lyrics_);
    // the EQ scrolls instead of imposing its full height: in a half-screen
    // tile the song list below keeps its room
    eqScroll_ = new QScrollArea;
    eqScroll_->setWidget(eq_);
    eqScroll_->setWidgetResizable(true);
    eqScroll_->setFrameShape(QFrame::NoFrame);
    eqScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    eqScroll_->setMinimumHeight(60);
    pages_->addWidget(eqScroll_);
    spectrum_ = new SpectrumView(engine_);
    pages_->addWidget(spectrum_);
    ev->addWidget(pages_, 1);
    v->addWidget(extras_, 1);
    v->addStretch(0);   // keeps the cover at the top while the extras are hidden

    auto showPage = [=, this](QWidget *page) {
        pages_->setCurrentWidget(page == eq_ ? eqScroll_ : page);
        updateModeRow();
        if (page == lyrics_) lyrics_->setPosition(engine_->position(), engine_->duration());
        scheduleSave();
    };
    connect(lyricsTab_, &QToolButton::clicked, this, [=, this] { showPage(lyrics_); });
    connect(eqTab_, &QToolButton::clicked, this, [=, this] { showPage(eq_); });
    connect(specTab_, &QToolButton::clicked, this, [=, this] { showPage(spectrum_); });
    connect(earlier, &QToolButton::clicked, this, [this] { lyrics_->setOffset(lyrics_->offset() + 0.5); });
    connect(later, &QToolButton::clicked, this, [this] { lyrics_->setOffset(lyrics_->offset() - 0.5); });
    connect(refetch, &QToolButton::clicked, this, [this] {
        if (!playingList_) return;
        if (const Item *it = playingList_->find(playingUid_)) {
            TrackInfo t = it->info;
            lyricsToken_ = nextToken_++;
            lyrics_->setLyrics({});
            fetcher_->request(t, lyricsToken_, true);
        }
    });
    connect(lyrics_, &LyricsView::seekRequested, this, &MainWindow::seekTo);
    connect(eq_, &EqPanel::changed, this, [this](const EqSettings &s) {
        engine_->setEq(s);
        scheduleSave();
    });

    // cover follows the pane width
    pane->installEventFilter(this);
    return pane;
}

QWidget *MainWindow::buildTransport()
{
    auto *bar = new QFrame;
    bar->setProperty("role", "bar");
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(10, 6, 14, 6);
    h->setSpacing(4);
    prevBtn_ = new GlyphButton(Glyph::Prev, 22, "Anterior (Ctrl+←)");
    playBtn_ = new GlyphButton(Glyph::Play, 24, "Reproducir / pausa (Espacio)");
    playBtn_->setPrimary(true);
    playBtn_->setFixedSize(42, 42);
    nextBtn_ = new GlyphButton(Glyph::Next, 22, "Siguiente (Ctrl+→)");
    elapsed_ = new QLabel("0:00");
    total_ = new QLabel("0:00");
    for (QLabel *l : {elapsed_, total_}) {
        l->setProperty("role", "dim");
        l->setMinimumWidth(44);
        l->setFont(QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont).family(), 10));
    }
    elapsed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    seek_ = new Bar;
    seek_->setTimeTips(true);
    shuffleBtn_ = new GlyphButton(Glyph::Shuffle, 18, "Aleatorio");
    shuffleBtn_->setCheckable(true);
    repeatBtn_ = new GlyphButton(Glyph::Repeat, 18, "Repetir: no");
    volBtn_ = new GlyphButton(Glyph::VolHigh, 18, "Silenciar (rueda: volumen)");
    vol_ = new Bar;
    vol_->setFixedWidth(96);
    vol_->setRange(1.0);
    extrasBtn_ = new GlyphButton(Glyph::Lyrics, 18, "Mostrar / ocultar letra, ecualizador y espectro (Ctrl+K)");
    extrasBtn_->setCheckable(true);
    extrasBtn_->setChecked(true);
    auto *sideBtn = new GlyphButton(Glyph::Sidebar, 18, "Mostrar / ocultar la lista");
    sideBtn->setCheckable(true);
    sideBtn->setChecked(true);

    h->addWidget(prevBtn_);
    h->addWidget(playBtn_);
    h->addWidget(nextBtn_);
    h->addSpacing(10);
    h->addWidget(elapsed_);
    h->addWidget(seek_, 1);
    h->addWidget(total_);
    h->addSpacing(8);
    h->addWidget(shuffleBtn_);
    h->addWidget(repeatBtn_);
    h->addSpacing(6);
    h->addWidget(volBtn_);
    volBtn_->installEventFilter(this);
    h->addWidget(vol_);
    h->addSpacing(4);
    h->addWidget(extrasBtn_);
    h->addWidget(sideBtn);

    connect(prevBtn_, &QToolButton::clicked, this, &MainWindow::prev);
    connect(nextBtn_, &QToolButton::clicked, this, &MainWindow::next);
    connect(playBtn_, &QToolButton::clicked, this, &MainWindow::playPause);
    connect(seek_, &Bar::moved, this, [this](double s) { elapsed_->setText(formatTime(s)); });
    connect(seek_, &Bar::released, this, &MainWindow::seekTo);
    connect(shuffleBtn_, &QToolButton::clicked, this, [this] { setShuffle(!shuffle_); });
    connect(repeatBtn_, &QToolButton::clicked, this, [this] { setRepeat(Repeat((repeat_ + 1) % 3)); });
    connect(vol_, &Bar::moved, this, [this](double v) { setVolume(v); });
    connect(volBtn_, &QToolButton::clicked, this, [this] {
        if (volume() > 0) {
            preMute_ = volume();
            setVolume(0);
        } else setVolume(preMute_ > 0 ? preMute_ : 0.8);
    });
    connect(extrasBtn_, &QToolButton::clicked, this, [this](bool on) { setExtrasVisible(on); });
    connect(sideBtn, &QToolButton::toggled, this, [this](bool on) { listPane_->setVisible(on); });
    return bar;
}

void MainWindow::fitCover()
{
    // The cover takes whatever the pane leaves once title, artist and album
    // fit, so dragging the divider shrinks it (and finally hides it); the
    // pane's minimum is the text alone, which is where the divider stops.
    const bool stacked = split_->orientation() == Qt::Vertical;
    auto natural = [](QLabel *l) { return l->isHidden() || l->text().isEmpty() ? 0 : l->fontMetrics().horizontalAdvance(l->text()) + 4; };
    // the title wraps (and finally elides), so it asks for at most 200 px;
    // artist, album and format stay on one line
    const int titleW = title_->fontMetrics().horizontalAdvance(fullTitle_) / 2 + 24;
    const int infoW = std::min(380, std::max({std::min(titleW, 200), natural(artist_), natural(album_), natural(format_), 100}));
    const int infoH = title_->fontMetrics().lineSpacing() * 2 + artist_->sizeHint().height() + (album_->isHidden() ? 0 : album_->sizeHint().height())
                      + format_->sizeHint().height() + 18;
    const QMargins m = nowPane_->layout()->contentsMargins();
    const int spacing = nowPane_->layout()->spacing();
    int modeW = lyricsMore_->sizeHint().width();
    for (QToolButton *b : {lyricsTab_, eqTab_, specTab_}) modeW += b->sizeHint().width() + 2;
    nowPane_->setMinimumSize(m.left() + m.right() + infoW, m.top() + m.bottom() + infoH);

    const int paneW = nowPane_->width(), paneH = nowPane_->height();
    // lyrics/EQ/spectrum only while there is room for their tabs and a few lines
    const int extrasNeed = spacing + lyricsTab_->sizeHint().height() + 12 + 60;
    const bool extras = extrasWanted_ && paneH - m.top() - m.bottom() - infoH >= extrasNeed && paneW - m.left() - m.right() >= modeW;
    if (extras_->isHidden() == extras) extras_->setVisible(extras);
    updateModeRow();
    const int byWidth = paneW - m.left() - m.right() - 18 - infoW;   // what the text leaves beside it
    const int inner = paneH - m.top() - m.bottom();
    int byHeight;
    if (stacked) byHeight = extras ? std::min(int(inner * 0.45), inner - extrasNeed) : inner;
    else byHeight = int(paneH * (extras ? 0.38 : 0.85));
    const int s = std::min({byWidth, byHeight, int(paneW * 0.45), extras ? 340 : 480});
    cover_->setVisible(s >= 48);
    if (s >= 48 && cover_->width() != s) cover_->setFixedSize(s, s);
}

// Wraps the title, but in short panes (quarter-screen tile) stops after a
// few lines with "…"; the whole title stays in the tooltip.
void MainWindow::setTitleText(const QString &text)
{
    fullTitle_ = text;
    const int maxLines = nowPane_->height() < 420 ? 2 : 3;
    const int w = title_->width();
    QString shown = text;
    if (w > 40) {
        QTextLayout layout(text, title_->font());
        layout.beginLayout();
        int lines = 0, cut = -1;
        for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(w);
            if (++lines == maxLines) cut = line.textStart();
        }
        layout.endLayout();
        if (lines > maxLines && cut >= 0)
            shown = text.left(cut) + QFontMetrics(title_->font()).elidedText(text.mid(cut), Qt::ElideRight, w - 2);
    }
    if (title_->text() != shown) title_->setText(shown);
    title_->setToolTip(shown != text ? text : QString());
}

void MainWindow::updateModeRow()
{
    const bool lyricsPage = pages_->currentWidget() == lyrics_;
    // the now-playing pane is the whole width when stacked, ~45 % side by side
    const bool wide = nowPane_->width() >= 600;
    lyricsSource_->setVisible(lyricsPage);
    for (QWidget *w : lyricsTools_) w->setVisible(lyricsPage && wide);
    lyricsMore_->setVisible(lyricsPage && !wide);
}

void MainWindow::setExtrasVisible(bool on)
{
    extrasBtn_->setChecked(on);
    if (extrasWanted_ == on) return;
    extrasWanted_ = on;
    fitCover();   // shows them if the pane has room
    if (extras_->isVisible()) lyrics_->setPosition(engine_->position(), engine_->duration());
    scheduleSave();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == nowPane_ && e->type() == QEvent::Resize) fitCover();
    if (table_ && o == table_->horizontalHeader() && e->type() == QEvent::Resize) {
        // Qt runs the vertical scrollbar beside the column header too; start it at the first song
        const QString css = QString("QScrollBar:vertical { margin-top: %1px; }").arg(table_->horizontalHeader()->height());
        if (table_->verticalScrollBar()->styleSheet() != css) table_->verticalScrollBar()->setStyleSheet(css);
    }
    if (o == title_ && e->type() == QEvent::Resize && static_cast<QResizeEvent *>(e)->size().width() != static_cast<QResizeEvent *>(e)->oldSize().width())
        setTitleText(fullTitle_);
    if (o == volBtn_ && e->type() == QEvent::Wheel) {
        const int d = static_cast<QWheelEvent *>(e)->angleDelta().y();
        if (d) setVolume(std::clamp(volume() + (d > 0 ? 0.05 : -0.05), 0.0, 1.0));
        return true;
    }
    if (table_ && o == table_->viewport() && e->type() == QEvent::Resize) {
        // artist/album share the width with the title instead of fixed pixels
        // and drop out when the list gets too narrow for them
        const int w = table_->viewport()->width();
        auto *hh = table_->horizontalHeader();
        table_->setColumnHidden(Playlist::ColAlbum, w < 560);
        table_->setColumnHidden(Playlist::ColArtist, w < 380);
        const int side = std::clamp(int(w * (w < 560 ? 0.30 : 0.22)), 80, 240);
        hh->resizeSection(Playlist::ColArtist, side);
        hh->resizeSection(Playlist::ColAlbum, side);
    }
    if (table_ && o == table_->viewport()
        && (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove || e->type() == QEvent::Drop)) {
        // Files from outside (Nautilus…): copy them in at the drop row. The view
        // would offer "move" and let the model decide; rows from our own tabs
        // still go through the model so reordering keeps working.
        auto *d = static_cast<QDropEvent *>(e);
        if (d->source() == nullptr && d->mimeData()->hasUrls()) {
            d->setDropAction(Qt::CopyAction);
            d->accept();
            if (e->type() == QEvent::Drop) {
                int row = -1;
                const QPoint pos = d->position().toPoint();
                if (const QModelIndex i = table_->indexAt(pos); i.isValid()) {
                    row = proxy_->mapToSource(i).row();
                    if (pos.y() > table_->visualRect(i).center().y()) ++row;
                }
                dropFiles(d->mimeData(), row);
            }
            return true;
        }
    }
    if (o == table_ && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) {
            QList<int> rows;
            for (const QModelIndex &i : table_->selectionModel()->selectedRows()) rows << proxy_->mapToSource(i).row();
            const int keep = table_->currentIndex().row();
            currentList()->removeRowsList(rows);
            table_->selectRow(std::min(keep, proxy_->rowCount() - 1));
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            const QModelIndex src = proxy_->mapToSource(table_->currentIndex());
            if (src.isValid()) playItem(currentList(), currentList()->at(src.row()).uid);
            return true;
        }
    }
    if (o == tabs_ && e->type() == QEvent::MouseButtonRelease) {
        auto *m = static_cast<QMouseEvent *>(e);
        if (m->button() == Qt::MiddleButton) {
            const int i = tabs_->tabAt(m->position().toPoint());
            if (i >= 0) closeList(i);
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

// Wide tile: list | now playing. Tall, narrow tile (half screen): now playing
// on top of the list.
void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    const bool stacked = width() < 900 && height() > width();
    if (lyricsMore_) updateModeRow();
    // narrow tiles: the volume slider goes (wheel over the speaker still works),
    // then shuffle/repeat, so the transport never forces the window wider
    vol_->setVisible(width() >= 720);
    shuffleBtn_->setVisible(width() >= 520);
    repeatBtn_->setVisible(width() >= 520);
    if (stacked == (split_->orientation() == Qt::Vertical)) return;
    if (isVisible()) (stacked ? splitH_ : splitV_) = split_->saveState();   // remember the layout being left
    split_->setOrientation(stacked ? Qt::Vertical : Qt::Horizontal);
    split_->insertWidget(0, stacked ? nowPane_ : listPane_);
    updateModeRow();
    fitCover();
    // once the splitter has its new geometry; stacked, the list gets the larger share
    QTimer::singleShot(0, this, [this, stacked] {
        // where the user left it
        if (!(stacked ? splitV_ : splitH_).isEmpty() && split_->restoreState(stacked ? splitV_ : splitH_)) return;
        const int len = stacked ? split_->height() : split_->width();
        split_->setSizes(stacked ? QList<int>{int(len * 0.4), int(len * 0.6)} : QList<int>{int(len * 0.56), int(len * 0.44)});
    });
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    QMainWindow::keyPressEvent(e);
}

// ─────────────────────────── playlists ───────────────────────────

Playlist *MainWindow::currentList() const
{
    const int i = tabs_->currentIndex();
    return i >= 0 && i < lists_.size() ? lists_[i] : nullptr;
}

Playlist *MainWindow::addList(Playlist *pl, bool select)
{
    pl->setParent(this);
    lists_ << pl;
    tabs_->addTab(pl->name());
    connect(pl, &Playlist::scanned, this, [this, pl] {
        if (pl != playingList_) return;
        const Item *it = pl->find(playingUid_);
        if (!it || fullTitle_ == it->info.displayTitle()) return;
        const TrackInfo &t = it->info;
        setTitleText(t.displayTitle());
        artist_->setText(t.displayArtist().isEmpty() ? "Artista desconocido" : t.displayArtist());
        QString alb = t.album;
        if (!t.date.isEmpty()) alb += alb.isEmpty() ? t.date : QString("  ·  %1").arg(t.date);
        album_->setText(alb);
        album_->setVisible(!alb.isEmpty());
        fitCover();   // the new text sets how small the pane may get
        setWindowTitle(t.displayArtist().isEmpty() ? t.displayTitle() : t.displayTitle() + " — " + t.displayArtist());
        emit metadataChanged();
    });
    connect(pl, &Playlist::changed, this, [this, pl] {
        const int i = lists_.indexOf(pl);
        if (i >= 0 && tabs_->tabText(i) != pl->name()) tabs_->setTabText(i, pl->name());
        if (pl == currentList()) updateStatus();
        if (pl == playingList_) planNext();
        scheduleSave();
    });
    if (select) tabs_->setCurrentIndex(lists_.size() - 1);
    scheduleSave();
    return pl;
}

void MainWindow::closeList(int idx)
{
    if (idx < 0 || idx >= lists_.size()) return;
    Playlist *pl = lists_[idx];
    if (pl->count() > 0) {
        QMenu confirm(this);
        confirm.addSection(QString("¿Cerrar «%1»? (%2 canciones)").arg(pl->name()).arg(pl->count()));
        QAction *yes = confirm.addAction("Cerrar lista");
        confirm.addAction("Cancelar");
        if (confirm.exec(QCursor::pos()) != yes) return;
    }
    if (pl == playingList_) {
        engine_->stop();
        playingList_ = nullptr;
        playingUid_ = 0;
    }
    lists_.removeAt(idx);
    tabs_->removeTab(idx);
    QFile::remove(dataDir("playlists") + '/' + pl->fileId() + ".json");
    if (lists_.isEmpty()) addList(new Playlist("Lista", this), true);
    proxy_->setSourceModel(currentList());
    pl->deleteLater();
    scheduleSave();
}

void MainWindow::renameList(int idx)
{
    if (idx < 0 || idx >= lists_.size()) return;
    bool ok = false;
    const QString n = QInputDialog::getText(this, "Renombrar lista", "Nombre:", QLineEdit::Normal, lists_[idx]->name(), &ok).trimmed();
    if (ok && !n.isEmpty()) lists_[idx]->setName(n);
}

void MainWindow::openFilesDialog(bool folder)
{
    const QString start = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    QStringList paths;
    if (folder) {
        const QString d = QFileDialog::getExistingDirectory(this, "Añadir carpeta", start);
        if (!d.isEmpty()) paths << d;
    } else {
        paths = QFileDialog::getOpenFileNames(this, "Añadir canciones", start,
                                              "Audio (*.mp3 *.flac *.ogg *.oga *.opus *.m4a *.mp4 *.aac *.wav *.wv *.ape *.aiff *.aif *.wma *.mpc "
                                              "*.dsf *.dff *.tta *.mka *.webm *.m4b *.m3u *.m3u8);;Todos (*)");
    }
    if (!paths.isEmpty()) addPaths(paths, false);
}

void MainWindow::addPaths(const QStringList &paths, bool playNow)
{
    QStringList files;
    for (const QString &f : expandPaths(paths))
        if (isAudioFile(f) || QFileInfo(f).isFile()) files << f;
    if (files.isEmpty()) return;
    Playlist *pl = currentList();
    if (!pl) pl = addList(new Playlist("Lista", this), true);

    // one file that's already in the list: just (maybe) play it, don't duplicate
    if (files.size() == 1) {
        for (int i = 0; i < pl->count(); ++i)
            if (pl->at(i).info.path == files.front()) {
                if (playNow || engine_->state() == Engine::Stopped) playItem(pl, pl->at(i).uid);
                table_->scrollTo(proxy_->mapFromSource(pl->index(i, 0)));
                return;
            }
    }
    const int first = pl->count();
    pl->insertPaths(files);
    table_->scrollTo(proxy_->mapFromSource(pl->index(first, 0)));
    if (playNow || engine_->state() == Engine::Stopped) playItem(pl, pl->at(first).uid);
}

bool MainWindow::dropFiles(const QMimeData *m, int row)
{
    QStringList paths;
    for (const QUrl &u : m->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    QStringList files;
    for (const QString &f : expandPaths(paths))
        if (isAudioFile(f)) files << f;
    if (files.isEmpty()) return false;
    Playlist *pl = currentList();
    if (!pl) pl = addList(new Playlist("Lista", this), true);
    if (row < 0 || row > pl->count()) row = pl->count();
    pl->insertPaths(files, row);
    table_->scrollTo(proxy_->mapFromSource(pl->index(row, 0)));
    return true;
}

// Drops anywhere else in the window (cover, lyrics, transport) append to the list.
void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
}

void MainWindow::dropEvent(QDropEvent *e)
{
    if (e->source() == nullptr && dropFiles(e->mimeData(), -1)) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    Playlist *pl = currentList();
    QList<int> rows;
    for (const QModelIndex &i : table_->selectionModel()->selectedRows()) rows << proxy_->mapToSource(i).row();
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty()) return;
    QMenu m(this);
    m.addAction("Reproducir", this, [=, this] { playItem(pl, pl->at(rows.front()).uid); });
    if (playingList_ == pl && playingUid_)
        m.addAction("Reproducir a continuación", this, [=, this] {
            QVector<Item> moving;
            QStringList paths;
            for (int r : rows) paths << pl->at(r).info.path;
            pl->removeRowsList(rows);
            pl->insertPaths(paths, pl->rowOf(playingUid_) + 1);
        });
    m.addAction("Abrir carpeta", this, [=] {
        const QString path = pl->at(rows.front()).info.path;
        if (!QProcess::startDetached("nautilus", {"--select", path}))
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    if (lists_.size() > 1) {
        QMenu *copy = m.addMenu("Copiar a la lista");
        for (Playlist *other : lists_) {
            if (other == pl) continue;
            copy->addAction(other->name(), this, [=] {
                QStringList paths;
                for (int r : rows) paths << pl->at(r).info.path;
                other->insertPaths(paths);
            });
        }
    }
    m.addAction("Nueva lista con la selección", this, [=, this] {
        QStringList paths;
        for (int r : rows) paths << pl->at(r).info.path;
        auto *nl = addList(new Playlist(QString("Lista %1").arg(lists_.size() + 1), this), false);
        nl->insertPaths(paths);
    });
    m.addSeparator();
    m.addAction(QString("Quitar de la lista (%1)").arg(rows.size()), this, [=] { pl->removeRowsList(rows); });
    m.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::updateStatus()
{
    Playlist *pl = currentList();
    if (!pl) return;
    const int n = pl->count();
    QString s = n == 1 ? "1 canción" : QString("%1 canciones").arg(n);
    if (n) s += " · " + humanDuration(pl->totalDuration());
    if (!search_->text().trimmed().isEmpty()) s = QString("%1 de %2").arg(proxy_->rowCount()).arg(s);
    status_->setText(s);
}

// ─────────────────────────── playback ───────────────────────────

bool MainWindow::hasCurrent() const
{
    return playingList_ && playingList_->find(playingUid_);
}

void MainWindow::playItem(Playlist *pl, quint64 uid, double start, bool paused)
{
    if (!pl) return;
    const Item *it = pl->find(uid);
    if (!it) return;
    const quint64 tok = nextToken_++;
    tokens_[tok] = {pl, uid};
    lastStartToken_ = tok;
    resumePos_ = -1;
    engine_->play(it->info.path, tok, start, paused);
    if (playingList_ && playingList_ != pl) playingList_->setPlaying(0);
    const bool changed = playingList_ != pl || playingUid_ != uid;
    playingList_ = pl;
    playingUid_ = uid;
    pl->setPlaying(uid);
    if (changed || engine_->state() != Engine::Stopped) showTrack(it);
    if (shuffle_) {
        shufflePlayed_.insert(uid);
        if (history_.isEmpty() || history_.back() != uid) history_ << uid;
    }
    planNext();
    emit metadataChanged();
}

void MainWindow::onTrackStarted(quint64 token)
{
    auto [pl, uid] = tokens_.value(token);
    // forget tokens we can't get back to
    for (auto it = tokens_.begin(); it != tokens_.end();)
        it = it.key() + 64 < token ? tokens_.erase(it) : std::next(it);
    if (!pl) return;
    failStreak_ = 0;
    if (pl == playingList_ && uid == playingUid_) {
        // started by playItem(); just refresh the format line
        format_->setText(engine_->formatInfo());
        return;
    }
    if (playingList_ && playingList_ != pl) playingList_->setPlaying(0);
    playingList_ = pl;
    playingUid_ = uid;
    pl->setPlaying(uid);
    if (shuffle_) {
        shufflePlayed_.insert(uid);
        history_ << uid;
    }
    if (const Item *it = pl->find(uid)) showTrack(it);
    format_->setText(engine_->formatInfo());
    planNext();
    emit metadataChanged();
}

void MainWindow::onFailed(quint64 token, const QString &path, const QString &msg)
{
    auto [pl, uid] = tokens_.value(token);
    status_->setText(QString("No se puede reproducir %1: %2").arg(QFileInfo(path).fileName(), msg));
    QTimer::singleShot(6000, this, &MainWindow::updateStatus);
    if (!pl) return;
    pl->setFailed(uid, true);
    if (token == lastStartToken_ && engine_->currentToken() != token) {
        // a track we asked to play right now: move on to the following one
        if (++failStreak_ > 25) {
            engine_->stop();
            return;
        }
        if (quint64 n = pickNext(pl, uid, true)) playItem(pl, n);
        else engine_->stop();
    } else {
        planNext();   // the queued "next" broke; failed items are skipped
    }
}

quint64 MainWindow::pickNext(Playlist *pl, quint64 from, bool manual)
{
    if (!pl || pl->count() == 0) return 0;
    if (!manual && repeat_ == RepeatOne && pl->find(from)) return from;
    if (shuffle_) {
        QVector<quint64> pool;
        for (int i = 0; i < pl->count(); ++i) {
            const Item &it = pl->at(i);
            if (it.uid != from && !it.failed && !shufflePlayed_.contains(it.uid)) pool << it.uid;
        }
        if (pool.isEmpty()) {
            if (repeat_ != RepeatAll && !manual) return 0;
            shufflePlayed_.clear();
            for (int i = 0; i < pl->count(); ++i)
                if (pl->at(i).uid != from && !pl->at(i).failed) pool << pl->at(i).uid;
            if (pool.isEmpty()) return 0;
        }
        // keep the same pick while it's still valid so "next" doesn't keep changing
        static quint64 planned = 0;
        if (pool.contains(planned)) return planned;
        planned = pool[QRandomGenerator::global()->bounded(int(pool.size()))];
        return planned;
    }
    const int row = pl->rowOf(from);
    for (int k = 1; k <= pl->count(); ++k) {
        int r = row + k;
        if (r >= pl->count()) {
            if (repeat_ != RepeatAll) return 0;
            r %= pl->count();
        }
        if (!pl->at(r).failed) return pl->at(r).uid;
    }
    return 0;
}

void MainWindow::planNext()
{
    if (!playingList_) {
        engine_->setNext({}, 0);
        return;
    }
    const quint64 n = pickNext(playingList_, playingUid_, false);
    const Item *it = n ? playingList_->find(n) : nullptr;
    if (!it) {
        engine_->setNext({}, 0);
        return;
    }
    const quint64 tok = nextToken_++;
    tokens_[tok] = {playingList_, n};
    engine_->setNext(it->info.path, tok);
}

void MainWindow::showTrack(const Item *it)
{
    const TrackInfo &t = it->info;
    setTitleText(t.displayTitle());
    artist_->setText(t.displayArtist().isEmpty() ? "Artista desconocido" : t.displayArtist());
    QString alb = t.album;
    if (!t.date.isEmpty()) alb += alb.isEmpty() ? t.date : QString("  ·  %1").arg(t.date);
    album_->setText(alb);
    album_->setVisible(!alb.isEmpty());
        fitCover();   // the new text sets how small the pane may get
    format_->setText(engine_->formatInfo());
    setWindowTitle(t.displayArtist().isEmpty() ? t.displayTitle() : t.displayTitle() + " — " + t.displayArtist());
    total_->setText(formatTime(t.duration));
    seek_->setRange(t.duration > 0 ? t.duration : 1);
    seek_->setValue(0);
    elapsed_->setText("0:00");

    // cover + full tags (embedded lyrics) off the GUI thread
    lyricsToken_ = nextToken_++;
    const quint64 ltok = lyricsToken_;
    lyrics_->setOffset(0);
    lyrics_->setLyrics({});
    lyrics_->setStatus("");
    lyricsSource_->clear();
    const QString path = t.path;
    const TrackInfo known = t;
    struct Res {
        QImage img;
        TrackInfo info;
        QString art;
    };
    auto *w = new QFutureWatcher<Res>(this);
    connect(w, &QFutureWatcher<Res>::finished, this, [this, w, ltok] {
        w->deleteLater();
        if (ltok != lyricsToken_) return;
        Res r = w->result();
        cover_->setImage(r.img);
        artFile_ = r.art;
        emit metadataChanged();
        fetcher_->request(r.info, ltok);
    });
    w->setFuture(QtConcurrent::run([path, known] {
        Res r;
        r.info = readTrackInfo(path);
        if (r.info.duration <= 0) r.info.duration = known.duration;
        QImage img = readCover(path);
        if (!img.isNull()) {
            if (img.width() > 1000 || img.height() > 1000) img = img.scaled(1000, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            r.img = img;
            // for MPRIS (the bar / notifications)
            const QByteArray key = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex();
            r.art = cacheDir("art") + '/' + key + ".jpg";
            img.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(r.art, "JPG", 88);
        }
        return r;
    }));
}

void MainWindow::play()
{
    if (engine_->state() == Engine::Paused) {
        engine_->setPaused(false);
        return;
    }
    if (engine_->state() == Engine::Playing) return;
    if (hasCurrent()) {
        playItem(playingList_, playingUid_, resumePos_ > 0 ? resumePos_ : 0);
        return;
    }
    Playlist *pl = currentList();
    if (!pl || pl->count() == 0) return;
    const QModelIndex cur = proxy_->mapToSource(table_->currentIndex());
    playItem(pl, pl->at(cur.isValid() ? cur.row() : 0).uid);
}

void MainWindow::pause()
{
    if (engine_->state() == Engine::Playing) engine_->setPaused(true);
}

void MainWindow::playPause()
{
    if (engine_->state() == Engine::Playing) pause();
    else play();
}

void MainWindow::stop()
{
    resumePos_ = -1;
    engine_->stop();
    seek_->setValue(0);
    elapsed_->setText("0:00");
}

void MainWindow::next()
{
    if (!hasCurrent()) {
        play();
        return;
    }
    if (quint64 n = pickNext(playingList_, playingUid_, true)) playItem(playingList_, n);
}

void MainWindow::prev()
{
    if (!hasCurrent()) return;
    if (engine_->state() != Engine::Stopped && engine_->position() > 3) {
        seekTo(0);
        return;
    }
    Playlist *pl = playingList_;
    if (shuffle_ && history_.size() > 1) {
        history_.pop_back();
        const quint64 back = history_.back();
        history_.pop_back();
        if (pl->find(back)) {
            playItem(pl, back);
            return;
        }
    }
    int r = pl->rowOf(playingUid_) - 1;
    if (r < 0) r = repeat_ == RepeatAll ? pl->count() - 1 : 0;
    playItem(pl, pl->at(r).uid);
}

void MainWindow::seekTo(double sec)
{
    if (engine_->state() == Engine::Stopped) {
        if (hasCurrent()) playItem(playingList_, playingUid_, std::max(0.0, sec));
        return;
    }
    engine_->seek(std::max(0.0, sec));
}

void MainWindow::setVolume(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    engine_->setVolume(float(v));
    vol_->setValue(v);
    updateVolumeIcon();
    emit optionsChanged();
    scheduleSave();
}

void MainWindow::setShuffle(bool on)
{
    shuffle_ = on;
    shuffleBtn_->setChecked(on);
    shuffleBtn_->setToolTip(on ? "Aleatorio: sí" : "Aleatorio: no");
    shufflePlayed_.clear();
    history_.clear();
    if (on && playingUid_) {
        shufflePlayed_.insert(playingUid_);
        history_ << playingUid_;
    }
    planNext();
    emit optionsChanged();
    scheduleSave();
}

void MainWindow::setRepeat(Repeat r)
{
    repeat_ = r;
    repeatBtn_->setGlyph(r == RepeatOne ? Glyph::RepeatOne : Glyph::Repeat);
    repeatBtn_->setCheckable(true);
    repeatBtn_->setChecked(r != RepeatOff);
    repeatBtn_->setToolTip(r == RepeatOff ? "Repetir: no" : r == RepeatAll ? "Repetir: toda la lista" : "Repetir: esta canción");
    repeatBtn_->update();
    planNext();
    emit optionsChanged();
    scheduleSave();
}

void MainWindow::updatePlayButton()
{
    const bool playing = engine_->state() == Engine::Playing;
    playBtn_->setGlyph(playing ? Glyph::Pause : Glyph::Play);
    playBtn_->update();
}

void MainWindow::updateVolumeIcon()
{
    const double v = volume();
    volBtn_->setGlyph(v <= 0 ? Glyph::VolOff : v < 0.34 ? Glyph::VolLow : v < 0.67 ? Glyph::VolMed : Glyph::VolHigh);
    volBtn_->setToolTip(QString("Volumen %1 % · clic: silenciar, rueda: cambiar").arg(qRound(v * 100)));
    volBtn_->update();
}

QVariantMap MainWindow::mprisMetadata() const
{
    QVariantMap m;
    const Item *it = hasCurrent() ? playingList_->find(playingUid_) : nullptr;
    if (!it) {
        m["mpris:trackid"] = QVariant::fromValue(QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
        return m;
    }
    const TrackInfo &t = it->info;
    m["mpris:trackid"] = QVariant::fromValue(QDBusObjectPath(QString("/org/sonata/track/%1").arg(playingUid_)));
    const double dur = engine_->duration() > 0 ? engine_->duration() : t.duration;
    m["mpris:length"] = qlonglong(dur * 1e6);
    m["xesam:title"] = t.displayTitle();
    if (!t.displayArtist().isEmpty()) m["xesam:artist"] = QStringList{t.displayArtist()};
    if (!t.album.isEmpty()) m["xesam:album"] = t.album;
    if (!t.albumArtist.isEmpty()) m["xesam:albumArtist"] = QStringList{t.albumArtist};
    if (t.trackNo) m["xesam:trackNumber"] = t.trackNo;
    m["xesam:url"] = QUrl::fromLocalFile(t.path).toString();
    if (!artFile_.isEmpty()) m["mpris:artUrl"] = QUrl::fromLocalFile(artFile_).toString();
    return m;
}

void MainWindow::raiseWindow()
{
    QProcess::startDetached("sh", {"-c", "hyprctl dispatch 'hl.dsp.focus({ window = \"class:sonata\" })' >/dev/null 2>&1"
                                         " || hyprctl dispatch focuswindow class:sonata"});
}

// ─────────────────────────── persistence ───────────────────────────

void MainWindow::scheduleSave()
{
    if (!loading_) saveTimer_->start();
}

void MainWindow::saveState()
{
    const QString plDir = dataDir("playlists");
    QJsonArray ids;
    QSet<QString> keep;
    for (Playlist *pl : lists_) {
        ids << pl->fileId();
        keep.insert(pl->fileId() + ".json");
        QFile f(plDir + '/' + pl->fileId() + ".json.tmp");
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(pl->toJson()).toJson(QJsonDocument::Compact));
            f.close();
            QFile::remove(plDir + '/' + pl->fileId() + ".json");
            f.rename(plDir + '/' + pl->fileId() + ".json");
        }
    }
    for (const QString &f : QDir(plDir).entryList({"*.json"}, QDir::Files))
        if (!keep.contains(f)) QFile::remove(plDir + '/' + f);

    (split_->orientation() == Qt::Vertical ? splitV_ : splitH_) = split_->saveState();
    const EqSettings eq = eq_->settings();
    QJsonArray gains;
    for (float g : eq.gains) gains << g;
    QJsonObject o{
        {"lists", ids},
        {"current", tabs_->currentIndex()},
        {"volume", volume()},
        {"shuffle", shuffle_},
        {"repeat", int(repeat_)},
        {"eq", QJsonObject{{"enabled", eq.enabled}, {"preamp", eq.preamp}, {"auto", eq.autoPreamp}, {"gains", gains}}},
        {"eqCustom", eq_->customPresets()},
        {"split", QString::fromLatin1(splitH_.toBase64())},
        {"splitStack", QString::fromLatin1(splitV_.toBase64())},
        {"page", pages_->currentWidget() == eqScroll_ ? "eq" : pages_->currentWidget() == spectrum_ ? "spectrum" : "lyrics"},
        {"listVisible", listPane_->isVisible()},
        {"extrasVisible", extrasWanted_},
    };
    if (hasCurrent()) {
        double pos = engine_->state() != Engine::Stopped ? engine_->position() : std::max(0.0, resumePos_);
        o["playing"] = QJsonObject{{"list", playingList_->fileId()}, {"row", playingList_->rowOf(playingUid_)}, {"pos", pos}};
    }
    QFile f(stateFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson());
}

void MainWindow::loadState()
{
    loading_ = true;
    QFile f(stateFile());
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();

    const QString plDir = dataDir("playlists");
    for (const QJsonValue &id : o.value("lists").toArray()) {
        QFile pf(plDir + '/' + id.toString() + ".json");
        if (!pf.open(QIODevice::ReadOnly)) continue;
        addList(Playlist::fromJson(QJsonDocument::fromJson(pf.readAll()).object(), this), false);
    }
    if (lists_.isEmpty()) addList(new Playlist("Lista", this), false);
    tabs_->setCurrentIndex(std::clamp(o.value("current").toInt(), 0, int(lists_.size()) - 1));
    proxy_->setSourceModel(currentList());

    auto *hh = table_->horizontalHeader();
    hh->resizeSection(Playlist::ColState, 24);
    hh->resizeSection(Playlist::ColNo, 38);
    hh->resizeSection(Playlist::ColTime, 78);
    for (int c : {Playlist::ColState, Playlist::ColNo, Playlist::ColTime}) hh->setSectionResizeMode(c, QHeaderView::Fixed);
    hh->setSectionResizeMode(Playlist::ColTitle, QHeaderView::Stretch);
    hh->setStretchLastSection(false);

    // "splitStack", not the old "splitV": that one was saved while the
    // artwork squeezed the list
    splitV_ = QByteArray::fromBase64(o.value("splitStack").toString().toLatin1());
    splitH_ = QByteArray::fromBase64(o.value("split").toString().toLatin1());
    if (splitH_.isEmpty() || !split_->restoreState(splitH_)) split_->setSizes({620, 480});
    split_->setOrientation(Qt::Horizontal);   // resizeEvent decides the real layout
    if (!o.value("listVisible").toBool(true)) {
        listPane_->hide();
        for (auto *b : findChildren<GlyphButton *>())
            if (b->text() == Glyph::Sidebar) b->setChecked(false);
    }

    setVolume(o.value("volume").toDouble(1.0));
    setShuffle(o.value("shuffle").toBool());
    setRepeat(Repeat(o.value("repeat").toInt()));

    EqSettings eq;
    const QJsonObject je = o.value("eq").toObject();
    eq.enabled = je.value("enabled").toBool();
    eq.preamp = float(je.value("preamp").toDouble());
    eq.autoPreamp = je.value("auto").toBool(true);
    const QJsonArray ga = je.value("gains").toArray();
    for (int b = 0; b < kBands && b < ga.size(); ++b) eq.gains[b] = float(ga[b].toDouble());
    eq_->setCustomPresets(o.value("eqCustom").toObject());
    eq_->setSettings(eq);
    engine_->setEq(eq);
    if (o.value("page").toString() == "eq") eqTab_->click();
    else if (o.value("page").toString() == "spectrum") specTab_->click();
    if (!o.value("extrasVisible").toBool(true)) setExtrasVisible(false);

    // last track, paused where we left it (press play to continue)
    const QJsonObject pj = o.value("playing").toObject();
    for (Playlist *pl : lists_) {
        if (pl->fileId() != pj.value("list").toString()) continue;
        const int row = pj.value("row").toInt(-1);
        if (row < 0 || row >= pl->count()) break;
        playingList_ = pl;
        playingUid_ = pl->at(row).uid;
        pl->setPlaying(playingUid_);
        showTrack(&pl->at(row));
        resumePos_ = pj.value("pos").toDouble();
        seek_->setValue(resumePos_);
        elapsed_->setText(formatTime(resumePos_));
        table_->scrollTo(proxy_->mapFromSource(pl->index(row, 0)), QAbstractItemView::PositionAtCenter);
        break;
    }
    updateStatus();
    loading_ = false;
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    saveTimer_->stop();
    saveState();
    engine_->stop();
    e->accept();
}

void MainWindow::reloadTheme()
{
    Theme::get().load();
    Theme::get().apply();
    for (QWidget *w : findChildren<QWidget *>()) w->update();
    if (auto *m = proxy_->sourceModel()) emit static_cast<Playlist *>(m)->dataChanged(m->index(0, 0), m->index(m->rowCount() - 1, Playlist::ColCount - 1));
}
