#pragma once

#include <KFileItem>
#include <QFileInfo>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <atomic>
#include <sys/stat.h>
#include <memory>

class QTimer;

class SearchEngine : public QObject
{
    Q_OBJECT

public:

    // A search stops after this many results. A short subsequence term over a
    // large tree otherwise matched hundreds of thousands of files, all held in
    // memory (and once more per recently left tab).
    static constexpr int kMaxResults = 10000;

    explicit SearchEngine(QObject *parent = nullptr);
    ~SearchEngine() override;

    // Recursive, case-insensitive name search below `directory` (local only).
    // `term` is a glob when it contains * or ?, otherwise a subsequence match.
    // Unless `crossFilesystems`, the walk stays on the filesystem `directory`
    // is on (`find -xdev`): searching / used to wander into /proc, /sys and
    // every network mount.
    void search(const QUrl &directory, const QString &term, bool crossFilesystems = false);
    // Stops the current search. Synchronous: afterwards isRunning() is false and
    // neither resultsBatch() nor finished() is delivered for it.
    void cancel();
    bool isRunning() const { return m_running; }

Q_SIGNALS:
    // Both are emitted on the engine's (GUI) thread, and only for the current search.
    // Coalesced on the GUI thread: at most one batch per kFlushIntervalMs, so
    // a big tree does not cost one model insert (and one proxy re-sort) per
    // directory that had a match.
    void resultsBatch(const KFileItemList &items);
    // The search stopped at kMaxResults; emitted just before finished().
    void limitReached();
    void finished();

private:
    // State the worker tasks share with the engine. Tasks hold this, never
    // `this`: the engine can then be destroyed while a task is still blocked
    // in readdir() on a hung mount, without waiting for it (the old
    // ~SearchEngine waited unboundedly, freezing panel/window close).
    struct Shared {
        std::atomic<int> generation{0};
        // Set on the GUI thread and only dereferenced there.
        QPointer<SearchEngine> owner;
    };
    using SharedPtr = std::shared_ptr<Shared>;

    // Immutable per-search state, shared by every task of that search instead of
    // being copied into each queued lambda (a large tree queues one task per
    // directory, each of which would otherwise copy the term and the regex).
    struct SearchContext {
        SharedPtr          shared;
        QString            term;
        QRegularExpression globRe;
        int                gen = 0;
        bool               crossFilesystems = false;
        // Mount points strictly below the root, read once from
        // /proc/self/mountinfo, so leaving the root's filesystem costs no
        // stat per directory. Used when !crossFilesystems.
        QSet<QString>      mountPoints;
        // Fallback when mountinfo cannot be read: compare st_dev per directory.
        bool               useDeviceCheck = false;
        dev_t              rootDevice = 0;
        std::atomic<int>   pendingTasks{1};
        std::atomic<int>   found{0};          // results accepted so far
        std::atomic<bool>  limitHit{false};   // found reached kMaxResults
        std::atomic<bool>  finishPosted{false};
        // Directories discovered but not yet dispatched. Bounded dispatch means
        // the pool queue can't grow to one entry per directory in the tree.
        QMutex             queueMutex;
        QList<QString>     pendingDirs;
        int                activeTasks = 0;   // guarded by queueMutex

        bool stale() const
        {
            return shared->generation.load(std::memory_order_relaxed) != gen;
        }
        // Stop walking: retired, or enough results.
        bool stopped() const
        {
            return stale() || limitHit.load(std::memory_order_relaxed);
        }
    };
    using ContextPtr = std::shared_ptr<SearchContext>;

    static void runTask(const QString &path, int depth, ContextPtr ctx);
    static void dispatchPending(const ContextPtr &ctx);
    static bool matchesName(const QString &name, const QString &term,
                            const QRegularExpression &globRe);
    static KFileItem makeItem(const QString &filePath, const QString &name,
                              const struct stat &st);
    static void taskDone(const ContextPtr &ctx);
    static void postBatch(const ContextPtr &ctx, const KFileItemList &items);
    static void postFinished(const ContextPtr &ctx, bool limited);
    static QSet<QString> mountPointsBelow(const QString &root);

    // GUI-thread side of the coalescing.
    void queueResults(const KFileItemList &items);
    void flushResults();

    SharedPtr m_shared;
    bool      m_running = false; // GUI thread only
    KFileItemList m_pendingResults;
    QTimer   *m_flushTimer = nullptr;
    static constexpr int kFlushIntervalMs = 100;
};
