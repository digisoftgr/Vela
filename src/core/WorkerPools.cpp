#include "WorkerPools.h"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QThreadPool>

namespace {

// Guards the "is the application still there" check together with the post
// itself, so a worker can never pass the check and then post into an
// application object the GUI thread has destroyed in between.
QMutex s_postMutex;
bool   s_shuttingDown = false;

QThreadPool *makePool(int maxThreads)
{
    // No parent: a QObject parent would delete the pool — and so wait on it —
    // during application teardown.
    auto *pool = new QThreadPool;
    pool->setMaxThreadCount(maxThreads);
    return pool;
}

} // namespace

namespace WorkerPools {

void installShutdownGuard()
{
    QCoreApplication *app = QCoreApplication::instance();
    Q_ASSERT(app);
    QObject::connect(app, &QCoreApplication::aboutToQuit, app, []() {
        QMutexLocker lock(&s_postMutex);
        s_shuttingDown = true;
    });
}

QThreadPool *io()
{
    static QThreadPool *pool = makePool(qMax(8, QThread::idealThreadCount()));
    return pool;
}

QThreadPool *search()
{
    // Half the logical cores. NOTE: this split is inherited and unmeasured —
    // directory traversal blocks on syscalls rather than CPU, so the full core
    // count may well be faster on NVMe (and slower on a spinning disk, where
    // more parallel readers means more head seeking). Benchmark before changing.
    static QThreadPool *pool = makePool(qMax(2, QThread::idealThreadCount() / 2));
    return pool;
}

QThreadPool *pdf()
{
    static QThreadPool *pool = makePool(1);
    return pool;
}

bool postToGui(std::function<void()> fn)
{
    QMutexLocker lock(&s_postMutex);
    QCoreApplication *app = QCoreApplication::instance();
    if (s_shuttingDown || !app)
        return false;
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
    return true;
}

} // namespace WorkerPools
