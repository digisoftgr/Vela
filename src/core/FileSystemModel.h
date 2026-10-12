#pragma once

#include "FileItemModelBase.h"

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

private:
    // Reports the current directory as gone: stops the listing, drops the stale
    // rows, and emits directoryError with the same wording KIO produces when a
    // navigation to a missing path fails.
    void reportDirectoryGone();
    void watchCurrentDirectory();

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
