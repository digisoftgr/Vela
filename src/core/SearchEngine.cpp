#include "SearchEngine.h"

#include "StringMatch.h"
#include "WorkerPools.h"

#include <KIO/UDSEntry>
#include <kio_version.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QThread>
#include <QThreadPool>
#include <sys/stat.h>

SearchEngine::SearchEngine(QObject *parent)
    : QObject(parent)
    , m_shared(std::make_shared<Shared>())
{
    m_shared->owner = this;
}

SearchEngine::~SearchEngine()
{
    // Every task sees itself stale and exits at its next check. No wait: tasks
    // hold only m_shared and their context, and whatever they post is dropped
    // on delivery because `owner` is null by then. A task stuck in a syscall on
    // a dead mount simply finishes (or doesn't) on its own.
    ++m_shared->generation;
}

void SearchEngine::cancel()
{
    // Synchronous: once this returns nothing from the cancelled search reaches
    // the receiver — not batches already queued, and not finished(). The caller
    // initiated the stop, so it needs no notification; isRunning() is false now.
    ++m_shared->generation;
    m_running = false;
}

void SearchEngine::search(const QUrl &directory, const QString &term, bool crossFilesystems)
{
    // The generation bump retires any running search: its tasks see themselves
    // stale and exit, and whatever they already posted is dropped on delivery.
    const int gen = ++m_shared->generation;
    m_running = true;

    if (!directory.isLocalFile()) {
        m_running = false;
        Q_EMIT finished();
        return;
    }

    auto ctx = std::make_shared<SearchContext>();
    ctx->shared = m_shared;
    ctx->term = term;
    ctx->gen  = gen;

    // Pre-compile glob regex once per search (not per file, not per task).
    if (term.contains(QLatin1Char('*')) || term.contains(QLatin1Char('?'))) {
        ctx->globRe = QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(
                term, QRegularExpression::UnanchoredWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
    }

    // Unlimited in practice; the cap only bounds a pathological tree.
    static constexpr int kMaxDepth = 1000;
    const int maxDepth = kMaxDepth;
    const QString rootPath = directory.toLocalFile();

    // One stat of the root, on the GUI thread: the directory is the one the
    // panel just listed, so it is answered from the inode cache. A root that
    // cannot be stat'ed disables the check rather than the search.
    ctx->crossFilesystems = crossFilesystems;
    if (!crossFilesystems) {
        struct stat rootSt;
        if (::stat(QFile::encodeName(rootPath).constData(), &rootSt) == 0)
            ctx->rootDevice = rootSt.st_dev;
        else
            ctx->crossFilesystems = true;
    }

    {
        QMutexLocker lock(&ctx->queueMutex);
        ++ctx->activeTasks;
    }
    WorkerPools::search()->start([rootPath, maxDepth, ctx]() {
        runTask(rootPath, maxDepth, ctx);
    });
}

// Move discovered directories from the shared backlog into the pool, never
// letting more tasks be in flight than the pool has threads. Without this the
// old code queued one runnable per directory up front, so scanning a large tree
// built a runnable queue proportional to the number of directories in it.
void SearchEngine::dispatchPending(const ContextPtr &ctx)
{
    QThreadPool *pool = WorkerPools::search();
    const int maxInFlight = pool->maxThreadCount();

    for (;;) {
        QString path;
        int depth = 0;
        {
            QMutexLocker lock(&ctx->queueMutex);
            if (ctx->activeTasks >= maxInFlight || ctx->pendingDirs.isEmpty())
                return;
            // Entries are "depth:path" so the backlog stays a flat string list.
            const QString entry = ctx->pendingDirs.takeLast();
            const int sep = entry.indexOf(QLatin1Char(':'));
            depth = QStringView(entry).left(sep).toInt();
            path  = entry.mid(sep + 1);
            ++ctx->activeTasks;
        }
        pool->start([path, depth, ctx]() {
            runTask(path, depth, ctx);
        });
    }
}

// Workers never emit directly. A worker-side generation check followed by an
// emit is a race: the GUI thread can start a new search in between, and the
// stale batch (or finished()) lands in the new one. Posting to the GUI thread
// instead makes the check and the emit one step there — the only thread that
// changes the generation or destroys the engine — so they cannot interleave.
void SearchEngine::postBatch(const ContextPtr &ctx, const KFileItemList &items)
{
    WorkerPools::postToGui([shared = ctx->shared, gen = ctx->gen, items]() {
        SearchEngine *engine = shared->owner.data();
        if (engine && gen == shared->generation.load(std::memory_order_relaxed))
            Q_EMIT engine->resultsBatch(items);
    });
}

void SearchEngine::postFinished(const ContextPtr &ctx)
{
    WorkerPools::postToGui([shared = ctx->shared, gen = ctx->gen]() {
        SearchEngine *engine = shared->owner.data();
        if (!engine || gen != shared->generation.load(std::memory_order_relaxed))
            return;
        engine->m_running = false;
        Q_EMIT engine->finished();
    });
}

void SearchEngine::runTask(const QString &path, int depth, ContextPtr ctx)
{
    const auto stale = [&ctx]() { return ctx->stale(); };

    if (stale() || depth <= 0) {
        taskDone(ctx);
        return;
    }

    // QDirIterator rather than QDir::entryInfoList(): no per-directory name
    // sort (the proxy sorts the results), and on Linux the file type comes
    // from readdir's d_type, so walking a directory costs no stat per entry.
    // Only entries whose name matches are stat'ed below — once.
    QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);

    // Accumulate results locally — no mutex needed since this is per-task state.
    // Emit in batches to avoid flooding the event loop with one signal per file.
    static constexpr int kBatchSize = 100;
    KFileItemList localBatch;
    localBatch.reserve(kBatchSize);

    QList<QString> discovered;

    while (it.hasNext()) {
        if (stale())
            break;
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString name = fi.fileName();

        // Folders at the depth limit are not queued: their task used to start
        // only to return immediately.
        if (depth > 1 && fi.isDir() && !fi.isSymLink()) {
            // Skipping symlinked directories also removes the traversal-loop
            // risk that the depth cap was previously the only guard against.
            const QString dirPath = fi.absoluteFilePath();
            bool descend = true;
            if (!ctx->crossFilesystems) {
                // A mountpoint is a directory whose device differs from the
                // root's. One stat per directory, not per file; the entry is
                // still matched below, only not entered.
                struct stat dst;
                descend = ::stat(QFile::encodeName(dirPath).constData(), &dst) == 0
                          && dst.st_dev == ctx->rootDevice;
            }
            if (descend)
                discovered.append(QString::number(depth - 1) + QLatin1Char(':') + dirPath);
        }

        if (!matchesName(name, ctx->term, ctx->globRe))
            continue;

        const QString filePath = fi.absoluteFilePath();
        const QByteArray native = QFile::encodeName(filePath);
        struct stat st;
        if (::stat(native.constData(), &st) != 0 && ::lstat(native.constData(), &st) != 0)
            continue; // vanished, or a dangling symlink we cannot even lstat

        localBatch.append(makeItem(filePath, name, st));
        if (localBatch.size() >= kBatchSize) {
            postBatch(ctx, localBatch);
            localBatch.clear();
        }
    }

    // Flush any remaining results for this directory.
    if (!localBatch.isEmpty() && !stale())
        postBatch(ctx, localBatch);

    if (!discovered.isEmpty() && !stale()) {
        // Count the children before this task retires, so pendingTasks can never
        // hit zero while work remains queued.
        ctx->pendingTasks.fetch_add(discovered.size(), std::memory_order_relaxed);
        QMutexLocker lock(&ctx->queueMutex);
        ctx->pendingDirs.append(discovered);
    }

    taskDone(ctx);
}

