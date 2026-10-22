#include "eqpanel.h"
#include "theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QLocale>

static QString bandLabel(double hz)
{
    return hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz);
}

static QString db(float v)
{
    return (v > 0 ? "+" : "") + QString::number(v, 'f', v == int(v) ? 0 : 1);
}

// Magnitude of the cascaded peaking filters (same maths as Dsp) at 48 kHz.
void EqCurve::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double W = width(), H = height(), range = 18;
    auto yOf = [&](double dbv) { return H / 2 - dbv / range * (H / 2 - 4); };
    p.setPen(QPen(t.muted, 1, Qt::DotLine));
    p.drawLine(QPointF(0, yOf(0)), QPointF(W, yOf(0)));
    if (!s_.enabled) {
        p.setPen(t.fgDim);
        p.drawText(rect(), Qt::AlignCenter, "Ecualizador desactivado");
        return;
    }
    // the shape the sliders ask for (plus the manual preamp); auto headroom is shown as a caption
    QPainterPath path;
    for (int x = 0; x <= W; x += 2) {
        const double f = 20 * std::pow(1000.0, x / W);   // 20 Hz … 20 kHz
        const QPointF pt(x, yOf(curveDb(s_.gains, f, 48000) + s_.preamp));
        x == 0 ? path.moveTo(pt) : path.lineTo(pt);
    }
    QPainterPath fill = path;
    fill.lineTo(W, yOf(0));
    fill.lineTo(0, yOf(0));
    QColor a = t.accent;
    a.setAlphaF(0.15);
    p.fillPath(fill, a);
    p.setPen(QPen(t.accent, 2));
    p.drawPath(path);
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(t.fgDim);
    if (s_.autoPreamp) {
        const double head = curvePeakDb(s_.gains);
        if (head > 0.05)
            p.drawText(rect().adjusted(0, 2, -4, 0), Qt::AlignRight | Qt::AlignTop,
                       QString("volumen −%1 dB para no saturar").arg(QLocale("es_ES").toString(head, 'f', 1)));
    }
}

EqPanel::EqPanel(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(10);

    auto *top = new QHBoxLayout;
    enabled_ = new QCheckBox("Activado");
    presets_ = new QComboBox;
    presets_->setMinimumWidth(150);
    auto *save = new QPushButton("Guardar");
    auto *del = new QPushButton("Borrar");
    save->setToolTip("Guardar estos ajustes como preajuste propio");
    del->setToolTip("Borrar el preajuste propio seleccionado");
    top->addWidget(enabled_);
    top->addStretch();
    top->addWidget(presets_);
    top->addWidget(save);
    top->addWidget(del);
    v->addLayout(top);

    curve_ = new EqCurve;
    curve_->setMinimumHeight(50);
    curve_->setMaximumHeight(150);
    v->addWidget(curve_, 2);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    auto mkSlider = [&](int col, const QString &label, QSlider *&s, QLabel *&val) {
        s = new QSlider(Qt::Vertical);
        s->setRange(-120, 120);   // tenths of dB
        s->setPageStep(10);
        s->setMinimumHeight(100);
        s->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        val = new QLabel("0");
        val->setProperty("role", "eqval");
        val->setAlignment(Qt::AlignCenter);
        auto *l = new QLabel(label);
        l->setProperty("role", "caption");
        l->setAlignment(Qt::AlignCenter);
        grid->addWidget(val, 0, col, Qt::AlignHCenter);
        grid->addWidget(s, 1, col, Qt::AlignHCenter);
        grid->setRowStretch(1, 1);
        grid->addWidget(l, 2, col, Qt::AlignHCenter);
    };
    mkSlider(0, "Pre", pre_, preVal_);
    grid->setColumnMinimumWidth(1, 10);
    for (int b = 0; b < kBands; ++b) mkSlider(b + 2, bandLabel(kBandHz[b]), band_[b], val_[b]);
    v->addLayout(grid, 3);

    auto_ = new QCheckBox("Evitar saturación (baja el volumen lo que suban las bandas)");
    auto_->setProperty("role", "caption");
    v->addWidget(auto_);


    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) return;
        s_.enabled = on;
        syncUi();
        emit changed(s_);
    });
    connect(auto_, &QCheckBox::toggled, this, [this](bool on) {
        if (syncing_) return;
        s_.autoPreamp = on;
        syncUi();
        emit changed(s_);
    });
    connect(pre_, &QSlider::valueChanged, this, &EqPanel::fromSliders);
    for (auto *s : band_) {
        connect(s, &QSlider::valueChanged, this, &EqPanel::fromSliders);
    }
    connect(presets_, &QComboBox::activated, this, &EqPanel::applyPreset);
    connect(save, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Guardar preajuste", "Nombre:", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        custom_.erase(std::remove_if(custom_.begin(), custom_.end(), [&](auto &p) { return p.name == name; }), custom_.end());
        custom_.push_back({name, s_.gains});
        refillPresets();
        emit changed(s_);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const QString name = presets_->currentText();
        custom_.erase(std::remove_if(custom_.begin(), custom_.end(), [&](auto &p) { return p.name == name; }), custom_.end());
        refillPresets();
        emit changed(s_);
    });
    refillPresets();
    syncUi();
}

