#include "PdfPreview.h"

#include "core/WorkerPools.h"

#include <KLocalizedString>

#include <poppler-qt6.h>

#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
constexpr int kPageGap = 8;
constexpr int kMargin  = 8;
}

// ── Strip: the tall widget inside the scroll area ────────────────────────────

class PdfPreview::Strip : public QWidget
{
public:
    explicit Strip(PdfPreview *owner) : QWidget(owner), m_owner(owner) {}

    // Top y of each page at the current scale; the last entry is the total.
    QList<int> m_offsets;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter p(this);
        const QRect clip = event->rect();
        const int n = m_owner->pageCount();
        const qreal dpr = devicePixelRatioF();

        for (int i = 0; i < n; ++i) {
            const int top    = m_offsets.value(i);
            const int bottom = m_offsets.value(i + 1) - kPageGap;
            if (bottom < clip.top())
                continue;
            if (top > clip.bottom())
                break;

            const QSizeF pts = m_owner->m_pageSizes.at(i);
            const int w = qRound(pts.width()  * m_owner->m_scale);
            const int h = qRound(pts.height() * m_owner->m_scale);
            const QRect pageRect((width() - w) / 2, top, w, h);

            bool stale = false;
            const QImage img = m_owner->cachedImage(i, stale);
            if (img.isNull()) {
                p.fillRect(pageRect, palette().color(QPalette::Base));
                m_owner->requestPage(i);
            } else {
                // Cached at this scale: 1:1 in device pixels. Stale: scaled
                // to the new rect until the fresh render replaces it.
                if (!stale && img.size() == QSize(w, h) * dpr) {
                    p.drawImage(pageRect.topLeft(), img);
                } else {
                    p.drawImage(pageRect, img);
                    if (stale)
                        m_owner->requestPage(i);
                }
            }
            p.setPen(palette().color(QPalette::Mid));
            p.drawRect(pageRect.adjusted(0, 0, -1, -1));
        }
    }

private:
    PdfPreview *m_owner;
};

// ── PdfPreview ───────────────────────────────────────────────────────────────

PdfPreview::PdfPreview(QWidget *parent)
    : QWidget(parent)
{
    m_renderLock = std::make_shared<QMutex>();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(false);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_strip = new Strip(this);
    m_scroll->setWidget(m_strip);

    m_info = new QLabel(this);
    m_info->setAlignment(Qt::AlignCenter);
    m_info->setWordWrap(true);
    m_info->setContentsMargins(8, 4, 8, 4);

    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_info);

    // Resize → new scale, but not per pixel of a splitter drag.
    m_rescale = new QTimer(this);
    m_rescale->setSingleShot(true);
    m_rescale->setInterval(150);
    connect(m_rescale, &QTimer::timeout, this, [this]() {
        if (m_doc)
            relayout();
    });

    m_loadWatcher = new QFutureWatcher<Loaded>(this);
    connect(m_loadWatcher, &QFutureWatcher<Loaded>::finished, this, &PdfPreview::onLoaded);

    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PdfPreview::evictFarPages);
}

// No waits. Loads capture only the path; renders hold a shared_ptr to the
// document and the lock, so both can finish on their own after we are gone,
// and their watchers — our children — disconnect from the futures on
// destruction. The pool is process-wide (WorkerPools::pdf) for the same
// reason: a member QThreadPool waited for its tasks in its destructor, which
// froze closing the window on a PDF stuck on a hung mount.
PdfPreview::~PdfPreview() = default;

void PdfPreview::bumpGeneration()
{
    ++m_generation;
    m_liveGeneration->store(m_generation);
}

void PdfPreview::clear()
{
    bumpGeneration();
    m_doc.reset();
    m_pageSizes.clear();
    m_cache.clear();
    m_staleCache.clear();
    m_inflight.clear();
    m_wanted->first.store(0);
    m_wanted->last.store(std::numeric_limits<int>::max());
    m_strip->m_offsets.clear();
    m_strip->resize(0, 0);
    m_info->clear();
}

void PdfPreview::setDocument(const QUrl &url)
{
    clear();
    if (!url.isLocalFile()) {
        m_info->setText(i18n("PDF preview is only available for local files."));
        return;
    }
    m_info->setText(i18n("Loading…"));
    const int generation = m_generation;
    m_loadWatcher->setProperty("generation", generation);
    const QString path = url.toLocalFile();
    const auto live = m_liveGeneration;
    m_loadWatcher->setFuture(QtConcurrent::run(WorkerPools::pdf(), [path, live, generation]() -> Loaded {
        Loaded out;
        if (live->load() != generation) {
            out.skipped = true;
            return out;
        }
        std::shared_ptr<Poppler::Document> doc = Poppler::Document::load(path);
        if (!doc) {
            out.error = i18n("Cannot open this PDF.");
            return out;
        }
        if (doc->isLocked()) {
            out.error = i18n("This PDF is password protected.");
            return out;
        }
        doc->setRenderHint(Poppler::Document::Antialiasing);
        doc->setRenderHint(Poppler::Document::TextAntialiasing);
        doc->setRenderHint(Poppler::Document::TextHinting);
        const int n = doc->numPages();
        out.pageSizes.reserve(n);
        for (int i = 0; i < n; ++i) {
            std::unique_ptr<Poppler::Page> page = doc->page(i);
            out.pageSizes.append(page ? page->pageSizeF() : QSizeF(595, 842));
        }
        out.doc = std::move(doc);
        return out;
    }));
}