void SearchEngine::taskDone(const ContextPtr &ctx)
{
    {
        QMutexLocker lock(&ctx->queueMutex);
        if (ctx->activeTasks > 0)
            --ctx->activeTasks;
    }

    // A stale search stops dispatching; its backlog is simply dropped with ctx.
    // Nobody waits on its pendingTasks reaching zero — cancel()/search() already
    // retired it — so the undispatched count is harmless.
    if (ctx->stale())
        return;

    // Pull more work in before deciding whether the search is over.
    dispatchPending(ctx);

    // The last task posts finished(); it is re-checked against the generation
    // on the GUI thread, so a search started meanwhile is not reported as done.
    if (ctx->pendingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
        postFinished(ctx);
}

bool SearchEngine::matchesName(const QString &name, const QString &term,
                               const QRegularExpression &globRe)
{
    if (term.isEmpty())
        return true;
    if (!globRe.pattern().isEmpty() && globRe.isValid())
        return globRe.match(name).hasMatch();
    return subsequenceMatch(term, name, Qt::CaseInsensitive);
}

// A fully populated item, built from the stat the worker already did. The old
// KFileItem(url, mime, mode) carried no size, times or permissions, so KFileItem
// lstat'ed each file lazily — on the GUI thread, the first time a Size/Date
// cell or a size sort touched it.
KFileItem SearchEngine::makeItem(const QString &filePath, const QString &name,
                                 const struct stat &st)
{
    // Resolve the MIME type here on the worker thread using extension
    // matching (no file I/O). Without this, iconName() on the main thread
    // triggers a lazy determineMimeType() that can block on I/O when many
    // unseen rows become visible during a fast scrollbar drag.
    static QMimeDatabase mimeDb;
    const bool isDir = S_ISDIR(st.st_mode);
    const QString mimeType = isDir
        ? QStringLiteral("inode/directory")
        : mimeDb.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();

    KIO::UDSEntry entry;
#if KIO_VERSION >= QT_VERSION_CHECK(6, 29, 0)
    entry.reserveStrings(3);
    entry.reserveNumbers(7);
#else
    entry.reserve(10);
#endif
    entry.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    entry.fastInsert(KIO::UDSEntry::UDS_LOCAL_PATH, filePath);
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, st.st_mode & S_IFMT);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS, st.st_mode & 07777);
    entry.fastInsert(KIO::UDSEntry::UDS_SIZE, st.st_size);
    entry.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME, st.st_mtime);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS_TIME, st.st_atime);
    entry.fastInsert(KIO::UDSEntry::UDS_LOCAL_USER_ID, st.st_uid);
    entry.fastInsert(KIO::UDSEntry::UDS_LOCAL_GROUP_ID, st.st_gid);
    entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mimeType);
    return KFileItem(entry, QUrl::fromLocalFile(filePath));
}
