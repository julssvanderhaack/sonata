#include "theme.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QTextStream>

Theme &Theme::get()
{
    static Theme t;
    return t;
}

QString Theme::colorsPath()
{
    for (const QString &p : {QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml",
                             QDir::homePath() + "/.config/omarchy/current/theme/colors.toml"})
        if (QFile::exists(p)) return p;
    return {};
}

static QHash<QString, QString> readColors()
{
    QHash<QString, QString> c;
    QFile f(Theme::colorsPath());
    if (!f.open(QIODevice::ReadOnly)) return c;
    static const QRegularExpression re("^\\s*([A-Za-z_]+)\\s*=\\s*\"([^\"]*)\"");
    QTextStream in(&f);
    while (!in.atEnd()) {
        auto m = re.match(in.readLine());
        if (m.hasMatch()) c.insert(m.captured(1), m.captured(2));
    }
    return c;
}

void Theme::load()
{
    auto c = readColors();
    auto col = [&](const char *k, const QColor &fallback) {
        QColor q(c.value(k));
        return q.isValid() ? q : fallback;
    };
    bg = col("background", QColor("#1a1b26"));
    bgDark = col("dark_background", bg.darker(120));
    bgLight = col("lighter_background", bg.lighter(140));
    fg = col("foreground", QColor("#c0caf5"));
    fgDim = col("dark_foreground", QColor("#7a7f9a"));
    fgLight = col("light_foreground", fg);
    accent = col("accent", QColor("#7aa2f7"));
    selection = col("selection", bgLight);
    muted = col("muted", bgLight.lighter(130));
    dark = bg.lightness() < 128;
    // fgDim in some themes is nearly the background; keep it readable
    if (std::abs(fgDim.lightness() - bg.lightness()) < 70) {
        fgDim = QColor::fromRgbF((fg.redF() + bg.redF()) / 2, (fg.greenF() + bg.greenF()) / 2, (fg.blueF() + bg.blueF()) / 2);
    }

    const QStringList fams = QFontDatabase::families();
    for (const char *f : {"Symbols Nerd Font", "JetBrainsMono Nerd Font Propo", "FiraCode Nerd Font Propo", "JetBrainsMono Nerd Font"})
        if (fams.contains(f)) {
            iconFont = f;
            break;
        }
}

void Theme::apply()
{
    QPalette pal;
    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::WindowText, fg);
    pal.setColor(QPalette::Base, bg);
    pal.setColor(QPalette::AlternateBase, bgDark);
    pal.setColor(QPalette::Text, fg);
    pal.setColor(QPalette::Button, bgLight);
    pal.setColor(QPalette::ButtonText, fg);
    pal.setColor(QPalette::Highlight, selection);
    pal.setColor(QPalette::HighlightedText, fg);
    pal.setColor(QPalette::PlaceholderText, fgDim);
    pal.setColor(QPalette::ToolTipBase, bgLight);
    pal.setColor(QPalette::ToolTipText, fg);
    pal.setColor(QPalette::Link, accent);
    pal.setColor(QPalette::Accent, accent);
    qApp->setPalette(pal);
    qApp->setStyleSheet(styleSheet());
}

