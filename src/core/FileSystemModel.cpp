#include "FileSystemModel.h"

#include "ThumbnailLoader.h"
#include "WorkerPools.h"

#include <KCoreDirLister>
#include <KDirWatch>
#include <KFileSystemType>
#include <KIO/CopyJob>
#include <KIO/DirectorySizeJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegateFactory>
#include <KLocalizedString>

#include <QCoreApplication>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <kcoreaddons_version.h>

#include <utility>

namespace {

// The one watcher behind every model's fast pickup: QFileSystemWatcher costs an
// inotify instance each, and instances are scarce (often 128 per user, shared
// with every other application). Parented to the application; a QPointer, as
// models may outlive it during shutdown.
QFileSystemWatcher *sharedFastWatcher()
{
    static QPointer<QFileSystemWatcher> watcher = new QFileSystemWatcher(qApp);
    return watcher;
}

QHash<QString, int> &fastWatchRefs()
{
    static QHash<QString, int> refs;
    return refs;
}

bool acquireFastWatch(const QString &path)
{
    QFileSystemWatcher *w = sharedFastWatcher();
    if (!w)
        return false;
    // Ask the watcher, not the count: inotify drops the watch of a deleted
    // directory on its own, and a recreated one needs adding again.
    if (!w->directories().contains(path) && !w->addPath(path))
        return false;
    ++fastWatchRefs()[path];
    return true;
}

void releaseFastWatch(const QString &path)
{
    auto &refs = fastWatchRefs();
    const auto it = refs.find(path);
    if (it == refs.end())
        return;
    if (--it.value() > 0)
        return;
    refs.erase(it);
    if (QFileSystemWatcher *w = sharedFastWatcher())
        w->removePath(path);
}

} // namespace

FileSystemModel::FileSystemModel(QObject *parent)
    : FileItemModelBase(parent)
{
    // KCoreDirLister handles listing, the KIO worker lifecycle, live watching,
    // and per-file delta signals — the same engine Dolphin builds on. This model
    // is just the QAbstractItemModel adapter that mirrors its item list.
    m_dirLister = new KCoreDirLister(this);
    m_dirLister->setAutoUpdate(true);
    m_dirLister->setShowHiddenFiles(m_showHidden);
    // Items carry a name-based MIME guess instead of one determined from the
    // file's contents. Without this, KFileItem::iconName() — called from data()
    // for every painted icon — read the leading bytes of every extension-less
    // file on the GUI thread, defeating MimeTypeResolver. Dolphin does the same.
    m_dirLister->setDelayedMimeTypes(true);

    connect(m_dirLister, &KCoreDirLister::itemsAdded,   this, &FileSystemModel::onItemsAdded);
    connect(m_dirLister, &KCoreDirLister::itemsDeleted, this, &FileSystemModel::onItemsDeleted);
    connect(m_dirLister, &KCoreDirLister::refreshItems, this, &FileSystemModel::onRefreshItems);
    connect(m_dirLister, &KCoreDirLister::clear,        this, &FileSystemModel::onListerClear);
    connect(m_dirLister, &KCoreDirLister::completed,    this, &FileSystemModel::onListerCompleted);
    connect(m_dirLister, &KCoreDirLister::jobError, this, [this](KIO::Job *job) {
        m_fastRelistRunning = false; // its completed() will not come through
        const QString message = job ? job->errorString() : i18n("Could not read the directory.");
        if (m_initialLoading) {
            // The listing itself failed: the rows are not the directory.
            m_listingError = true;
            Q_EMIT directoryError(message);
            return;
        }
        // A re-list of a directory that did list. The rows on screen are still
        // the last good listing, so this is not a listing failure: marking it
        // one put a banner over valid rows and stopped live updates until the
        // next navigation (a hiccup on a network mount was enough). Report it
        // only when the user asked for the re-list, and let the existence check
        // decide whether the directory is really gone.
        const bool explicitReload = std::exchange(m_emitFullOnNextCompleted, false);
        if (explicitReload)
            Q_EMIT directoryError(message);
        revalidateDirectory();
    });

    // KCoreDirLister watches the directory's *contents* and reports per-file
    // deltas, but has no signal for "the directory you are listing is gone" —
    // it simply drops every row, which is indistinguishable from an emptied
    // folder. Watch the directory itself so a deletion made anywhere else (the
    // other split panel, another window, a terminal) surfaces immediately.
    m_dirWatch = new KDirWatch(this);
    connect(m_dirWatch, &KDirWatch::deleted, this, &FileSystemModel::onWatchedPathDeleted);

    m_clock.start();
    if (QFileSystemWatcher *w = sharedFastWatcher()) {
        connect(w, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
            if (path == m_fastWatchedPath)
                onFastWatchFired();
        });
    }
    m_fastRelistTimer = new QTimer(this);
    m_fastRelistTimer->setSingleShot(true);
    connect(m_fastRelistTimer, &QTimer::timeout, this, &FileSystemModel::startFastRelist);
    m_fastResumeTimer = new QTimer(this);
    m_fastResumeTimer->setSingleShot(true);
    m_fastResumeTimer->setInterval(kFastResumeQuietMs);
    connect(m_fastResumeTimer, &QTimer::timeout, this, [this]() {
        // Quiet again: catch up on what KIO's slower path may not have shown
        // yet, and take the fast path from here.
        m_fastPaused = false;
        m_fastRelists.clear();
        m_fastCatchUp = true;
        m_fastRelistTimer->start(0);
    });
}

