#include "ThumbnailLoader.h"

#include <KIO/PreviewJob>
#include <KPluginMetaData>

#include <QDebug>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStringList>

// ── ThumbnailStore ────────────────────────────────────────────────────────────

ThumbnailStore::ThumbnailStore()
    : QObject(nullptr)
{
    m_cache.setMaxCost(kMaxCostKiB);
}

ThumbnailStore *ThumbnailStore::self()
{
    static ThumbnailStore instance;
    return &instance;
}

bool ThumbnailStore::lookup(const QUrl &url, QIcon &out) const
{
    if (const QIcon *icon = m_cache.object(url)) {
        out = *icon;
        return true;
    }
    return false;
}

bool ThumbnailStore::insert(const QUrl &url, const QPixmap &pixmap)
{
    // QCache cost is in the same unit as maxCost (KiB here).
    const int costKiB =
        qMax(1, static_cast<int>(qint64(pixmap.width()) * pixmap.height() * 4 / 1024));
    // Store the QIcon, not the pixmap: data() is called for every visible cell
    // on every repaint, and wrapping a pixmap there allocated each time.
    //
    // insert() deletes the object and returns false when the cost exceeds the
    // whole budget. That used to leave the URL in neither the cache nor the
    // failure set, so it was re-requested on every single pass, forever.
    return m_cache.insert(url, new QIcon(pixmap), costKiB);
}

void ThumbnailStore::markFailed(const QUrl &url)
{
    if (m_failed.size() >= kMaxFailed)
        m_failed.clear();
    m_failed.insert(url);
}

void ThumbnailStore::addInFlight(const QList<QUrl> &urls)
{
    for (const QUrl &u : urls)
        m_inFlight.insert(u);
}

void ThumbnailStore::removeInFlight(const QList<QUrl> &urls)
{
    for (const QUrl &u : urls)
        m_inFlight.remove(u);
}

void ThumbnailStore::forget(const QUrl &url)
{
    m_cache.remove(url);
    m_failed.remove(url);
}

void ThumbnailStore::clear()
{
    m_cache.clear();
    m_failed.clear();
    // Deliberately not m_inFlight: those jobs are still running and will report
    // back. Their loaders release the entries when the job finishes.
}

void ThumbnailStore::setSize(int px)
{
    if (m_size == px || px <= 0)
        return;
    m_size = px;
    clear(); // cached pixmaps are the wrong resolution now
}

void ThumbnailStore::setDevicePixelRatio(qreal dpr)
{
    if (dpr <= 0.0 || qFuzzyCompare(m_dpr, dpr))
        return;
    m_dpr = dpr;
}

void ThumbnailStore::setMaxFileSize(qint64 bytes)
{
    if (bytes <= 0 || m_maxFileSize == bytes)
        return;
    const bool raised = bytes > m_maxFileSize;
    m_maxFileSize = bytes;
    // Raising the cap makes previously-skipped files eligible. They were never
    // recorded as failures (they never reached a job), so nothing to undo —
    // except for files a job rejected on size, which did land in m_failed.
    if (raised)
        m_failed.clear();
}

// ── Image format support ──────────────────────────────────────────────────────