void EqPanel::refillPresets()
{
    syncing_ = true;
    presets_->clear();
    for (const auto &p : builtinPresets()) presets_->addItem(p.name);
    if (!custom_.isEmpty()) presets_->insertSeparator(presets_->count());
    for (const auto &p : custom_) presets_->addItem(p.name);
    syncing_ = false;
    syncUi();
}

QString EqPanel::presetName() const
{
    for (const auto &p : builtinPresets())
        if (p.gains == s_.gains) return p.name;
    for (const auto &p : custom_)
        if (p.gains == s_.gains) return p.name;
    return "Personalizado";
}

void EqPanel::applyPreset(int idx)
{
    const QString name = presets_->itemText(idx);
    for (const auto &list : {builtinPresets(), custom_})
        for (const auto &p : list)
            if (p.name == name) {
                s_.gains = p.gains;
                s_.enabled = true;
                syncUi();
                emit changed(s_);
                return;
            }
}

void EqPanel::fromSliders()
{
    if (syncing_) return;
    s_.preamp = pre_->value() / 10.0f;
    for (int b = 0; b < kBands; ++b) s_.gains[b] = band_[b]->value() / 10.0f;
    // snap to half dB so presets compare equal and values read cleanly
    s_.preamp = std::round(s_.preamp * 2) / 2;
    for (float &g : s_.gains) g = std::round(g * 2) / 2;
    s_.enabled = true;
    syncUi();
    emit changed(s_);
}

void EqPanel::syncUi()
{
    syncing_ = true;
    enabled_->setChecked(s_.enabled);
    auto_->setChecked(s_.autoPreamp);
    pre_->setValue(int(s_.preamp * 10));
    preVal_->setText(db(s_.preamp));
    for (int b = 0; b < kBands; ++b) {
        band_[b]->setValue(int(s_.gains[b] * 10));
        val_[b]->setText(db(s_.gains[b]));
    }
    const QString name = presetName();
    int i = presets_->findText(name);
    if (i < 0) {
        if (presets_->itemText(presets_->count() - 1) != "Personalizado") presets_->addItem("Personalizado");
        i = presets_->count() - 1;
    } else if (name != "Personalizado") {
        int pi = presets_->findText("Personalizado");
        if (pi >= 0) presets_->removeItem(pi);
    }
    presets_->setCurrentIndex(i);
    curve_->setSettings(s_);
    syncing_ = false;
}

void EqPanel::setSettings(const EqSettings &s)
{
    s_ = s;
    syncUi();
}

QJsonObject EqPanel::customPresets() const
{
    QJsonObject o;
    for (const auto &p : custom_) {
        QJsonArray a;
        for (float g : p.gains) a << g;
        o[p.name] = a;
    }
    return o;
}

void EqPanel::setCustomPresets(const QJsonObject &o)
{
    custom_.clear();
    for (auto it = o.begin(); it != o.end(); ++it) {
        EqPreset p{it.key(), {}};
        const QJsonArray a = it.value().toArray();
        for (int b = 0; b < kBands && b < a.size(); ++b) p.gains[b] = float(a[b].toDouble());
        custom_ << p;
    }
    refillPresets();
}