FileSystemModel::~FileSystemModel()
{
    cancelAllFolderSizeJobs();
    if (!m_fastWatchedPath.isEmpty())
        releaseFastWatch(m_fastWatchedPath);
    // m_dirLister is a QObject child and is destroyed automatically.
}

void FileSystemModel::setDirectory(const QUrl &url, bool reload)
{
    // Drop folder-size state from the previous directory.
    //
    // INTENDED — do not flag as "missing cache": sizes are deliberately not
    // kept across navigation. A folder's mtime does not change when something
    // deep inside it does, so a cached size can be silently wrong and there is
    // no cheap way to tell. Rescanning on return is the price of never showing
    // a stale size. Decided and kept deliberately.
    cancelAllFolderSizeJobs();
    m_folderSizes.clear();

    m_currentDir = url;
    m_listingError = false;
    m_frozen = false;
    m_dirLister->setAutoUpdate(true);
    m_initialLoading = true; // suppress per-batch refresh signals during the bulk load

    // Reset rows immediately for a snappy navigation. The lister will also emit
    // clear() as it begins the new listing; resetting an empty model is harmless.
    clearItems();

    m_emitFullOnNextCompleted = true; // the completed() for this listing is a full load
    watchCurrentDirectory();
    Q_EMIT loadingStarted(url);
    m_dirLister->openUrl(url, reload ? KCoreDirLister::Reload : KCoreDirLister::NoFlags);
}

QUrl FileSystemModel::directory() const
{
    return m_currentDir;
}

void FileSystemModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    m_dirLister->setShowHiddenFiles(show);
    m_dirLister->emitChanges();               // emits itemsAdded/itemsDeleted for the delta
    Q_EMIT directoryRefreshed(m_currentDir);  // let the panel refresh counts / empty label
}

bool FileSystemModel::showHidden() const
{
    return m_showHidden;
}

void FileSystemModel::setFoldersFirst(bool first)
{
    // Display order is owned by the sort/filter proxy; nothing to re-sort here.
    m_foldersFirst = first;
}

bool FileSystemModel::foldersFirst() const
{
    return m_foldersFirst;
}

// ── Directory existence ───────────────────────────────────────────────────────

