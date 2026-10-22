#include "lyricsview.h"
#include "theme.h"
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QWheelEvent>

static QFont lyricFont(int width)
{
    QFont f;
    f.setPixelSize(std::clamp(width / 26, 15, 22));
    return f;
}

LyricsView::LyricsView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(60);
    setAttribute(Qt::WA_OpaquePaintEvent);   // paints its own background: parents stay untouched
    anim_.setDuration(420);
    anim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { setScroll(v.toDouble()); });
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &LyricsView::onTimer);
}

void LyricsView::setLyrics(const Lyrics &l)
{
    lyr_ = l;
    cur_ = -1;
    manual_ = 0;
    manualAt_.invalidate();
    relayout();
    anim_.stop();
    scroll_ = targetScroll();
    setPosition(now(), dur_);
    update();
}

void LyricsView::setStatus(const QString &s)
{
    status_ = s;
    update();
}

void LyricsView::relayout()
{
    const QFont f = lyricFont(width());
    font_ = f;
    boldFont_ = f;
    boldFont_.setBold(true);
    const QFontMetricsF fm(f);
    const double w = std::max(60, width() - 48);
    textW_ = w;
    page_ = QPixmap();
    fade_ = QPixmap();
    plain_ = QVector<QStaticText>(lyr_.lines.size());
    bold_ = QVector<QStaticText>(lyr_.lines.size());
    top_.resize(lyr_.lines.size());
    height_.resize(lyr_.lines.size());
    double y = 0;
    const double gap = fm.height() * (lyr_.synced ? 0.55 : 0.25);
    for (int i = 0; i < lyr_.lines.size(); ++i) {
        const QString &txt = lyr_.lines[i].text;
        double h = txt.isEmpty() ? fm.height() * (lyr_.synced ? 0.6 : 0.8)
                                 : fm.boundingRect(QRectF(0, 0, w, 1e6), Qt::AlignHCenter | Qt::TextWordWrap, txt).height();
        top_[i] = y;
        height_[i] = h;
        y += h + gap;
    }
    contentH_ = y;
}

void LyricsView::resizeEvent(QResizeEvent *)
{
    relayout();
    anim_.stop();
    scroll_ = targetScroll();
}

void LyricsView::showEvent(QShowEvent *)
{
    setPosition(now(), dur_);
}

void LyricsView::hideEvent(QHideEvent *)
{
    timer_.stop();
    anim_.stop();
}

QStaticText &LyricsView::staticFor(int i, bool bold)
{
    QStaticText &st = (bold ? bold_ : plain_)[i];
    if (st.text().isEmpty()) {
        st.setTextFormat(Qt::PlainText);
        st.setTextWidth(textW_);
        QTextOption o(Qt::AlignHCenter);
        o.setWrapMode(QTextOption::WordWrap);
        st.setTextOption(o);
        st.setPerformanceHint(QStaticText::AggressiveCaching);
        st.setText(lyr_.lines[i].text);
        st.prepare(QTransform(), bold ? boldFont_ : font_);
    }
    return st;
}

bool LyricsView::buildPage()
{
    const Theme &t = Theme::get();
    const double dpr = devicePixelRatioF();
    if (!page_.isNull() && pageColor_ == t.fg && page_.devicePixelRatio() == dpr) return true;
    const QSize sz(width(), int(std::ceil(contentH_)) + 8);
    if (double(sz.width()) * sz.height() * dpr * dpr * 4 > 32e6) return false;   // very long text: draw live
    page_ = QPixmap(sz * dpr);
    page_.setDevicePixelRatio(dpr);
    page_.fill(t.bg);
    QPainter p(&page_);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font_);
    p.setPen(t.fg);
    for (int i = 0; i < lyr_.lines.size(); ++i)
        if (!lyr_.lines[i].text.isEmpty()) p.drawStaticText(QPointF((width() - textW_) / 2, top_[i]), staticFor(i, false));
    pageColor_ = t.fg;
    return true;
}

double LyricsView::now() const
{
    return pos_ + (playing_ && since_.isValid() ? since_.nsecsElapsed() / 1e9 : 0);
}

void LyricsView::setPlaying(bool playing)
{
    if (playing_ == playing) return;
    pos_ = now();
    since_.start();
    playing_ = playing;
    schedule();
}