// Restricting the plugin set is what keeps this to "images only": without it
// PreviewJob would happily thumbnail text files, fonts and documents, which
// means reading every file in a source tree just to draw an icon view.
//
// Derived from what is actually installed rather than a hardcoded list of five
// plugin names. The old list ignored anything the user had beyond it
// (rawthumbnail, heifthumbnail, jxlthumbnail...) while the separate hardcoded
// extension list happily claimed .heic and .jxl were eligible — so those files
// were queued, failed, and were parked in the failure set, one wasted KIO round
// trip each. Both halves now come from the same source and cannot drift.
const ThumbnailLoader::ImageSupport &ThumbnailLoader::imageSupport()
{
    static const ImageSupport support = []() {
        ImageSupport out;
        QStringList imageMimes;
        bool wildcard = false;

        const QList<KPluginMetaData> plugins = KIO::PreviewJob::availableThumbnailerPlugins();
        for (const KPluginMetaData &md : plugins) {
            bool wanted = false;
            const QStringList mimes = md.mimeTypes();
            for (const QString &m : mimes) {
                if (!m.startsWith(QLatin1String("image/")))
                    continue;
                wanted = true;
                if (m.endsWith(QLatin1Char('*')))
                    wildcard = true;
                else
                    imageMimes.append(m);
            }
            if (wanted)
                out.plugins.append(md.pluginId());
        }

        QMimeDatabase db;
        const auto addSuffixes = [&out](const QMimeType &type) {
            const QStringList suffixes = type.suffixes();
            for (const QString &s : suffixes)
                out.suffixes.insert(s.toLower());
        };

        if (wildcard) {
            // A plugin claiming "image/*" — enumerate the database once rather
            // than guess. This runs at most once per process.
            const QList<QMimeType> all = db.allMimeTypes();
            for (const QMimeType &t : all) {
                if (t.name().startsWith(QLatin1String("image/")))
                    addSuffixes(t);
            }
        }
        for (const QString &m : std::as_const(imageMimes))
            addSuffixes(db.mimeTypeForName(m));

        if (out.plugins.isEmpty()) {
            qWarning() << "Vela: no image thumbnail plugins installed — icon views "
                          "will show MIME icons only";
        }
        if (out.suffixes.isEmpty()) {
            // Nothing derivable (unusual MIME database). Fall back to a fixed
            // set so extension-based eligibility still works at all.
            static const char *kFallback[] = {
                "png", "jpg", "jpeg", "gif", "bmp", "webp", "tif", "tiff",
                "svg", "svgz", "ico", "avif", "heif", "heic", "exr", "jxl",
                "ppm", "xpm",
            };
            for (const char *s : kFallback)
                out.suffixes.insert(QLatin1String(s));
        }
        return out;
    }();
    return support;
}

// Cheap fallback for items whose MIME type has not been resolved yet. Matching
// on the extension costs nothing; asking KFileItem would not. One hash lookup
// rather than the eighteen endsWith() calls this used to do per item.
static bool looksLikeImageByExtension(const QString &name, const QSet<QString> &suffixes)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1)
        return false;
    return suffixes.contains(name.mid(dot + 1).toLower());
}

// ── ThumbnailLoader ───────────────────────────────────────────────────────────

ThumbnailLoader::ThumbnailLoader(QObject *parent)
    : QObject(parent)
{
    // Generating a screenful of thumbnails produces a burst of completions.
    // Announcing each one separately made the view repaint once per image,
    // which is what made the first visit to an image folder stutter. One flush
    // per frame-ish interval instead.
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(32);
    connect(&m_flushTimer, &QTimer::timeout, this, &ThumbnailLoader::flushReady);

    // Results generated by any loader in the process are relevant to this one:
    // the store is shared, so a thumbnail produced for another panel is already
    // available here. Models drop URLs they don't hold a row for.
    connect(ThumbnailStore::self(), &ThumbnailStore::ready,
            this, &ThumbnailLoader::thumbnailsReady);
    connect(ThumbnailStore::self(), &ThumbnailStore::abandoned,
            this, &ThumbnailLoader::onAbandoned);
}

ThumbnailLoader::~ThumbnailLoader()
{
    cancel();
}

void ThumbnailLoader::flushReady()
{
    if (m_readyBatch.isEmpty())
        return;
    const QList<QUrl> batch = std::move(m_readyBatch);
    m_readyBatch.clear();
    // Via the store, so every panel hears about it — not just this one.
    ThumbnailStore::self()->announce(batch);
}

void ThumbnailLoader::setThumbnailSize(int px)
{
    if (px == ThumbnailStore::self()->size())
        return;
    cancel();
    ThumbnailStore::self()->setSize(px);
}

void ThumbnailLoader::setDevicePixelRatio(qreal dpr)
{
    ThumbnailStore::self()->setDevicePixelRatio(dpr);
}

void ThumbnailLoader::setMaxFileSize(qint64 bytes)
{
    ThumbnailStore::self()->setMaxFileSize(bytes);
}