void FileSystemModel::watchCurrentDirectory()
{
    const QString path = m_currentDir.isLocalFile() ? m_currentDir.toLocalFile() : QString();

    // A new directory: what was pending for the old one no longer applies.
    if (path != m_fastWatchedPath) {
        resetFastPickup();
        if (!m_fastWatchedPath.isEmpty())
            releaseFastWatch(m_fastWatchedPath);
        m_fastWatchedPath.clear();
        const KFileSystemType::Type fs =
            path.isEmpty() ? KFileSystemType::Unknown : KFileSystemType::fileSystemType(path);
        bool network = fs == KFileSystemType::Nfs || fs == KFileSystemType::Smb;
#if KCOREADDONS_VERSION >= QT_VERSION_CHECK(6, 30, 0)
        // sshfs, rclone, …: before 6.30 the common ones already read as Nfs.
        network = network || fs == KFileSystemType::FuseNoDev;
#endif
        if (!path.isEmpty() && !network && acquireFastWatch(path))
            m_fastWatchedPath = path;
    } else if (!path.isEmpty()) {
        // Same directory, re-armed (a reload after it was deleted and
        // recreated): inotify dropped the watch with the old directory.
        QFileSystemWatcher *w = sharedFastWatcher();
        if (w && !w->directories().contains(path))
            w->addPath(path);
    }

    if (path == m_watchedPath)
        return;

    if (!m_watchedPath.isEmpty())
        m_dirWatch->removeDir(m_watchedPath);
    m_watchedPath = path;
    // Remote locations (sftp, smb, trash, …) have no inotify equivalent here;
    // they fall back to revalidateDirectory() on activation.
    if (!m_watchedPath.isEmpty())
        m_dirWatch->addDir(m_watchedPath);
}

// ── Fast change pickup ────────────────────────────────────────────────────────

void FileSystemModel::onFastWatchFired()
{
    if (m_frozen)
        return;
    if (m_fastPaused) {
        m_fastResumeTimer->start(); // not quiet yet
        return;
    }
    // Coalesce a burst; one already queued or running absorbs this change.
    if (m_fastRelistRunning) {
        m_fastRelistAgain = true;
        return;
    }
    if (!m_fastRelistTimer->isActive())
        m_fastRelistTimer->start(kFastRelistDelayMs);
}

void FileSystemModel::refreshSoon()
{
    // Not a filesystem event: while paused it neither re-lists nor delays the
    // resume. KIO's own path shows the change meanwhile.
    if (!m_fastPaused)
        onFastWatchFired();
}

void FileSystemModel::relistLight()
{
    if (!m_currentDir.isValid() || m_frozen)
        return;
    m_dirLister->updateDirectory(m_currentDir);
}

void FileSystemModel::setFrozen(bool frozen)
{
    if (m_frozen == frozen)
        return;
    m_frozen = frozen;
    if (frozen) {
        m_dirLister->setAutoUpdate(false);
        if (m_initialLoading)
            m_dirLister->stop(); // incomplete: needsReload() stays true
        cancelAllFolderSizeJobs();
        resetFastPickup();
        if (!m_fastWatchedPath.isEmpty())
            releaseFastWatch(m_fastWatchedPath);
        m_fastWatchedPath.clear();
        if (!m_watchedPath.isEmpty())
            m_dirWatch->removeDir(m_watchedPath);
        m_watchedPath.clear();
        return;
    }
    m_dirLister->setAutoUpdate(true);
    watchCurrentDirectory();
}

void FileSystemModel::pauseFastPickup()
{
    m_fastPaused = true;
    m_fastRelistTimer->stop();
    m_fastRelistAgain = false;
    m_fastResumeTimer->start();
}

void FileSystemModel::resetFastPickup()
{
    m_fastRelistTimer->stop();
    m_fastResumeTimer->stop();
    m_fastRelistRunning = false;
    m_fastRelistAgain = false;
    m_fastPaused = false;
    m_fastCatchUp = false;
    m_fastRelists.clear();
}

void FileSystemModel::startFastRelist()
{
    // The initial load is still streaming in (it will show the change itself),
    // or there is nothing valid to re-list.
    if (m_frozen || m_initialLoading || m_listingError || !m_currentDir.isValid())
        return;
    m_fastRelistRunning = true;
    m_fastRelistStartedMs = m_clock.elapsed();
    // A live update: its completed() takes the light directoryRefreshed path.
    m_dirLister->updateDirectory(m_currentDir);
}

void FileSystemModel::onWatchedPathDeleted(const QString &path)
{
    if (path != m_watchedPath)
        return;
    reportDirectoryGone();
}

