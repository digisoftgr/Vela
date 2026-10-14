#include "FileOperationsManager.h"

#include "VelaSettings.h"

#include <KIO/CopyJob>
#include <KIO/DeleteJob>
#include <KIO/FileUndoManager>
#include <KIO/JobUiDelegateFactory>
#include <KIO/MkdirJob>
#include <KIO/SimpleJob>
#include <KIO/StoredTransferJob>
#include <QWidget>

FileOperationsManager *FileOperationsManager::instance()
{
    static FileOperationsManager s_instance;
    return &s_instance;
}

FileOperationsManager::FileOperationsManager(QObject *parent)
    : QObject(parent)
{
}

// Progress for these jobs is reported by KIO itself to the installed job
// tracker (JobTracker), which also sees jobs started outside this class.
static void attachUiDelegate(KJob *job, QWidget *window)
{
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window));
}

QUrl FileOperationsManager::childUrl(const QUrl &dir, const QString &name)
{
    // Not "path + '/' + name": at the root that made "//name", which then
    // matched no listed row.
    QUrl url = dir;
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    url.setPath(path + name);
    return url;
}

// ── Public operations ─────────────────────────────────────────────────────────

KIO::CopyJob *FileOperationsManager::copy(const QList<QUrl> &sources, const QUrl &destDir,
                                          QWidget *window)
{
    if (sources.isEmpty())
        return nullptr;
    auto *job = KIO::copy(sources, destDir);
    attachUiDelegate(job, window);
    KIO::FileUndoManager::self()->recordCopyJob(job);
    return job;
}

KIO::CopyJob *FileOperationsManager::duplicate(const QList<QUrl> &sources, const QUrl &destDir,
                                               QWidget *window)
{
    if (sources.isEmpty())
        return nullptr;
    auto *job = KIO::copy(sources, destDir);
    // Chosen by the worker when the conflict is hit, not predicted up front on
    // the GUI thread: two quick pastes used to both pick "x (1)", and the
    // second failed.
    job->setAutoRename(true);
    attachUiDelegate(job, window);
    KIO::FileUndoManager::self()->recordCopyJob(job);
    return job;
}

KIO::CopyJob *FileOperationsManager::move(const QList<QUrl> &sources, const QUrl &destDir,
                                          QWidget *window)
{
    if (sources.isEmpty())
        return nullptr;
    auto *job = KIO::move(sources, destDir);
    attachUiDelegate(job, window);
    KIO::FileUndoManager::self()->recordCopyJob(job);
    return job;
}

KJob *FileOperationsManager::trash(const QList<QUrl> &sources, QWidget *window)
{
    if (sources.isEmpty())
        return nullptr;
    auto *job = KIO::trash(sources);
    attachUiDelegate(job, window);
    // Same recording Dolphin does: undo restores the files from the trash.
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Trash, sources,
                                            QUrl(QStringLiteral("trash:/")), job);
    return job;
}

KJob *FileOperationsManager::del(const QList<QUrl> &sources, QWidget *window)
{
    if (sources.isEmpty())
        return nullptr;
    auto *job = KIO::del(sources);
    attachUiDelegate(job, window);
    return job;
}

KJob *FileOperationsManager::mkdir(const QUrl &parentDir, const QString &name, QWidget *window)
{
    if (name.isEmpty())
        return nullptr;
    const QUrl dest = childUrl(parentDir, name);
    auto *job = KIO::mkdir(dest);
    attachUiDelegate(job, window);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Mkdir, {}, dest, job);
    return job;
}

KJob *FileOperationsManager::createEmptyFile(const QUrl &dest, QWidget *window)
{
    auto *job = KIO::storedPut(QByteArray(), dest, -1, KIO::HideProgressInfo);
    attachUiDelegate(job, window);
    KIO::FileUndoManager::self()->recordJob(KIO::FileUndoManager::Put, {}, dest, job);
    return job;
}
