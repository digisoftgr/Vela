#pragma once

#include <KFileItem>
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QIcon>
#include <QList>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <memory>

class KDirLister;
class KFileItemList;
class QLabel;
class QHeaderView;
class QListView;

/**
 * A peek at a folder: its first entries, folders first, sorted the way a
 * human would (case-insensitive, natural numbers), each with icon and size.
 * The resting state of a folder preview — cheap, capped and unwatched. It is
 * not a file view: it only reports how the user reached for it (focus, a
 * click, a double-click, a right-click), and the preview swaps in a full file
 * view and carries the gesture out there. Capped: at most kMaxEntries entries
 * are stat'ed and listed, whatever the folder holds, with a footer saying
 * how many were left out.
 *
 * A click is reported at once; a double-click cannot reach the snapshot, as
 * the first click already replaced it (the preview catches the second one).
 *
 * Local folders are read on a worker thread: directory names come from a
 * single readdir pass (cheap even for huge folders), and only the entries
 * that survive the sort + cap get a KFileItem (the stat + mime lookup that
 * actually costs something). Remote folders go through KDirLister, which is
 * stopped once the cap is reached.
 */
class FolderPreview : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kMaxEntries = 100;

    struct Entry {
        QUrl    url;
        QString name;
        QString sizeText;
        QString iconName;
        bool    isDir = false;
        // Built on the worker (local) or by the lister (remote), with its type
        // already determined, so opening it never stats on the GUI thread.
        KFileItem item;
    };

    explicit FolderPreview(QWidget *parent = nullptr);
    ~FolderPreview() override;

    void setFolder(const QUrl &url, bool showHidden);
    void clear();

Q_SIGNALS:
    // Keyboard focus arrived (Tab), or a click landed on no row.
    void engaged();
    void rowClicked(const KFileItem &item);
    // Right-click; `item` is null on empty space.
    void contextMenuRequested(const KFileItem &item, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    KFileItem itemAt(const QModelIndex &index) const;

    struct Listing {
        QList<Entry> entries;
        int  total     = 0;    // entries seen before the cap
        bool truncated = false;
        bool failed    = false;
    };

    void listLocal(const QString &path, bool showHidden);
    void listRemote(const QUrl &url, bool showHidden);
    void present(const Listing &listing);
    static Entry entryFor(const KFileItem &item);

    class Model;
    Model     *m_model  = nullptr;
    QListView *m_view   = nullptr;
    // Stands where the full view's column header will be (Details only), so
    // going live does not push the rows down. Shows the Name column alone.
    QHeaderView *m_header = nullptr;
    QLabel    *m_footer = nullptr;
    // Centred over the list, as a tab shows it (Panel's m_emptyDirLabel).
    QLabel    *m_emptyLabel = nullptr;

    // Only the newest request may deliver. Bumped on every setFolder/clear,
    // so a slow listing of a folder that is no longer selected is dropped
    // rather than painted over the current one.
    int m_generation = 0;
    // Worker-visible mirror of m_generation; a listing queued behind others
    // is skipped once superseded.
    std::shared_ptr<std::atomic<int>> m_liveGeneration = std::make_shared<std::atomic<int>>(0);
    QFutureWatcher<Listing> *m_watcher = nullptr;
    KDirLister *m_lister = nullptr;
    int m_remoteGeneration = -1; // generation the current remote listing belongs to
    // Everything listed so far: the cap is applied after sorting, as for a
    // local folder. Capping on arrival kept the first 100 in *server* order,
    // so a remote preview showed an arbitrary subset.
    KFileItemList m_remoteItems;
};