void FileSystemModel::reportDirectoryGone()
{
    if (m_listingError) // already reported for this directory
        return;
    m_listingError = true;
    m_initialLoading = false;
    m_emitFullOnNextCompleted = false;

    // Stop listing and drop the rows: they describe a directory that no longer
    // exists, and leaving them on screen invites operations that cannot work.
    m_dirLister->stop();
    cancelAllFolderSizeJobs();
    m_folderSizes.clear();
    clearItems();

    // Same wording KIO gives when a navigation to a missing path fails, so the
    // banner reads identically however the directory came to be missing.
    Q_EMIT directoryError(KIO::buildErrorString(
        KIO::ERR_DOES_NOT_EXIST, m_currentDir.toDisplayString(QUrl::PreferLocalFile)));
}

void FileSystemModel::revalidateDirectory()
{
    if (!m_currentDir.isValid() || !m_currentDir.isLocalFile())
        return; // nothing local to stat; the lister's own errors stand
    if (m_listingError)
        return; // already reported — don't re-emit on every activation

    // One stat is the whole check: a path that resolves to a directory proves
    // every ancestor along it resolved too, so walking the chain separately
    // would find nothing this misses.
    //
    // On a worker: this runs on every window activation for every panel, and
    // a stat on a hung NFS/sshfs mount blocks until the mount answers — on the
    // GUI thread that froze the whole application. A path already being
    // checked is not checked again, so a hung mount ties up one worker, not
    // one per activation.
    const QString path = m_currentDir.toLocalFile();
    if (m_revalidating.contains(path))
        return;
    m_revalidating.insert(path);

    const QUrl dir = m_currentDir;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, dir, path]() {
        watcher->deleteLater();
        m_revalidating.remove(path);
        // Applied only if we are still showing that directory and nothing
        // else has reported it meanwhile.
        if (!watcher->result() && m_currentDir == dir && !m_listingError)
            reportDirectoryGone();
    });
    watcher->setFuture(QtConcurrent::run(WorkerPools::io(),
                                         [path]() { return QFileInfo(path).isDir(); }));
}

// ── Folder sizes ──────────────────────────────────────────────────────────────

QVariant FileSystemModel::folderSizeData(const KFileItem &item, int role) const
{
    const auto it = m_folderSizes.constFind(item.url());
    if (it == m_folderSizes.constEnd())
        return {}; // not calculated — base class supplies the fallback

    if (role == NumericSizeRole)
        return QVariant(static_cast<qint64>(it.value()));
    return KIO::convertSize(it.value());
}

void FileSystemModel::onRowDropped(const QUrl &url)
{
    if (ThumbnailLoader *loader = thumbnailLoader())
        loader->forget(url);
    m_folderSizes.remove(url);
    if (KJob *job = m_runningSizeJobs.take(url)) {
        // kill(Quietly) emits no result(), so onFolderSizeJobDone never runs
        // for this job and nothing else would refill the freed slot.
        job->kill(KJob::Quietly);
        schedulePumpFolderSizeQueue();
    }
    if (m_queuedSizes.remove(url))
        m_sizeQueue.removeOne(url);
}

void FileSystemModel::schedulePumpFolderSizeQueue()
{
    // Deferred, not immediate: rows are dropped one at a time ahead of a batch
    // removal, and pumping now could start a job for a sibling that is about to
    // be dropped by the same batch.
    if (m_pumpScheduled)
        return;
    m_pumpScheduled = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_pumpScheduled = false;
        pumpFolderSizeQueue();
    }, Qt::QueuedConnection);
}

void FileSystemModel::requestAllFolderSizes()
{
    m_autoFolderSizes = true;
    if (m_frozen)
        return; // queued again on thaw by the panel
    for (const KFileItem &item : std::as_const(m_items)) {
        if (item.isDir())
            enqueueFolderSize(item.url());
    }
    pumpFolderSizeQueue();
}