QString Theme::styleSheet() const
{
    auto n = [](const QColor &c) { return c.name(QColor::HexArgb); };
    QColor hover = bgLight.lighter(dark ? 130 : 95);
    QColor accentText = accent.lightness() > 140 ? bg : fg;
    QString s = R"(
QWidget { background: %BG%; color: %FG%; }
QToolTip { background: %BGL%; color: %FG%; border: 1px solid %MUTED%; padding: 4px; }
QLineEdit, QComboBox {
  background: %BGD%; border: 1px solid %MUTED%; border-radius: 0; padding: 4px 8px;
  selection-background-color: %SEL%; selection-color: %FG%;
}
QLineEdit:focus, QComboBox:focus { border-color: %ACC%; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow { image: url(%ARROW%); width: 10px; height: 10px; }
QComboBox QAbstractItemView { background: %BGL%; border: 1px solid %MUTED%; selection-background-color: %SEL%; outline: 0; }
QPushButton { background: %BGL%; border: 1px solid transparent; padding: 5px 12px; }
QPushButton:hover { background: %HOVER%; }
QPushButton:pressed { background: %SEL%; }
QPushButton:checked { background: %SEL%; border-color: %ACC%; }
QPushButton[role="primary"] { background: %ACC%; color: %ACCT%; font-weight: bold; }
QToolButton { background: transparent; border: none; padding: 4px; color: %FG%; }
QToolButton:hover { color: %ACC%; }
QToolButton:checked { color: %ACC%; }
QToolButton:disabled { color: %MUTED%; }
QToolButton::menu-indicator { image: none; width: 0; }
QToolButton[role="text"] { color: %DIM%; padding: 4px 8px; }
QToolButton[role="text"]:hover { color: %FG%; background: %BGL%; }
QToolButton[role="text"]:checked { color: %FG%; background: %BGL%; }
QToolButton[role="pagetab"] { color: %DIM%; font-size: 12px; padding: 3px 7px; border-bottom: 2px solid transparent; }
QToolButton[role="pagetab"]:hover { color: %FG%; }
QToolButton[role="pagetab"]:checked { color: %FG%; border-bottom: 2px solid %ACC%; }   /* like the list tabs */
QTabBar { background: %BGD%; }
QTabBar::tab { background: %BGD%; color: %DIM%; padding: 7px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %FG%; background: %BG%; border-bottom: 2px solid %ACC%; }
QTabBar::tab:hover { color: %FG%; }
QTabBar::close-button { image: none; }
QTableView, QListView {
  background: %BG%; alternate-background-color: %BGD%; border: none; outline: 0;
  selection-background-color: %SEL%; selection-color: %FG%; gridline-color: transparent;
}
QTableView { font-size: 12px; }   /* kept in step with Playlist's bold title font */
QTableView::item { padding: 0 6px; border: none; }
QTableView::item:selected { background: %SEL%; }
QHeaderView { background: %BG%; border: none; }
QHeaderView::section { background: %BG%; color: %DIM%; border: none; border-bottom: 1px solid %MUTED%; padding: 5px 6px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
QScrollBar::handle:vertical { background: %MUTED%; min-height: 24px; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }
QScrollBar::handle:horizontal { background: %MUTED%; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: %MUTED%; }
QMenu { background: %BGL%; border: 1px solid %MUTED%; padding: 4px; }
QMenu::item { padding: 5px 18px; }
QMenu::item:selected { background: %SEL%; color: %ACC%; }
QMenu::item:disabled { color: %DIM%; }
QMenu::separator { height: 1px; background: %MUTED%; margin: 4px 6px; }
QLabel { background: transparent; }
QLabel[role="dim"] { color: %DIM%; }
QLabel[role="title"] { font-size: 19px; font-weight: bold; }
QLabel[role="artist"] { color: %ACC%; font-size: 14px; }
QLabel[role="caption"] { color: %DIM%; font-size: 11px; }
QLabel[role="eqval"] { color: %DIM%; font-size: 10px; }
QFrame[role="bar"] { background: %BGD%; border-top: 1px solid %MUTED%; }
QFrame[role="bar"] QWidget { background: transparent; }
QFrame[role="panel"] { background: %BGD%; }
QFrame[role="panel"] QWidget { background: transparent; }
QFrame[role="panel"] QComboBox, QFrame[role="panel"] QLineEdit { background: %BG%; }
QSlider::groove:vertical { background: %MUTED%; width: 3px; }
QSlider::add-page:vertical { background: %ACC%; width: 3px; }
QSlider::sub-page:vertical { background: %MUTED%; width: 3px; }
QSlider::handle:vertical { background: %FG%; height: 8px; width: 16px; margin: 0 -7px; }
QSlider::handle:vertical:hover { background: %ACC%; }
QCheckBox { background: transparent; spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid %MUTED%; background: %BG%; }
QCheckBox::indicator:checked { background: %ACC%; border-color: %ACC%; }
)";
    s.replace("%BGD%", n(bgDark)).replace("%BGL%", n(bgLight)).replace("%BG%", n(bg));
    s.replace("%FG%", n(fg)).replace("%DIM%", n(fgDim)).replace("%MUTED%", n(muted));
    s.replace("%SEL%", n(selection)).replace("%ACCT%", n(accentText));
    s.replace("%ACC%", n(accent)).replace("%HOVER%", n(hover));
    // the combo arrow has to be an image; draw one in the theme colour
    const QString arrow = QDir::tempPath() + "/sonata-arrow.svg";
    QFile af(arrow);
    if (af.open(QIODevice::WriteOnly | QIODevice::Truncate))
        af.write(QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'><path d='M1 3 L5 7 L9 3' fill='none' stroke='%1' stroke-width='1.6'/></svg>")
                     .arg(fgDim.name()).toUtf8());
    s.replace("%ARROW%", arrow);
    return s;
}
