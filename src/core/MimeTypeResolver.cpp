#include "MimeTypeResolver.h"

#include "WorkerPools.h"

#include <QCoreApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <QThreadPool>

#include <KDesktopFile>

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
    // The shared I/O pool rather than a private two-thread one: a sniff stuck
    // on a hung mount then ties up one of many threads, instead of half of the
    // only two that served every panel's Type column.
    WorkerPools::io()->start([shared = m_shared, generation, self, url, path]() {
        if (shared->generation.load() != generation)
            return; // cancelled while queued — skip the read entirely

        // QMimeDatabase is documented as thread-safe. MatchDefault reads the
        // file's leading bytes only when the name is not decisive.
        static QMimeDatabase db;
        const QString name = db.mimeTypeForFile(path).name();
        // A .desktop entry describes itself; its Comment is the type text.
        // Read here, not via KFileItem::mimeComment() on the GUI thread.
        QString comment;
        QString iconName;
        if (path.endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive)) {
            const KDesktopFile desktop(path);
            comment  = desktop.readComment();
            iconName = desktop.readIcon();
        }
        if (iconName.isEmpty())
            iconName = db.mimeTypeForName(name).iconName();

        if (shared->generation.load() != generation)
            return;
        WorkerPools::postToGui([shared, generation, self, url, name, comment, iconName]() {
            // GUI thread: the generation and the pointer are checked where
            // they are changed, so neither can move under us.
            if (!self || shared->generation.load() != generation)
                return;
            self->m_pending.remove(url);
            Q_EMIT self->resolved(url, name, comment, iconName);
        });
    });
}
