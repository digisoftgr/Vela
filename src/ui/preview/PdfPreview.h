#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QSizeF>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QScrollArea;
class QTimer;

namespace Poppler {
class Document;
}

/**
 * Continuous-scroll, fit-to-width PDF preview on Poppler.
 *
 * Only what is on screen is rendered: the page strip is a widget whose
 * height is known from the page sizes alone, and paintEvent asks for the
 * pages intersecting the viewport — a placeholder is drawn until the image
 * arrives. Rendering runs on one dedicated worker thread, because a
 * Poppler::Document must not render two pages at once; requests carry a
 * generation so a scroll past a page cancels nothing but makes its result
 * land in the bin. A resize changes the scale, so it clears the cache — but
 * only after the resize settles, and the stale images are drawn scaled
 * meanwhile, so dragging the splitter does not re-render every visible page
 * per pixel.
 */
class PdfPreview : public QWidget
{
    Q_OBJECT

public:
    explicit PdfPreview(QWidget *parent = nullptr);
    ~PdfPreview() override;

    void setDocument(const QUrl &url);
    void clear();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    class Strip;
    friend class Strip;

    struct Loaded {
        std::shared_ptr<Poppler::Document> doc;
        QList<QSizeF> pageSizes; // points
        QString error;
        bool skipped = false;
    };
    struct Rendered {
        int    page = -1;
        double scale = 0;
        QImage image;
    };

    void onLoaded();
    // Drops cached images for pages far from the viewport. Without this a
    // long document scrolled end to end would keep every page in memory.
    void evictFarPages();
    // Pixels per point at the current viewport width.
    double fitScale() const;
    void relayout();
    // Called by the strip for every visible page without a cached image.
    void requestPage(int page);
    // Called by the strip: the image for `page` at the current scale, or a
    // stale one from before the last resize (drawn scaled), or null.
    QImage cachedImage(int page, bool &stale) const;
    int pageCount() const { return m_pageSizes.size(); }

    QScrollArea *m_scroll = nullptr;
    Strip       *m_strip  = nullptr;
    QLabel      *m_info   = nullptr;
    QTimer      *m_rescale = nullptr;

    std::shared_ptr<Poppler::Document> m_doc;
    // Serialises page renders; Poppler documents are not thread-safe.
    std::shared_ptr<QMutex> m_renderLock;
    QList<QSizeF> m_pageSizes;
    double        m_scale = 1.0;      // px per point at the current width
    QHash<int, QImage> m_cache;       // rendered at m_scale
    QHash<int, QImage> m_staleCache;  // rendered at an earlier scale
    QSet<int>          m_inflight;

    int m_generation = 0;
    // Mirror of m_generation for the workers: a load or render queued behind
    // others checks it before starting, so arrowing through 20 PDFs or flinging
    // through a 500-page one no longer runs every superseded job.
    std::shared_ptr<std::atomic<int>> m_liveGeneration = std::make_shared<std::atomic<int>>(0);
    void bumpGeneration();
    QFutureWatcher<Loaded>   *m_loadWatcher = nullptr;
    QList<QFutureWatcher<Rendered> *> m_renderWatchers;
};
