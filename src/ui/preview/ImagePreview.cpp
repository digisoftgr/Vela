#include "ImagePreview.h"

#include "core/ThumbnailLoader.h"
#include "core/WorkerPools.h"

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QStandardPaths>
#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

ImagePreview::ImagePreview(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    m_label = new QLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_label->setMinimumSize(1, 1);

    m_info = new QLabel(this);
    m_info->setAlignment(Qt::AlignCenter);
    m_info->setWordWrap(true);

    layout->addWidget(m_label, 1);
    layout->addWidget(m_info);

    // A splitter drag sends a resize per pixel, and each refit smooth-scales
    // an image of up to kMaxDecodeEdge px. Same debounce as PdfPreview.
    m_refitDebounce = new QTimer(this);
    m_refitDebounce->setSingleShot(true);
    m_refitDebounce->setInterval(150);
    connect(m_refitDebounce, &QTimer::timeout, this, &ImagePreview::refit);

    m_watcher = new QFutureWatcher<Decoded>(this);
    connect(m_watcher, &QFutureWatcher<Decoded>::finished, this, [this]() {
        if (m_watcher->property("generation").toInt() != m_generation)
            return;
        const Decoded d = m_watcher->result();
        if (d.skipped)
            return;
        m_placeholder = false;
        if (!d.error.isEmpty()) {
            m_image = QImage();
            m_label->clear();
            m_info->setText(d.error);
            return;
        }
        m_image        = d.image;
        m_originalSize = d.originalSize;
        m_info->setText(i18n("%1 × %2", m_originalSize.width(), m_originalSize.height()));
        refit();
    });

    m_thumbWatcher = new QFutureWatcher<QImage>(this);
    connect(m_thumbWatcher, &QFutureWatcher<QImage>::finished, this, [this]() {
        // Only while nothing better is on screen: the full decode may have won.
        if (m_thumbWatcher->property("generation").toInt() != m_generation
            || (!m_image.isNull() && !m_placeholder))
            return;
        const QImage thumb = m_thumbWatcher->result();
        if (thumb.isNull())
            return;
        m_image = thumb;
        m_placeholder = true;
        refit();
    });
}

void ImagePreview::bumpGeneration()
{
    ++m_generation;
    m_liveGeneration->store(m_generation);
}

// freedesktop thumbnail spec: $XDG_CACHE_HOME/thumbnails/<size>/<md5(uri)>.png,
// valid only while its Thumb::MTime matches the file. Largest first.
QImage ImagePreview::cachedThumbnail(const QString &path)
{
    const QFileInfo fi(path);
    const QByteArray uri = QUrl::fromLocalFile(fi.absoluteFilePath()).toEncoded();
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex()) + QStringLiteral(".png");
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                         + QStringLiteral("/thumbnails/");
    const QString mtime = QString::number(fi.lastModified().toSecsSinceEpoch());
    for (const char *dir : {"xx-large", "x-large", "large", "normal"}) {
        QImage img;
        if (!img.load(base + QLatin1String(dir) + QLatin1Char('/') + name))
            continue;
        if (img.text(QStringLiteral("Thumb::MTime")) == mtime)
            return img;
    }
    return {};
}

// No wait for a running decode: the task captures only the path, and the
// watcher (a child) disconnects from its future when it is destroyed. Waiting
// here froze closing the window while a decode was stuck on a hung mount.
ImagePreview::~ImagePreview() = default;

void ImagePreview::clear()
{
    bumpGeneration();
    m_image = QImage();
    m_placeholder = false;
    m_label->clear();
    m_info->clear();
}

void ImagePreview::setImage(const QUrl &url)
{
    clear();
    if (!url.isLocalFile()) {
        m_info->setText(i18n("Image preview is only available for local files."));
        return;
    }
    m_info->setText(i18n("Loading…"));

    const int generation = m_generation;
    m_watcher->setProperty("generation", generation);
    m_thumbWatcher->setProperty("generation", generation);
    const QString path = url.toLocalFile();
    const auto live = m_liveGeneration;

    // Stand-in first. The icon views' in-memory thumbnail costs nothing;
    // otherwise probe the on-disk cache next to the decode.
    QIcon icon;
    if (ThumbnailStore::self()->lookup(url, icon)) {
        QSize biggest(256, 256);
        const QList<QSize> sizes = icon.availableSizes();
        for (const QSize &sz : sizes) {
            if (sz.width() * sz.height() > biggest.width() * biggest.height())
                biggest = sz;
        }
        m_image = icon.pixmap(biggest).toImage();
        m_placeholder = !m_image.isNull();
        refit();
    } else {
        m_thumbWatcher->setFuture(QtConcurrent::run(WorkerPools::io(), [path, live, generation]() {
            return live->load() == generation ? cachedThumbnail(path) : QImage();
        }));
    }

    m_watcher->setFuture(QtConcurrent::run(WorkerPools::io(), [path, live, generation]() -> Decoded {
        if (live->load() != generation) {
            Decoded skipped;
            skipped.skipped = true;
            return skipped;
        }
        Decoded d;
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (!reader.canRead()) {
            d.error = i18n("Cannot decode this image.");
            return d;
        }
        d.originalSize = reader.size();
        // Apply the EXIF rotation to the reported size too, so a portrait
        // photo says 3000 × 4000 rather than the sensor's 4000 × 3000.
        const auto t = reader.transformation();
        if (t & (QImageIOHandler::TransformationRotate90))
            d.originalSize.transpose();

        if (d.originalSize.isValid()
            && (d.originalSize.width() > kMaxDecodeEdge
                || d.originalSize.height() > kMaxDecodeEdge)) {
            // The scaled size is in pre-rotation coordinates for the reader.
            QSize target = reader.size();
            target.scale(kMaxDecodeEdge, kMaxDecodeEdge, Qt::KeepAspectRatio);
            reader.setScaledSize(target);
        }
        d.image = reader.read();
        if (d.image.isNull())
            d.error = reader.errorString().isEmpty()
                ? i18n("Cannot decode this image.") : reader.errorString();
        return d;
    }));
}

void ImagePreview::refit()
{
    if (m_image.isNull())
        return;
    const QSize avail = m_label->size();
    if (avail.width() < 2 || avail.height() < 2)
        return;
    // Never upscale: a 16 px icon shown at 800 px is noise, not a preview.
    // The stand-in thumbnail is the exception — it is scaled to where the real
    // image will land, so the swap does not jump.
    QSize target = m_image.size();
    if (m_placeholder || target.width() > avail.width() || target.height() > avail.height())
        target.scale(avail, Qt::KeepAspectRatio);
    const qreal dpr = devicePixelRatioF();
    QImage scaled = m_image.scaled(target * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    m_label->setPixmap(QPixmap::fromImage(scaled));
}

void ImagePreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_image.isNull())
        m_refitDebounce->start();
}
