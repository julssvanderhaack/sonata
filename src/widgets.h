// Small custom-painted widgets: glyph buttons, seek/volume bars, cover art.
#pragma once
#include <QImage>
#include <QPixmap>
#include <QToolButton>
#include <QWidget>

namespace Glyph {
// Material Design Icons as shipped in Nerd Fonts
inline const QString Play = QString::fromUcs4(U"\U000F040A");
inline const QString Pause = QString::fromUcs4(U"\U000F03E4");
inline const QString Next = QString::fromUcs4(U"\U000F04AD");
inline const QString Prev = QString::fromUcs4(U"\U000F04AE");
inline const QString Shuffle = QString::fromUcs4(U"\U000F049D");
inline const QString Repeat = QString::fromUcs4(U"\U000F0456");
inline const QString RepeatOne = QString::fromUcs4(U"\U000F0458");
inline const QString VolHigh = QString::fromUcs4(U"\U000F057E");
inline const QString VolMed = QString::fromUcs4(U"\U000F0580");
inline const QString VolLow = QString::fromUcs4(U"\U000F057F");
inline const QString VolOff = QString::fromUcs4(U"\U000F0581");
inline const QString Note = QString::fromUcs4(U"\U000F075A");
inline const QString Plus = QString::fromUcs4(U"\U000F0415");
inline const QString Folder = QString::fromUcs4(U"\U000F024B");
inline const QString Menu = QString::fromUcs4(U"\U000F01D8");
inline const QString Search = QString::fromUcs4(U"\U000F0349");
inline const QString Eq = QString::fromUcs4(U"\U000F00EC");
inline const QString Lyrics = QString::fromUcs4(U"\U000F036C");
inline const QString Sidebar = QString::fromUcs4(U"\U000F0CB8");
}

QFont iconFont(int px);

class GlyphButton : public QToolButton {
    Q_OBJECT
public:
    GlyphButton(const QString &glyph, int px, const QString &tip, QWidget *parent = nullptr);
    void setGlyph(const QString &g) { setText(g); }
    void setPrimary(bool p) { primary_ = p; update(); }

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { hover_ = true; update(); }
    void leaveEvent(QEvent *) override { hover_ = false; update(); }

private:
    int px_;
    bool hover_ = false, primary_ = false;
};

// Thin horizontal bar used for both seeking and volume.
class Bar : public QWidget {
    Q_OBJECT
public:
    explicit Bar(QWidget *parent = nullptr);
    void setRange(double max)
    {
        if (max == max_) return;   // called on every tick
        max_ = max;
        update();
    }
    void setValue(double v);
    double value() const { return value_; }
    bool dragging() const { return drag_; }
    void setTimeTips(bool on) { timeTips_ = on; }

signals:
    void moved(double v);       // while dragging / clicking
    void released(double v);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void enterEvent(QEnterEvent *) override { hover_ = true; update(); }
    void leaveEvent(QEvent *) override { hover_ = false; hoverX_ = -1; update(); }
    QSize sizeHint() const override { return {200, 18}; }

private:
    double at(double x) const;
    double max_ = 1, value_ = 0;
    bool drag_ = false, hover_ = false, timeTips_ = false;
    double hoverX_ = -1;
};

class CoverView : public QWidget {
    Q_OBJECT
public:
    explicit CoverView(QWidget *parent = nullptr);
    void setImage(const QImage &img);
    const QImage &image() const { return img_; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }
    QSize sizeHint() const override { return {320, 320}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override { scaled_ = {}; }

private:
    QImage img_;
    QPixmap scaled_;
};