void FileSystemModel::enqueueFolderSize(const QUrl &url)
{
    if (m_folderSizes.contains(url) || m_runningSizeJobs.contains(url)
        || m_queuedSizes.contains(url)) {
        return;
    }
    m_sizeQueue.append(url);
    m_queuedSizes.insert(url);
}

void FileSystemModel::pumpFolderSizeQueue()
{
    while (!m_frozen && m_runningSizeJobs.size() < kMaxConcurrentSizeJobs
           && !m_sizeQueue.isEmpty()) {
        const QUrl url = m_sizeQueue.takeFirst();
        m_queuedSizes.remove(url);

        // The row may have been removed between queueing and starting.
        if (rowForUrl(url) < 0 || m_folderSizes.contains(url))
            continue;

        auto *job = KIO::directorySize(url);
        m_runningSizeJobs.insert(url, job);
        connect(job, &KJob::result, this, [this, url](KJob *finishedJob) {
            onFolderSizeJobDone(url, finishedJob);
        });
    }
}

void FileSystemModel::cancelAllFolderSizeJobs()
{
    // kill(Quietly) emits no result(), so onFolderSizeJobDone does not run for
    // these; the hash is detached first simply so iteration never depends on it.
    const QList<KJob *> running = m_runningSizeJobs.values();
    m_runningSizeJobs.clear();
    m_sizeQueue.clear();
    m_queuedSizes.clear();
    for (KJob *job : running)
        job->kill(KJob::Quietly);
}

void FileSystemModel::clearFolderSizes()
{
    m_autoFolderSizes = false;
    cancelAllFolderSizeJobs();

    if (m_folderSizes.isEmpty())
        return;
    m_folderSizes.clear();

    if (!m_items.isEmpty()) {
        Q_EMIT dataChanged(createIndex(0, Size),
                           createIndex(m_items.size() - 1, Size),
                           {Qt::DisplayRole, NumericSizeRole});
    }
}

void FileSystemModel::onFolderSizeJobDone(const QUrl &url, KJob *job)
{
    m_runningSizeJobs.remove(url);

    auto *sizeJob = qobject_cast<KIO::DirectorySizeJob *>(job);
    if (sizeJob && !sizeJob->error()) {
        m_folderSizes.insert(url, sizeJob->totalSize());

        const int row = rowForUrl(url);
        if (row >= 0) {
            const QModelIndex idx = createIndex(row, Size);
            Q_EMIT dataChanged(idx, idx, {Qt::DisplayRole, NumericSizeRole});
        }
    }

    // Keep the pipeline full.
    pumpFolderSizeQueue();
}

// ── Inline rename ─────────────────────────────────────────────────────────────