bool ThumbnailLoader::isEligible(const KFileItem &item) const
{
    if (item.isNull() || item.isDir())
        return false;
    // Local files only: a preview over sftp/smb has to transfer the whole file
    // before it can be decoded, which is exactly the stall we're avoiding.
    if (!item.url().isLocalFile())
        return false;

    // Deliberately NOT item.mimetype(): that is documented as equivalent to
    // determineMimeType(), which sniffs file contents when the extension is
    // unknown. Running that over every visible row on the GUI thread is exactly
    // the stall this feature is supposed to avoid. Use the type only when it is
    // already known, otherwise fall back to the extension.
    if (item.isMimeTypeKnown()) {
        if (!item.currentMimeType().name().startsWith(QLatin1String("image/")))
            return false;
    } else if (!looksLikeImageByExtension(item.name(), imageSupport().suffixes)) {
        return false;
    }

    const KIO::filesize_t size = item.size();
    if (size > 0 && static_cast<qint64>(size) > ThumbnailStore::self()->maxFileSize())
        return false;
    return true;
}

bool ThumbnailLoader::thumbnail(const QUrl &url, QIcon &out) const
{
    return ThumbnailStore::self()->lookup(url, out);
}

void ThumbnailLoader::forget(const QUrl &url)
{
    ThumbnailStore::self()->forget(url);
    if (m_queued.remove(url)) {
        for (int i = 0; i < m_queue.size(); ++i) {
            if (m_queue.at(i).url() == url) {
                m_queue.removeAt(i);
                break;
            }
        }
    }
}

void ThumbnailLoader::clearCache()
{
    cancel();
    ThumbnailStore::self()->clear();
}

void ThumbnailLoader::releaseInFlight(bool killed)
{
    if (m_jobUrls.isEmpty())
        return;
    ThumbnailStore *store = ThumbnailStore::self();
    store->removeInFlight(m_jobUrls);
    QList<QUrl> unfinished;
    if (killed) {
        for (const QUrl &url : std::as_const(m_jobUrls)) {
            if (!store->contains(url) && !store->isFailed(url))
                unfinished.append(url);
        }
    }
    m_jobUrls.clear();
    if (!unfinished.isEmpty())
        store->announceAbandoned(unfinished);
}

void ThumbnailLoader::onAbandoned(const QList<QUrl> &urls)
{
    if (m_deferred.isEmpty())
        return;
    ThumbnailStore *store = ThumbnailStore::self();
    bool added = false;
    for (const QUrl &url : urls) {
        const auto it = m_deferred.constFind(url);
        if (it == m_deferred.constEnd())
            continue;
        const KFileItem item = it.value();
        m_deferred.erase(it);
        // Re-checked: a third loader may already have picked it up.
        if (store->contains(url) || store->isFailed(url) || store->isInFlight(url)
            || m_queued.contains(url))
            continue;
        m_queue.append(item);
        m_queued.insert(url);
        added = true;
    }
    if (added)
        startNextJob();
}

void ThumbnailLoader::cancel()
{
    m_queue.clear();
    m_queued.clear();
    m_deferred.clear();

    // Flush before dropping the batch. Those thumbnails are already in the
    // store; discarding the notification only meant the rows displaying them
    // kept their MIME icon until some unrelated repaint came along.
    m_flushTimer.stop();
    flushReady();

    if (m_job) {
        // Killing mid-batch is cheap and loses nothing: ScaledAndCached means
        // every thumbnail already generated is on disk in the shared cache.
        m_job->kill();
        m_job = nullptr;
        releaseInFlight(/*killed=*/true);
    }
    releaseInFlight();
}

