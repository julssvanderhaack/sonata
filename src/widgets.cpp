#include "widgets.h"
#include "meta.h"
#include "theme.h"
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

QFont iconFont(int px)
{
    QFont f(Theme::get().iconFont);
    f.setPixelSize(px);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

GlyphButton::GlyphButton(const QString &glyph, int px, const QString &tip, QWidget *parent)
    : QToolButton(parent), px_(px)
{
    setText(glyph);
    setToolTip(tip);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(px + 14, px + 12);
}

void GlyphButton::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor c = !isEnabled() ? t.muted : (isChecked() || hover_) ? t.accent : t.fg;
    if (primary_) {
        const int d = std::min(width(), height()) - 2;
        QRectF r((width() - d) / 2.0, (height() - d) / 2.0, d, d);
        p.setPen(Qt::NoPen);
        p.setBrush(hover_ ? t.accent.lighter(112) : t.accent);
        p.drawEllipse(r);
        c = t.accent.lightness() > 140 ? t.bg : t.fg;
    }
    p.setFont(iconFont(px_));
    p.setPen(c);
    p.drawText(rect(), Qt::AlignCenter, text());
    if (isCheckable() && isChecked() && !primary_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.accent);
        p.drawEllipse(QPointF(width() / 2.0, height() - 2.5), 1.6, 1.6);
    }
}

// ──────────────── Bar ────────────────

Bar::Bar(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(60);
    setFixedHeight(18);
}

void Bar::setValue(double v)
{
    if (drag_) return;
    v = std::clamp(v, 0.0, max_);
    // repaint only when the visible fill actually moves a pixel
    const double w = width() - 12;
    if (max_ > 0 && std::abs((v - value_) / max_ * w) < 0.5 && v != 0) {
        value_ = v;
        return;
    }
    value_ = v;
    update();
}

double Bar::at(double x) const
{
    const double w = std::max(1, width() - 12);
    return std::clamp((x - 6) / w, 0.0, 1.0) * max_;
}

void Bar::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double w = width() - 12, y = height() / 2.0;
    const double h = (hover_ || drag_) ? 4 : 3;
    const double frac = max_ > 0 ? value_ / max_ : 0;
    p.setPen(Qt::NoPen);
    p.setBrush(t.muted);
    p.drawRect(QRectF(6, y - h / 2, w, h));
    if (hoverX_ >= 0 && timeTips_) {
        QColor hc = t.accent;
        hc.setAlphaF(0.25);
        p.setBrush(hc);
        p.drawRect(QRectF(6, y - h / 2, std::clamp(hoverX_ - 6, 0.0, w), h));
    }
    p.setBrush(t.accent);
    p.drawRect(QRectF(6, y - h / 2, w * frac, h));
    if (hover_ || drag_) {
        p.setBrush(t.fg);
        p.drawEllipse(QPointF(6 + w * frac, y), 5.5, 5.5);
    }
}

void Bar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    drag_ = true;
    value_ = at(e->position().x());
    emit moved(value_);
    update();
}

void Bar::mouseMoveEvent(QMouseEvent *e)
{
    hoverX_ = e->position().x();
    if (drag_) {
        value_ = at(hoverX_);
        emit moved(value_);
    }
    if (timeTips_ && max_ > 0)
        QToolTip::showText(e->globalPosition().toPoint() - QPoint(0, 40), formatTime(at(hoverX_)), this);
    update();
}

void Bar::mouseReleaseEvent(QMouseEvent *e)
{
    if (!drag_) return;
    drag_ = false;
    value_ = at(e->position().x());
    emit released(value_);
    update();
}

void Bar::wheelEvent(QWheelEvent *e)
{
    const double step = (timeTips_ ? 5.0 : max_ * 0.05) * (e->angleDelta().y() > 0 ? 1 : -1);
    value_ = std::clamp(value_ + step, 0.0, max_);
    emit moved(value_);
    emit released(value_);
    update();
}

// ──────────────── CoverView ────────────────

CoverView::CoverView(QWidget *parent) : QWidget(parent)
{
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    setMinimumSize(0, 0);
}

void CoverView::setImage(const QImage &img)
{
    img_ = img;
    scaled_ = {};
    update();
}

void CoverView::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const int side = std::min(width(), height());
    const QRect box((width() - side) / 2, (height() - side) / 2, side, side);
    if (img_.isNull()) {
        p.fillRect(box, t.bgLight);
        p.setFont(iconFont(side / 3));
        p.setPen(t.muted);
        p.drawText(box, Qt::AlignCenter, Glyph::Note);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    if (scaled_.isNull()) {
        scaled_ = QPixmap::fromImage(img_.scaled(box.size() * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        scaled_.setDevicePixelRatio(dpr);
    }
    const QSizeF s = scaled_.deviceIndependentSize();
    p.drawPixmap(QPointF(box.x() + (side - s.width()) / 2, box.y() + (side - s.height()) / 2), scaled_);
}
