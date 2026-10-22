// Live spectrum: a 4096-point FFT of what is playing right now, drawn as bars
// on a logarithmic frequency axis with falling peak caps. Runs at ~30 fps and
// only while it is on screen.
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
    explicit SpectrumView(Engine *engine, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    void step();
    void analyse(int rate, int bands);
    QRect barArea() const;
    void paintBackground();   // grid + labels, cached: only the bars are drawn each frame

    Engine *engine_;
    QTimer timer_;
    QElapsedTimer clock_;
    std::vector<float> samples_, window_;
    std::vector<std::complex<float>> fft_, twiddle_;
    std::vector<unsigned> bitrev_;
    std::vector<float> target_, level_, peak_, peakHold_;
    int rate_ = 44100;
    QPixmap bg_;
    QString bgKey_;
};
