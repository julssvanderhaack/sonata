// Live spectrum: a 4096-point FFT of what is playing right now on a logarithmic
// frequency axis that runs up to the track's Nyquist frequency (half its sample
// rate), with slowly falling peaks. Clicking the view cycles through its styles
// (curve, bars, blocks, mirrored bars, line, waveform); right click lists them.
// Runs at ~30 fps and only while it is on screen.
#pragma once
#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <QWidget>
#include <complex>
#include <vector>

class Engine;

class SpectrumView : public QWidget {
    Q_OBJECT
public:
    enum Style { Curve, Bars, Blocks, Mirror, Line, Wave, StyleCount };
    explicit SpectrumView(Engine *engine, QWidget *parent = nullptr);
    int style() const { return style_; }
    void setStyle(int style, bool announce = false);   // announce: show its name for a moment

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    void step();
    void analyse(int rate, int points);
    QRect plotArea() const;
    void paintBackground();   // grid + labels, cached: only the curve is drawn each frame
    void paintCurve(QPainter &p, const QRectF &area, bool filled);
    void paintBars(QPainter &p, const QRectF &area);
    void paintWave(QPainter &p, const QRectF &area);

    Engine *engine_;
    QTimer timer_;
    QElapsedTimer clock_;
    std::vector<float> samples_, window_;
    std::vector<std::complex<float>> fft_, twiddle_;
    std::vector<unsigned> bitrev_;
    std::vector<float> target_, level_, peak_, peakHold_;
    int rate_ = 44100;
    int style_ = Curve;
    QElapsedTimer announced_;   // running while the style's name is on screen
    QPixmap bg_;
    QString bgKey_;
};
