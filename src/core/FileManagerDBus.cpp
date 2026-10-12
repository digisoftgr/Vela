#include "FileManagerDBus.h"
#include "ui/MainWindow.h"

#include <KPropertiesDialog>
#include <QHash>
#include <QList>
#include <QUrl>

// ── org.freedesktop.FileManager1 ────────────────────────────────────────────────

FileManagerDBus::FileManagerDBus(QObject *parent)
    : QObject(parent)
{}

// Navigate to folders. Always a fresh window, raised and focused, holding one
// tab per requested folder — never a directory change in someone's existing
// tab. Everything after the first URI used to be dropped.
void FileManagerDBus::ShowFolders(const QStringList &uris, const QString & /*startupId*/)
{
    QList<MainWindow::FolderTarget> targets;
    for (const QString &uri : uris) {
        const QUrl url(uri);
        if (!url.isValid())
            continue;
        MainWindow::FolderTarget t;
        t.folder = url;
        targets.append(t);
    }
    MainWindow::openTabsInNewWindow(targets);
}

// Reveal files: one new window, one tab per parent folder (in the order the
// folders first appear), with every requested file of that folder selected.
// "Reveal these 3 files" used to select only the first.
void FileManagerDBus::ShowItems(const QStringList &uris, const QString & /*startupId*/)
{
    QList<MainWindow::FolderTarget> targets;
    QHash<QUrl, int> tabForFolder;
    for (const QString &uri : uris) {
        const QUrl fileUrl(uri);
        if (!fileUrl.isValid())
            continue;
        const QString path = fileUrl.adjusted(QUrl::StripTrailingSlash).path();
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        QUrl folder = fileUrl.adjusted(QUrl::StripTrailingSlash);
        folder.setPath(slash > 0 ? path.left(slash) : QStringLiteral("/"));

        auto it = tabForFolder.constFind(folder);
        if (it == tabForFolder.constEnd()) {
            it = tabForFolder.insert(folder, targets.size());
            MainWindow::FolderTarget t;
            t.folder = folder;
            targets.append(t);
        }
        targets[it.value()].reveal.append(fileUrl.adjusted(QUrl::StripTrailingSlash));
    }
    MainWindow::openTabsInNewWindow(targets);
}

// The spec asks for the properties dialog, not a reveal. Previously this just
// forwarded to ShowItems, so "Properties" from another app opened a browser
// window instead.
void FileManagerDBus::ShowItemProperties(const QStringList &uris, const QString & /*startupId*/)
{
    if (uris.isEmpty())
        return;

    QList<QUrl> urls;
    urls.reserve(uris.size());
    for (const QString &uri : uris) {
        const QUrl u(uri);
        if (u.isValid())
            urls.append(u);
    }
    if (urls.isEmpty())
        return;

    // Parent to a live window when there is one so the dialog is not orphaned.
    KPropertiesDialog::showDialog(urls, MainWindow::anyLiveWindow());
}

// ── org.kde.vela.MainWindow (single instance) ───────────────────────────────────

VelaInstanceDBus::VelaInstanceDBus(QObject *parent)
    : QObject(parent)
{}

void VelaInstanceDBus::raise()
{
    MainWindow::raiseExisting();
}

void VelaInstanceDBus::openPaths(const QStringList &paths)
{
    MainWindow::openTargetsInNewWindows(paths);
}
