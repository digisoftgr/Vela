#pragma once

#include "FileItemModelBase.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QUrl>
#include <KFileItem>
#include <KIO/Global>
#include <KIO/Job>

class KCoreDirLister;
class KDirWatch;
class QTimer;

class FileSystemModel : public FileItemModelBase
{
    Q_OBJECT

public:
    explicit FileSystemModel(QObject *parent = nullptr);
    ~FileSystemModel() override;

    // reload = bypass KCoreDirLister's cache and list from the source. Needed
    // when the rows no longer mirror the lister (after an optimistic removal
    // whose job failed); updateDirectory() would diff against the lister's
    // own view and find nothing to restore.
    void setDirectory(const QUrl &url, bool reload = false);
    QUrl directory() const;

    void setShowHidden(bool show);
    bool showHidden() const;

    void setFoldersFirst(bool first);
    bool foldersFirst() const;

    void requestAllFolderSizes();  // queues size jobs for every visible folder
    void clearFolderSizes();       // cancels pending jobs and clears cached sizes

    void optimisticallyRemove(const QList<QUrl> &urls);
    void immediateRefresh();
    // A light re-list soon, coalesced and throttled like the fast watch below.
    // For callers that know the directory likely changed (a terminal command
    // just finished). Unlike immediateRefresh() it is a live update, not a
    // load: no focus or selection routine runs when it lands.
    void refreshSoon();

    // Re-checks that the listed directory is still on disk, reporting it through
    // directoryError if not. The KDirWatch below covers deletions live; this is
    // the safety net for what inotify cannot see — a moved-away parent, a remote
    // location. A single stat, run on a worker thread so a hung mount cannot
    // freeze the window; a missing directory is reported asynchronously.
    void revalidateDirectory();

    // True once the current listing has failed or its directory disappeared.
    // The panel uses it to suppress the "folder is empty" overlay: an empty row
    // list after a failure means "unknown", not "empty".
    bool hasListingError() const { return m_listingError; }

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

Q_SIGNALS:
    void directoryLoaded(const QUrl &url);
    void directoryRefreshed(const QUrl &url); // emitted after an incremental live update
    void loadingStarted(const QUrl &url);
    // Emitted per batch while the initial bulk listing is still streaming in,
    // carrying the number of rows so far. Deliberately not directoryRefreshed:
    // that one makes the panel recount folders vs files, which is a full walk of
    // the model and would turn a large load into O(rows²). This is O(1) and only
    // drives the loading indicator.
    void loadingProgress(int rowCount);
    void directoryError(const QString &message);
    void renamedTo(const QUrl &newUrl); // emitted after successful inline rename

protected:
    QVariant folderSizeData(const KFileItem &item, int role) const override;
    void onRowDropped(const QUrl &url) override;

private Q_SLOTS:
    // KCoreDirLister deltas
    void onItemsAdded(const QUrl &dirUrl, const KFileItemList &items);
    void onItemsDeleted(const KFileItemList &items);
    void onRefreshItems(const QList<QPair<KFileItem, KFileItem>> &items);
    void onListerClear();
    void onListerCompleted();

    void onFolderSizeJobDone(const QUrl &url, KJob *job);

    void onWatchedPathDeleted(const QString &path);
    void onFastWatchFired();

private:
    // Reports the current directory as gone: stops the listing, drops the stale
    // rows, and emits directoryError with the same wording KIO produces when a
    // navigation to a missing path fails.
    void reportDirectoryGone();
    void watchCurrentDirectory();
    void startFastRelist();
    void pauseFastPickup();
    void resetFastPickup();

    // Folder-size jobs are throttled: each KIO::directorySize is a full recursive
    // scan, and firing one per subfolder at once thrashes the disk (hundreds of
    // concurrent traversals on a large directory). Queue them and keep at most
    // kMaxConcurrentSizeJobs in flight.
    static constexpr int kMaxConcurrentSizeJobs = 8;
    void enqueueFolderSize(const QUrl &url);
    void pumpFolderSizeQueue();
    void schedulePumpFolderSizeQueue();
    void cancelAllFolderSizeJobs();

    QUrl m_currentDir;
    KCoreDirLister *m_dirLister = nullptr;
    // Own instance rather than KDirWatch::self(): the shared one also carries
    // every path KIO itself registers, so its deleted() would have to be
    // filtered on each emission. Instances share one backend, so this adds no
    // extra inotify watch beyond the directory itself.
    KDirWatch *m_dirWatch = nullptr;
    QString m_watchedPath;  // local path currently registered with m_dirWatch

    // ── Fast change pickup ────────────────────────────────────────────────────
    // KCoreDirLister reports outside changes ~700 ms late: KDirWatch batches
    // inotify events for 500 ms, then the lister waits 200 ms more (both fixed
    // in KF6; measured on tmpfs, ext4 and ntfs alike). A plain inotify watch on
    // the shown directory triggers the lister's re-list right away instead.
    //
    // One watcher (one inotify instance) is shared by every model; see
    // acquireFastWatch(). Local, non-network mounts only — adding a watch on a
    // hung NFS mount blocks.
    //
    // Bursts are coalesced over kFastRelistDelayMs, and at most one re-list runs
    // at a time, spaced by twice the last one's duration. Under constant change
    // the fast path pauses: more than kMaxFastRelists re-lists, or more than
    // kMaxFastRelistShare of the time spent re-listing, within kFastWindowMs.
    // KIO's own ~700 ms batching keeps the view correct meanwhile; once the
    // directory has been quiet for kFastResumeQuietMs, one catch-up re-list runs
    // and the fast path resumes.
    static constexpr int    kFastRelistDelayMs  = 30;
    static constexpr int    kFastWindowMs       = 2000;
    static constexpr int    kMaxFastRelists     = 5;
    static constexpr double kMaxFastRelistShare = 0.25;
    static constexpr int    kFastResumeQuietMs  = 2000;
    QTimer *m_fastRelistTimer = nullptr;
    QTimer *m_fastResumeTimer = nullptr;  // restarted by every event while paused
    QElapsedTimer m_clock;                // monotonic time base, started in the ctor
    QString m_fastWatchedPath;
    bool    m_fastRelistRunning = false;  // our updateDirectory() has not completed yet
    bool    m_fastRelistAgain = false;    // a change arrived while it ran
    bool    m_fastPaused = false;
    bool    m_fastCatchUp = false;        // the re-list after a resume: not churn, not counted
    qint64  m_fastRelistStartedMs = 0;
    qint64  m_lastFastRelistMs = 0;       // duration of the last one, for the spacing
    QList<QPair<qint64, qint64>> m_fastRelists; // (start, duration) within the window
    QSet<QString> m_revalidating; // paths with an existence check in flight

    bool m_showHidden = false;
    bool m_foldersFirst = true;             // display order is owned by the proxy model
    bool m_emitFullOnNextCompleted = false; // next completed() is a navigation/explicit load
    bool m_listingError = false;            // the current listing failed (suppresses loaded/empty)
    bool m_initialLoading = false;          // between setDirectory and its first completed()

    QHash<QUrl, KIO::filesize_t> m_folderSizes;
    QHash<QUrl, KJob *> m_runningSizeJobs;  // in flight, capped at kMaxConcurrentSizeJobs
    QList<QUrl>         m_sizeQueue;        // waiting to start
    QSet<QUrl>          m_queuedSizes;      // O(1) membership test for m_sizeQueue
    bool                m_pumpScheduled = false;
};
