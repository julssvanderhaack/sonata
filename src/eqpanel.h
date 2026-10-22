#pragma once
#include "eq.h"
#include <QJsonObject>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;

class EqCurve : public QWidget {
public:
    explicit EqCurve(QWidget *parent = nullptr) : QWidget(parent) { setMinimumHeight(70); }
    void setSettings(const EqSettings &s) { s_ = s; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    EqSettings s_;
};

class EqPanel : public QWidget {
    Q_OBJECT
public:
    explicit EqPanel(QWidget *parent = nullptr);
    void setSettings(const EqSettings &s);
    EqSettings settings() const { return s_; }
    QJsonObject customPresets() const;
    void setCustomPresets(const QJsonObject &o);
    QString presetName() const;

signals:
    void changed(const EqSettings &s);

private:
    void fromSliders();
    void syncUi();
    void refillPresets();
    void applyPreset(int idx);

    EqSettings s_;
    QVector<EqPreset> custom_;
    QCheckBox *enabled_, *auto_;
    QComboBox *presets_;
    QSlider *pre_;
    QLabel *preVal_;
    QSlider *band_[kBands];
    QLabel *val_[kBands];
    EqCurve *curve_;
    bool syncing_ = false;
};