bool FileSystemModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || index.column() != Name)
        return false;
    if (index.row() < 0 || index.row() >= m_items.size())
        return false;

    const QString newName = value.toString().trimmed();
    // "." and ".." are not names: moveAs(file, dir/..) would move the file into
    // the parent directory instead of renaming it.
    if (newName.isEmpty() || newName.contains(QLatin1Char('/'))
        || newName == QLatin1String(".") || newName == QLatin1String(".."))
        return false;

    // Capture identity up front: the row is replaced in-place below, so reading
    // anything off m_items afterwards would report the *new* item.
    const KFileItem oldItem = m_items.at(index.row());
    if (newName == oldItem.name())
        return false;

    const QUrl oldUrl = oldItem.url();
    QUrl newUrl = oldUrl.adjusted(QUrl::RemoveFilename);
    newUrl.setPath(newUrl.path() + newName);

    // Route through KIO rather than QFile::rename so the operation lands in the
    // undo stack, errors are actually reported, and an existing target is never
    // replaced without the user confirming it (KIO's overwrite prompt — there
    // is no setting to skip it).
    auto *job = KIO::moveAs(oldUrl, newUrl, KIO::DefaultFlags);
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr));
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Rename, {oldUrl}, newUrl, job);

    // The new name already has a row: this is a rename onto an existing file,
    // and the user has yet to answer the overwrite prompt. Swapping the row in
    // place would leave two rows claiming newUrl while the URL index pointed at
    // only one — after an overwrite the old target row stayed as a ghost, and
    // after a cancel the real target became unreachable (deleting it later
    // left its row behind). Leave both rows alone; the lister's deltas update
    // the view once the job has actually done something.
    if (rowForUrl(newUrl) >= 0) {
        connect(job, &KJob::result, this, [this, newUrl](KJob *j) {
            if (!j->error())
                Q_EMIT renamedTo(newUrl);
        });
        return true;
    }

    // Update the row in-place immediately for instant feedback. The lister will
    // shortly emit a matching delta; onItemsAdded/onRefreshItems replace (not
    // duplicate) an already-present URL and restore full stat info.
    // Carry the type over only if it is already known: oldItem.mimetype() could
    // sniff the file on the GUI thread. The lister's delta brings the full item.
    replaceItemAt(index.row(), KFileItem(newUrl,
                                         oldItem.isMimeTypeKnown()
                                             ? oldItem.currentMimeType().name()
                                             : QString(),
                                         oldItem.mode()));
    Q_EMIT dataChanged(this->index(index.row(), 0),
                       this->index(index.row(), ColumnCount - 1));
    Q_EMIT renamedTo(newUrl);

    // If the rename failed or was cancelled, put the optimistic row back. The
    // error itself is reported by the job's own UI delegate (which also skips
    // ERR_USER_CANCELED); a directoryError here would report it a second time,
    // as a banner meant for listing failures.
    connect(job, &KJob::result, this, [this, oldItem, newUrl](KJob *j) {
        if (!j->error())
            return;
        const int row = rowForUrl(newUrl);
        if (row >= 0) {
            replaceItemAt(row, oldItem);
            Q_EMIT dataChanged(this->index(row, 0), this->index(row, ColumnCount - 1));
        }
    });

    return true;
}

// ── KCoreDirLister deltas ─────────────────────────────────────────────────────

void FileSystemModel::onItemsAdded(const QUrl &, const KFileItemList &items)
{
    KFileItemList toAppend;
    toAppend.reserve(items.size());
    for (const KFileItem &it : items) {
        const int row = rowForUrl(it.url());
        if (row >= 0) {
            // Already present (e.g. an in-place rename) — replace with the fully
            // stat'd item instead of adding a duplicate.
            replaceItemAt(row, it);
            Q_EMIT dataChanged(createIndex(row, 0), createIndex(row, ColumnCount - 1));
        } else {
            toAppend.append(it);
        }
    }
    appendItems(toAppend);

    // A folder that appeared while sizes are on gets one too. (During the
    // initial load the panel requests them all once the listing completes.)
    if (m_autoFolderSizes && !m_initialLoading) {
        for (const KFileItem &it : items) {
            if (it.isDir())
                enqueueFolderSize(it.url());
        }
        pumpFolderSizeQueue();
    }

    // Live addition (a file appeared while viewing) — refresh counts / empty label.
    // Suppressed during the initial bulk load; completed() drives that once.
    if (m_listingError)
        return;
    if (m_initialLoading)
        Q_EMIT loadingProgress(m_items.size());
    else
        Q_EMIT directoryRefreshed(m_currentDir);
}

void FileSystemModel::onItemsDeleted(const KFileItemList &items)
{
    QList<int> rows;
    rows.reserve(items.size());
    for (const KFileItem &it : items) {
        const int r = rowForUrl(it.url());
        if (r >= 0)
            rows.append(r);
        onRowDropped(it.url());
    }
    removeRowsAt(rows);

    // Live removal (files deleted/moved out) — refresh counts and, when the folder
    // just became empty, surface the "folder is empty" label. While the directory
    // itself is gone, its rows draining away must not read as "empty".
    if (!m_initialLoading && !m_listingError)
        Q_EMIT directoryRefreshed(m_currentDir);
}

