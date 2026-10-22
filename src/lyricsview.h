// Scrolling, karaoke-style lyrics: the current line sits in the middle and
// glides when the next one starts. Click a line to jump there.
//
// Cheap to keep on screen: line layouts are cached (QStaticText), the clock
// is extrapolated between the player's ticks so a line change is scheduled
// for its exact moment, and unsynced text drifts one device pixel at a time
// with a timer paced to the scroll speed instead of polling.
#pragma once
#include "lyrics.h"
#include <QElapsedTimer>
#include <QPixmap>
#include <QStaticText>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

class LyricsView : public QWidget {
    Q_OBJECT
public:
    explicit LyricsView(QWidget *parent = nullptr);
    void setLyrics(const Lyrics &l);
    void setStatus(const QString &s);              // shown when there are no lines
    void setPosition(double sec, double duration);
    void setPlaying(bool playing);
    void setOffset(double s) { offset_ = s; setPosition(now(), dur_); }
    double offset() const { return offset_; }
    const Lyrics &lyrics() const { return lyr_; }

signals:
    void seekRequested(double sec);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override { hoverLine_ = -1; update(); }

private:
    void relayout();
    int lineAt(double y) const;
    double targetScroll() const;
    void glideTo(double y);
    double now() const;          // playback position extrapolated from the last tick
    void schedule();             // arms the timer for the next line / drift pixel
    void onTimer();
    void setScroll(double y);    // repaints only when it moves a device pixel
    QStaticText &staticFor(int i, bool bold);
    bool buildPage();            // unsynced text pre-rendered once: each frame is a single blit

    Lyrics lyr_;
    QString status_;
    QVector<double> top_, height_;   // per line, in content coordinates
    double contentH_ = 0;
    int cur_ = -1, hoverLine_ = -1;
    double pos_ = 0, dur_ = 0, offset_ = 0;
    double scroll_ = 0, manual_ = 0;
    QElapsedTimer manualAt_;
    QVariantAnimation anim_;
    QTimer timer_;
    QElapsedTimer since_;        // time since pos_ was set
    bool playing_ = false;
    QVector<QStaticText> plain_, bold_;
    QFont font_, boldFont_;
    double textW_ = 0;
    QPixmap page_, fade_, fadeBottom_;   // pre-rendered text and edge fades
    QColor pageColor_;
};