void PdfPreview::onLoaded()
{
    if (m_loadWatcher->property("generation").toInt() != m_generation)
        return;
    const Loaded l = m_loadWatcher->result();
    if (l.skipped)
        return;
    if (!l.error.isEmpty()) {
        m_info->setText(l.error);
        return;
    }
    m_doc       = l.doc;
    m_pageSizes = l.pageSizes;
    m_info->setText(i18np("%1 page", "%1 pages", m_pageSizes.size()));
    relayout();
}

double PdfPreview::fitScale() const
{
    double widest = 1.0;
    for (const QSizeF &s : m_pageSizes)
        widest = qMax(widest, s.width());
    const int avail = m_scroll->viewport()->width() - 2 * kMargin;
    return qMax(avail, 32) / widest;
}

void PdfPreview::relayout()
{
    const double scale = fitScale();
    if (!qFuzzyCompare(scale, m_scale)) {
        // Everything rendered so far is at the wrong size. Keep it as a
        // stand-in; the strip re-requests each visible page at the new scale.
        for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
            m_staleCache.insert(it.key(), it.value());
        m_cache.clear();
        m_inflight.clear();
        bumpGeneration(); // in-flight renders at the old scale are dropped
        m_scale = scale;
    }

    m_strip->m_offsets.clear();
    m_strip->m_offsets.reserve(m_pageSizes.size() + 1);
    int y = kMargin;
    for (const QSizeF &s : m_pageSizes) {
        m_strip->m_offsets.append(y);
        y += qRound(s.height() * m_scale) + kPageGap;
    }
    m_strip->m_offsets.append(y);
    m_strip->resize(m_scroll->viewport()->width(), y - kPageGap + kMargin);
    m_strip->update();
    evictFarPages(); // also sets the wanted range for the new layout
}

void PdfPreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_doc)
        return;
    // Keep the strip as wide as the viewport right away so the stale images
    // scale visibly with the drag; the real re-render waits for the timer.
    m_strip->resize(m_scroll->viewport()->width(), m_strip->height());
    m_rescale->start();
}

QImage PdfPreview::cachedImage(int page, bool &stale) const
{
    stale = false;
    const auto it = m_cache.constFind(page);
    if (it != m_cache.cend())
        return it.value();
    const auto st = m_staleCache.constFind(page);
    if (st != m_staleCache.cend()) {
        stale = true;
        return st.value();
    }
    return {};
}

void PdfPreview::requestPage(int page)
{
    if (!m_doc || m_inflight.contains(page) || m_cache.contains(page))
        return;
    m_inflight.insert(page);

    const int generation = m_generation;
    const double scale   = m_scale;
    const qreal dpr      = devicePixelRatioF();
    auto doc  = m_doc;
    auto lock = m_renderLock;
    auto live = m_liveGeneration;
    auto wanted = m_wanted;

    auto *watcher = new QFutureWatcher<Rendered>(this);
    watcher->setProperty("generation", generation);
    m_renderWatchers.append(watcher);
    connect(watcher, &QFutureWatcher<Rendered>::finished, this, [this, watcher]() {
        m_renderWatchers.removeOne(watcher);
        watcher->deleteLater();
        if (watcher->property("generation").toInt() != m_generation)
            return;
        const Rendered r = watcher->result();
        m_inflight.remove(r.page);
        if (r.image.isNull())
            return;
        m_cache.insert(r.page, r.image);
        m_staleCache.remove(r.page);
        m_strip->update();
    });

    watcher->setFuture(QtConcurrent::run(WorkerPools::pdf(), [doc, lock, live, wanted, generation, page, scale, dpr]() -> Rendered {
        Rendered r;
        r.page  = page;
        r.scale = scale;
        const auto worthIt = [&]() {
            return live->load() == generation
                   && page >= wanted->first.load() && page <= wanted->last.load();
        };
        if (!worthIt())
            return r; // scrolled past, resized or replaced while queued
        QMutexLocker locker(lock.get());
        if (!worthIt())
            return r; // went out of range while waiting for the document
        std::unique_ptr<Poppler::Page> p = doc->page(page);
        if (!p)
            return r;
        const double dpi = 72.0 * scale * dpr;
        r.image = p->renderToImage(dpi, dpi);
        r.image.setDevicePixelRatio(dpr);
        return r;
    }));
}

void PdfPreview::evictFarPages()
{
    const QList<int> &off = m_strip->m_offsets;
    if (off.size() < 2)
        return;
    const int top    = m_scroll->verticalScrollBar()->value();
    const int bottom = top + m_scroll->viewport()->height();
    // First page whose bottom is below the viewport top, last page whose
    // top is above the viewport bottom.
    int first = 0;
    while (first + 1 < off.size() - 1 && off[first + 1] - kPageGap < top)
        ++first;
    int last = first;
    while (last + 1 < off.size() - 1 && off[last + 1] <= bottom)
        ++last;
    constexpr int kKeep = 3;
    m_wanted->first.store(first - kKeep);
    m_wanted->last.store(last + kKeep);
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        if (it.key() < first - kKeep || it.key() > last + kKeep)
            it = m_cache.erase(it);
        else
            ++it;
    }
    for (auto it = m_staleCache.begin(); it != m_staleCache.end(); ) {
        if (it.key() < first - kKeep || it.key() > last + kKeep)
            it = m_staleCache.erase(it);
        else
            ++it;
    }
}