void FileSystemModel::onRefreshItems(const QList<QPair<KFileItem, KFileItem>> &items)
{
    for (const auto &[oldItem, newItem] : items) {
        const bool urlChanged = oldItem.url() != newItem.url();
        int row = rowForUrl(oldItem.url());
        if (urlChanged) {
            const int targetRow = rowForUrl(newItem.url());
            if (row < 0) {
                row = targetRow;
            } else if (targetRow >= 0 && targetRow != row) {
                // Renamed onto a name that already has a row (an overwrite made
                // elsewhere). Keep one row per URL: drop the source row and
                // refresh the target, rather than letting two rows claim it.
                onRowDropped(oldItem.url());
                removeRowsAt({row});
                row = rowForUrl(newItem.url());
            }
        }
        if (row < 0)
            continue;
        m_folderSizes.remove(oldItem.url()); // size may be stale after a change
        // The file's contents may have changed, so any cached thumbnail for it
        // is stale too. (The on-disk shared cache is keyed by mtime, so it
        // re-generates correctly by itself.)
        if (ThumbnailLoader *loader = thumbnailLoader()) {
            loader->forget(oldItem.url());
            loader->forget(newItem.url());
        }
        replaceItemAt(row, newItem);
        Q_EMIT dataChanged(createIndex(row, 0), createIndex(row, ColumnCount - 1));
        // Its size was just dropped as possibly stale; with sizes on, compute
        // the new one rather than leaving the cell blank until the next visit.
        if (m_autoFolderSizes && newItem.isDir())
            enqueueFolderSize(newItem.url());
    }
    if (m_autoFolderSizes)
        pumpFolderSizeQueue();
}

void FileSystemModel::onListerClear()
{
    clearItems();
}

void FileSystemModel::onListerCompleted()
{
    if (m_listingError) // a failed listing must not surface as "loaded" / "empty"
        return;
    m_initialLoading = false; // bulk load finished; later deltas may refresh freely
    if (m_fastRelistRunning) {
        m_fastRelistRunning = false;
        const qint64 now = m_clock.elapsed();
        m_lastFastRelistMs = now - m_fastRelistStartedMs;

        // Constant change? Count and cost over the window decide. The
        // catch-up after a resume answers the earlier churn, not new churn.
        if (!std::exchange(m_fastCatchUp, false))
            m_fastRelists.append({m_fastRelistStartedMs, m_lastFastRelistMs});
        qint64 spent = 0;
        for (auto it = m_fastRelists.begin(); it != m_fastRelists.end();) {
            if (it->first + it->second < now - kFastWindowMs) {
                it = m_fastRelists.erase(it);
            } else {
                spent += it->second;
                ++it;
            }
        }
        if (m_fastRelists.size() > kMaxFastRelists
            || spent > kMaxFastRelistShare * kFastWindowMs) {
            pauseFastPickup();
        } else if (m_fastRelistAgain) {
            m_fastRelistAgain = false;
            m_fastRelistTimer->start(qMax<qint64>(kFastRelistDelayMs, 2 * m_lastFastRelistMs));
        }
    }
    if (m_emitFullOnNextCompleted) {
        m_emitFullOnNextCompleted = false;
        Q_EMIT directoryLoaded(m_currentDir);
    } else {
        // A live update settled (KCoreDirLister auto-refresh) — light handler only.
        Q_EMIT directoryRefreshed(m_currentDir);
    }
}

void FileSystemModel::optimisticallyRemove(const QList<QUrl> &urls)
{
    QList<int> rows;
    rows.reserve(urls.size());
    for (const QUrl &url : urls) {
        const int r = rowForUrl(url);
        if (r >= 0)
            rows.append(r);
        onRowDropped(url);
    }
    removeRowsAt(rows);

    // The lister's own itemsDeleted for these URLs will find nothing to remove.
    // Refresh now so the empty-folder label appears instantly, not only once the
    // lister catches up.
    if (!m_listingError)
        Q_EMIT directoryRefreshed(m_currentDir);
}

void FileSystemModel::immediateRefresh()
{
    if (!m_currentDir.isValid())
        return;
    // Force a re-list now (e.g. right after create/paste). Its completed() is a
    // full load so the panel runs its select-pending / focus routine.
    m_listingError = false;
    m_emitFullOnNextCompleted = true;
    watchCurrentDirectory(); // a manual reload re-arms the watch on a recreated dir
    m_dirLister->updateDirectory(m_currentDir);
}