void ThumbnailLoader::requestBatch(const KFileItemList &items)
{
    ThumbnailStore *store = ThumbnailStore::self();

    KFileItemList wanted;
    QSet<QUrl> wantedUrls;
    wanted.reserve(items.size());
    m_deferred.clear();

    for (const KFileItem &item : items) {
        const QUrl url = item.url();
        // isInFlight is the important one: those have been handed to a running
        // PreviewJob (possibly another panel's) and are in neither the cache
        // nor the failure set yet, so without it they are generated twice.
        if (store->contains(url) || store->isFailed(url))
            continue;
        if (store->isInFlight(url)) {
            // Someone else's job (or our own running one). Remember it, so a
            // kill of that job hands it to us instead of leaving it blank.
            if (!m_jobUrls.contains(url) && isEligible(item))
                m_deferred.insert(url, item);
            continue;
        }
        if (wantedUrls.contains(url))
            continue;
        if (!isEligible(item))
            continue;
        wantedUrls.insert(url);
        wanted.append(item);
    }

    // Replace rather than append — the caller is telling us what is on screen
    // *now*, and anything queued for a range the user has scrolled past is
    // wasted work.
    m_queue  = wanted;
    m_queued = wantedUrls;

    if (m_queue.isEmpty())
        return;

    // Deliberately does NOT kill an in-flight job. A batch is about one
    // screenful, so letting it finish costs little and its results are written
    // to the shared on-disk cache regardless. Killing here meant that while a
    // directory was still streaming in, every arriving chunk tore down and
    // respawned the KIO thumbnail worker — churn that showed up precisely as a
    // stutter on the first visit to a folder. startNextJob() is a no-op while a
    // job runs; the queue is picked up as soon as it completes.
    startNextJob();
}

void ThumbnailLoader::startNextJob()
{
    if (m_job || m_queue.isEmpty())
        return;

    // Small chunks, deliberately. PreviewJob does a meaningful amount of work
    // on the calling thread per item — resolving a thumbnailer, hashing the
    // URL, stat'ing the on-disk cache and decoding the cached PNG when it hits.
    // Handing it a whole screenful at once therefore blocks the GUI in one
    // burst, which is exactly the stutter seen on the first visit to an image
    // folder (on later visits the in-memory cache answers first and PreviewJob
    // is never constructed at all). Chunking spreads that cost across event
    // loop iterations, so thumbnails paint in progressively and the UI keeps
    // responding.
    //
    // INTENDED — do not flag: one job per panel at a time, in chunks of 12, is
    // a deliberate choice for GUI smoothness over raw throughput. A second
    // parallel job might shorten a cold first visit on fast storage, but it
    // doubles the per-item GUI-thread work described above; it is only worth
    // revisiting with measurements showing a win on both wall time and stalls.
    static constexpr int kChunkSize = 12;

    KFileItemList batch;
    const int count = qMin(kChunkSize, m_queue.size());
    batch.reserve(count);
    m_jobUrls.reserve(count);
    for (int i = 0; i < count; ++i) {
        const KFileItem item = m_queue.takeFirst();
        m_queued.remove(item.url());
        m_jobUrls.append(item.url());
        batch.append(item);
    }
    ThumbnailStore *store = ThumbnailStore::self();
    store->addInFlight(m_jobUrls);

    const QStringList plugins = imageSupport().plugins;
    auto *job = KIO::filePreview(batch, QSize(store->size(), store->size()), &plugins);
    // ScaledAndCached writes results into ~/.cache/thumbnails, so this work is
    // done once per file per size for the whole desktop, not once per launch.
    job->setScaleType(KIO::PreviewJob::ScaledAndCached);
    job->setDevicePixelRatio(store->devicePixelRatio());
    // Our own configurable cap in isEligible() is the single authority on what
    // is too big. Leaving KIO's own limit active meant a second, invisible and
    // much lower threshold silently overrode the user's setting.
    job->setIgnoreMaximumSize(true);
    m_job = job;

    connect(job, &KIO::PreviewJob::gotPreview, this,
            [this](const KFileItem &item, const QPixmap &preview) {
        ThumbnailStore *s = ThumbnailStore::self();
        const QUrl url = item.url();
        if (!s->insert(url, preview)) {
            // Bigger than the entire cache budget — it will never be servable,
            // so record it as failed rather than regenerating it forever.
            s->markFailed(url);
            return;
        }
        m_readyBatch.append(url);
        if (!m_flushTimer.isActive())
            m_flushTimer.start();
    });

    connect(job, &KIO::PreviewJob::failed, this, [](const KFileItem &item) {
        // Remember failures so a broken file isn't retried on every scroll.
        ThumbnailStore::self()->markFailed(item.url());
    });

    connect(job, &KJob::result, this, [this](KJob *) {
        m_job = nullptr;
        releaseInFlight();
        startNextJob(); // pick up anything queued while this batch ran
    });
}
