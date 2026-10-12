#pragma once

#include <QFutureWatcher>
#include <QImage>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QTimer;

/**
 * Scaled-to-fit image preview. Decoding runs on a worker thread and is
 * bounded: anything larger than kMaxDecodeEdge on a side is decoded straight
 * to that size by the reader (JPEG does this at a fraction of the cost of a
 * full decode), so a 60-megapixel photo never becomes a 240 MB QImage on the
 * heap. EXIF orientation is applied. The result is re-fitted on every resize
 * from the decoded image, never from an already-scaled copy.
 *
 * While the full decode runs, an existing thumbnail stands in for it: the
 * in-memory one the icon views already hold, or else the freedesktop
 * ~/.cache/thumbnails entry (read on a worker, checked against the file's
 * mtime). A cache miss costs one failed open; nothing is generated here.
 */
class ImagePreview : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kMaxDecodeEdge = 4096;

    explicit ImagePreview(QWidget *parent = nullptr);
    ~ImagePreview() override;

    void setImage(const QUrl &url);
    void clear();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Decoded {
        QImage  image;
        QSize   originalSize;
        QString error;
        bool    skipped = false; // superseded before it started
    };

    void refit();
    void bumpGeneration();
    // Reads the freedesktop cached thumbnail for `path`, or a null image.
    static QImage cachedThumbnail(const QString &path);

    QLabel *m_label = nullptr;
    QLabel *m_info  = nullptr;
    QImage  m_image;
    QSize   m_originalSize;
    bool    m_placeholder = false; // m_image is a stand-in thumbnail
    int m_generation = 0;
    // Mirror of m_generation the workers can read, so a decode queued behind
    // others is skipped once superseded instead of running for nothing
    // (arrowing through 20 images used to decode all 20).
    std::shared_ptr<std::atomic<int>> m_liveGeneration = std::make_shared<std::atomic<int>>(0);
    QFutureWatcher<Decoded> *m_watcher = nullptr;
    QFutureWatcher<QImage>  *m_thumbWatcher = nullptr;
    QTimer *m_refitDebounce = nullptr; // resize → refit, once per drag pause
};
