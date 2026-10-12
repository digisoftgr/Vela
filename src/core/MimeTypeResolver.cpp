#include "MimeTypeResolver.h"

#include "WorkerPools.h"

#include <QCoreApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <QThreadPool>

// One pool for every resolver (two models per panel used to mean two pools
// per panel). This is short, latency-bound I/O (a few hundred bytes per file),
// not throughput work: a couple of threads is plenty and keeps us from
// competing with thumbnail generation for disk time.
QThreadPool *MimeTypeResolver::sharedPool()
{
    // No parent and never deleted: ~QThreadPool waits for running tasks, and
    // a sniff stuck on a hung mount would then freeze quitting. (This used to
    // drain the pool on aboutToQuit with an unbounded waitForDone().) Tasks
    // report through WorkerPools::postToGui, which stops posting once the
    // application starts shutting down.
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setMaxThreadCount(2);
        return p;
    }();
    return pool;
}

MimeTypeResolver::MimeTypeResolver(QObject *parent)
    : QObject(parent)
    , m_shared(std::make_shared<Shared>())
{
}

MimeTypeResolver::~MimeTypeResolver()
{
    // No wait needed any more: tasks hold only the shared state and a guarded
    // pointer, so a stale task skips its read and a finished one finds the
    // pointer null on the GUI thread. (The old unbounded waitForDone() here
    // blocked closing a panel until every queued sniff had run.)
    ++m_shared->generation;
}

void MimeTypeResolver::clear()
{
    // Leaving a folder with 100k extension-less files used to keep sniffing
    // all of them. The generation bump turns every queued task into a no-op.
    ++m_shared->generation;
    m_pending.clear();
}

void MimeTypeResolver::resolve(const QUrl &url)
{
    if (!url.isLocalFile() || m_pending.contains(url))
        return;
    m_pending.insert(url);

    const QString path = url.toLocalFile();
    const int generation = m_shared->generation.load();
    // Created here, on the GUI thread; only ever dereferenced there as well.
    const QPointer<MimeTypeResolver> self(this);
    sharedPool()->start([shared = m_shared, generation, self, url, path]() {
        if (shared->generation.load() != generation)
            return; // cancelled while queued — skip the read entirely

        // QMimeDatabase is documented as thread-safe. MatchDefault reads the
        // file's leading bytes only when the name is not decisive.
        static QMimeDatabase db;
        const QString name = db.mimeTypeForFile(path).name();

        if (shared->generation.load() != generation)
            return;
        WorkerPools::postToGui([shared, generation, self, url, name]() {
            // GUI thread: the generation and the pointer are checked where
            // they are changed, so neither can move under us.
            if (!self || shared->generation.load() != generation)
                return;
            self->m_pending.remove(url);
            Q_EMIT self->resolved(url, name);
        });
    });
}