void LyricsView::setScroll(double y)
{
    // whole device pixels: no blurry half-pixel text, no repaint for sub-pixel moves
    const double dpr = devicePixelRatioF();
    y = std::round(y * dpr) / dpr;
    if (y == scroll_) return;
    scroll_ = y;
    update();
}

void LyricsView::onTimer()
{
    setPosition(now(), dur_);
}

void LyricsView::schedule()
{
    timer_.stop();
    if (!playing_ || !isVisible() || lyr_.lines.isEmpty()) return;
    const double t = now();
    if (lyr_.synced) {
        // wake up exactly when the next line starts
        const int next = cur_ + 1;
        if (next >= lyr_.lines.size()) return;
        const double at = lyr_.lines[next].t - offset_ - 0.15;
        timer_.start(std::clamp(int((at - t) * 1000) + 1, 1, 60000));
    } else {
        // drift: one device pixel per wakeup
        if (contentH_ <= height() || dur_ <= 0 || t < 15 || t > dur_ - 25) {
            if (t < 15) timer_.start(int((15 - t) * 1000) + 1);
            return;
        }
        const double pxPerSec = (contentH_ - height() + 48) / std::max(1.0, dur_ - 40) * devicePixelRatioF();
        timer_.start(std::clamp(int(1000 / std::max(pxPerSec, 0.01)), 16, 60000));
    }
}

double LyricsView::targetScroll() const
{
    const double view = height();
    if (manualAt_.isValid() && manualAt_.elapsed() < 3500) return manual_;
    if (lyr_.synced) {
        if (cur_ < 0) return -view / 2 + (top_.isEmpty() ? 0 : height_[0] / 2) + 40;
        return top_[cur_] + height_[cur_] / 2 - view * 0.45;
    }
    // unsynced: drift through the text in step with the song
    if (contentH_ <= view || dur_ <= 0) return -std::max(0.0, (view - contentH_) / 2);
    const double frac = std::clamp((pos_ - 15) / std::max(1.0, dur_ - 40), 0.0, 1.0);
    return -24 + frac * (contentH_ - view + 48);
}

void LyricsView::glideTo(double y)
{
    if (std::abs(y - scroll_) < 0.5) return;
    if (!isVisible()) {
        scroll_ = y;
        return;
    }
    anim_.stop();
    anim_.setStartValue(scroll_);
    anim_.setEndValue(y);
    anim_.start();
}

void LyricsView::setPosition(double sec, double duration)
{
    pos_ = sec;
    dur_ = duration;
    since_.start();
    if (!isVisible()) return;
    if (lyr_.synced) {
        const double t = sec + offset_ + 0.15;   // a hair early reads naturally
        int c = -1;
        int lo = 0, hi = lyr_.lines.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (lyr_.lines[mid].t <= t) {
                c = mid;
                lo = mid + 1;
            } else hi = mid - 1;
        }
        if (c != cur_) {
            cur_ = c;
            glideTo(targetScroll());
            update();
        } else if (manualAt_.isValid() && manualAt_.elapsed() >= 3500) {
            manualAt_.invalidate();
            glideTo(targetScroll());
        }
    } else if (!anim_.state() && !lyr_.lines.isEmpty()) {
        // the extrapolated clock may run a hair ahead of the next tick: never step back for that
        const double y = targetScroll();
        if (y >= scroll_ || scroll_ - y > 2.5 / devicePixelRatioF()) setScroll(y);
    }
    schedule();
}

int LyricsView::lineAt(double y) const
{
    const double cy = y + scroll_;
    for (int i = 0; i < top_.size(); ++i)
        if (cy >= top_[i] - 4 && cy <= top_[i] + height_[i] + 4) return i;
    return -1;
}

void LyricsView::wheelEvent(QWheelEvent *e)
{
    if (lyr_.lines.isEmpty()) return;
    anim_.stop();
    manual_ = (manualAt_.isValid() ? manual_ : scroll_) - e->angleDelta().y() * 0.6;
    manual_ = std::clamp(manual_, -height() / 2.0, std::max(0.0, contentH_ - height() / 2.0));
    manualAt_.start();
    scroll_ = manual_;
    update();
}

