#pragma once

#include <KFileItem>
#include <QFileInfo>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <atomic>
#include <sys/stat.h>
#include <memory>

class SearchEngine : public QObject
{
    Q_OBJECT

public:

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
    void resultsBatch(const KFileItemList &items);
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
        dev_t              rootDevice = 0; // compared when !crossFilesystems
        std::atomic<int>   pendingTasks{1};
        // Directories discovered but not yet dispatched. Bounded dispatch means
        // the pool queue can't grow to one entry per directory in the tree.
        QMutex             queueMutex;
        QList<QString>     pendingDirs;
        int                activeTasks = 0;   // guarded by queueMutex

        bool stale() const
        {
            return shared->generation.load(std::memory_order_relaxed) != gen;
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
    static void postFinished(const ContextPtr &ctx);

    SharedPtr m_shared;
    bool      m_running = false; // GUI thread only
};