void LyricsView::mouseMoveEvent(QMouseEvent *e)
{
    const int l = lyr_.synced ? lineAt(e->position().y()) : -1;
    if (l != hoverLine_) {
        hoverLine_ = l;
        setCursor(l >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void LyricsView::mousePressEvent(QMouseEvent *e)
{
    if (!lyr_.synced || e->button() != Qt::LeftButton) return;
    const int l = lineAt(e->position().y());
    if (l >= 0) {
        manualAt_.invalidate();
        emit seekRequested(std::max(0.0, lyr_.lines[l].t - offset_));
    }
}

void LyricsView::paintEvent(QPaintEvent *e)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QFont &f = font_;
    if (lyr_.lines.isEmpty()) {
        p.fillRect(e->rect(), t.bg);
        QFont sf = f;
        sf.setPixelSize(std::max(13, f.pixelSize() - 3));
        p.setFont(sf);
        p.setPen(t.fgDim);
        p.drawText(rect().adjusted(20, 0, -20, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   lyr_.instrumental ? QString("♪  Instrumental  ♪") : status_);
        return;
    }
    const double w = textW_;
    const int view = height();
    if (!lyr_.synced && buildPage()) {
        p.setCompositionMode(QPainter::CompositionMode_Source);   // opaque: a plain copy
        p.drawPixmap(QPointF(0, -scroll_), page_);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // the page is opaque, so only what it doesn't cover needs the background
        if (-scroll_ > 0) p.fillRect(QRectF(0, 0, width(), -scroll_), t.bg);
        const double pageEnd = -scroll_ + page_.height() / page_.devicePixelRatio();
        if (pageEnd < view) p.fillRect(QRectF(0, pageEnd, width(), view - pageEnd), t.bg);
        // soft fade at the top and bottom edges, painted over the text
        // (blending is the costliest part of a frame: keep the band small in short views)
        const double dpr = devicePixelRatioF();
        const int fh = std::clamp(view / 6, 12, 60);
        if (fade_.isNull() || fade_.width() != qRound(width() * dpr) || fade_.height() != qRound(fh * dpr) || fade_.devicePixelRatio() != dpr) {
            QImage img(qRound(width() * dpr), qRound(fh * dpr), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < img.height(); ++y) {
                QColor c = t.bg;
                c.setAlphaF(1.0 - double(y) / img.height());
                const QRgb px = qPremultiply(c.rgba());
                std::fill_n(reinterpret_cast<QRgb *>(img.scanLine(y)), img.width(), px);
            }
            img.setDevicePixelRatio(dpr);
            fade_ = QPixmap::fromImage(img);
            fadeBottom_ = QPixmap::fromImage(img.flipped(Qt::Vertical));
        }
        p.drawPixmap(QPointF(0, 0), fade_);
        p.drawPixmap(QPointF(0, view - fh), fadeBottom_);
        return;
    }
    p.fillRect(e->rect(), t.bg);
    for (int i = 0; i < lyr_.lines.size(); ++i) {
        const double y = top_[i] - scroll_;
        if (y + height_[i] < 0 || y > view) continue;
        const QString &txt = lyr_.lines[i].text;
        if (txt.isEmpty()) {
            if (i == cur_) {   // instrumental break: three dots
                p.setPen(Qt::NoPen);
                p.setBrush(t.accent);
                for (int k = -1; k <= 1; ++k) p.drawEllipse(QPointF(width() / 2.0 + k * 12, y + height_[i] / 2), 2.5, 2.5);
            }
            continue;
        }
        QColor c;
        if (!lyr_.synced) c = t.fg;
        else if (i == cur_) c = t.accent;
        else if (i == hoverLine_) c = t.fg;
        else {
            // fade with distance from the current line
            const int d = std::abs(i - (cur_ < 0 ? 0 : cur_));
            c = i < cur_ ? t.fgDim : t.fg;
            c.setAlphaF(std::clamp(1.0 - d * 0.12, 0.35, i < cur_ ? 0.8 : 0.85));
        }
        // soft fade at the top and bottom edges
        const double edge = std::min(y + height_[i] / 2, view - (y + height_[i] / 2));
        if (edge < 60) c.setAlphaF(c.alphaF() * std::clamp(edge / 60.0, 0.0, 1.0));
        const bool bold = i == cur_ && lyr_.synced;
        p.setFont(bold ? boldFont_ : f);
        p.setPen(c);
        p.drawStaticText(QPointF((width() - w) / 2, y), staticFor(i, bold));
    }
}
