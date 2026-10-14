#include "FolderPreview.h"

#include "core/VelaSettings.h"
#include "core/WorkerPools.h"

#include <KDirLister>
#include <KFileItem>
#include <KFormat>
#include <KLocalizedString>

#include <QCollator>
#include <QDirIterator>
#include <QFocusEvent>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QLabel>
#include <QListView>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

// ── Header ───────────────────────────────────────────────────────────────────

// A header for a layout. QTreeView places its own at exactly sizeHint(); in a
// layout a header gets at least minimumSizeHint(), which — it being a scroll
// area — reserves room for scroll bars (68 px against 21 measured). Answering
// with the natural height instead keeps it equal to the tab's header through
// font, style, DPI and screen changes, since the layout asks every time.
namespace {
class NaturalHeightHeader : public QHeaderView
{
public:
    using QHeaderView::QHeaderView;
    QSize minimumSizeHint() const override { return sizeHint(); }
};
} // namespace

// ── Model ────────────────────────────────────────────────────────────────────

class FolderPreview::Model : public QAbstractListModel
{
public:
    using QAbstractListModel::QAbstractListModel;

    void setEntries(QList<Entry> entries)
    {
        beginResetModel();
        m_entries = std::move(entries);
        // Resolved once per listing (≤ kMaxEntries rows, a handful of distinct
        // names), not per paint: QIcon::fromTheme is a theme lookup each time.
        // Here rather than on the worker, where theme icons must not be built.
        QHash<QString, QIcon> byName;
        m_icons.clear();
        m_icons.reserve(m_entries.size());
        for (const Entry &e : std::as_const(m_entries)) {
            const QString key = e.iconName + (e.isDir ? QLatin1Char('/') : QLatin1Char(' '));
            auto it = byName.constFind(key);
            if (it == byName.constEnd()) {
                it = byName.insert(key, QIcon::fromTheme(e.iconName, QIcon::fromTheme(
                    e.isDir ? QStringLiteral("folder") : QStringLiteral("unknown"))));
            }
            m_icons.append(it.value());
        }
        endResetModel();
    }

    const Entry &entryAt(int row) const { return m_entries.at(row); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_entries.size())
            return {};
        const Entry &e = m_entries.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            return e.isDir || e.sizeText.isEmpty()
                ? e.name
                : QStringLiteral("%1  —  %2").arg(e.name, e.sizeText);
        case Qt::DecorationRole:
            return m_icons.value(index.row());
        case Qt::ToolTipRole:
            return e.name;
        case Qt::UserRole:
            return e.url;
        default:
            return {};
        }
    }

private:
    QList<Entry> m_entries;
    QList<QIcon> m_icons; // parallel to m_entries
};

// ── Widget ───────────────────────────────────────────────────────────────────

FolderPreview::FolderPreview(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_model = new Model(this);
    m_view  = new QListView(this);
    m_view->setModel(m_model);
    // Nothing is selected here: every gesture is handed up, and the full view
    // that replaces this listing does the selecting. Focusable, so Tab can
    // reach the preview as well as the mouse.
    m_view->setSelectionMode(QAbstractItemView::NoSelection);
    m_view->setFocusPolicy(Qt::StrongFocus);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Scrolls within its own height. At either end the wheel falls through
    // to the preview column around it.
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setUniformItemSizes(true);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setTextElideMode(Qt::ElideMiddle);
    m_view->setAlternatingRowColors(true);
    m_view->setMouseTracking(true);
    m_view->setCursor(Qt::PointingHandCursor);
    connect(m_view, &QListView::clicked, this, [this](const QModelIndex &index) {
        const KFileItem item = itemAt(index);
        if (!item.isNull())
            Q_EMIT rowClicked(item);
    });
    connect(m_view, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
        Q_EMIT contextMenuRequested(itemAt(m_view->indexAt(pos)),
                                    m_view->viewport()->mapToGlobal(pos));
    });

    // Same text and look as a tab's, and mouse-transparent like it: a click
    // there is a click on the empty list, which wakes the full view.
    m_emptyLabel = new QLabel(i18n("This folder is empty"), m_view->viewport());
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: palette(placeholderText); font-size: 13px;"));
    m_emptyLabel->hide();

    m_footer = new QLabel(this);
    m_footer->setContentsMargins(8, 4, 8, 4);
    m_footer->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_footer->setWordWrap(true);
    m_footer->hide();

    // Same class, font and alignment as the tab's header, so the same height.
    // Inert: no sorting, no resizing — a click on it only wakes the full view.
    auto *headerModel = new QStandardItemModel(0, 1, this);
    headerModel->setHorizontalHeaderItem(0, new QStandardItem(i18n("Name")));
    m_header = new NaturalHeightHeader(Qt::Horizontal, this);
    m_header->setModel(headerModel);
    m_header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_header->setStretchLastSection(true);
    m_header->setSectionsClickable(false);
    m_header->setSectionResizeMode(QHeaderView::Fixed);
    m_header->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_header->installEventFilter(this);
    // The full view opens in the default view mode; only Details has a header.
    m_header->setVisible(VelaSettings::self()->defaultViewMode() == 0);

    layout->addWidget(m_header);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_footer);

    m_watcher = new QFutureWatcher<Listing>(this);
    connect(m_watcher, &QFutureWatcher<Listing>::finished, this, [this]() {
        // The watcher carries the generation it was started for in its
        // property; anything older than the current request is stale.
        if (m_watcher->property("generation").toInt() != m_generation)
            return;
        present(m_watcher->result());
    });
}

// No wait for a running listing: the task captures only its arguments (never
// `this`), and the watcher, a child, disconnects from the future when it is
// destroyed — so nothing can report into a dead widget. Waiting here froze
// closing the window while a listing was stuck on a hung mount.
FolderPreview::~FolderPreview() = default;

void FolderPreview::clear()
{
    ++m_generation;
    m_liveGeneration->store(m_generation);
    if (m_lister)
        m_lister->stop();
    m_model->setEntries({});
    m_footer->hide();
    m_emptyLabel->hide();
}

KFileItem FolderPreview::itemAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= m_model->rowCount())
        return {};
    return m_model->entryAt(index.row()).item;
}

bool FolderPreview::eventFilter(QObject *watched, QEvent *event)
{
    // Focus by keyboard (or by code) is a gesture of its own. By mouse it is
    // not: the click that brought it decides.
    if (watched == m_view && event->type() == QEvent::FocusIn) {
        const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
        if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
            || reason == Qt::ShortcutFocusReason || reason == Qt::OtherFocusReason)
            Q_EMIT engaged();
    }
    if (watched == m_view->viewport() && event->type() == QEvent::Resize)
        m_emptyLabel->setGeometry(m_view->viewport()->rect());
    // A left click on the header: no sorting here, but the user reached for it.
    if (watched == m_header && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        Q_EMIT engaged();
    // A left click on no row: nothing to act on, but the user reached for it.
    if (watched == m_view->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && !m_view->indexAt(me->position().toPoint()).isValid())
            Q_EMIT engaged();
    }
    return QWidget::eventFilter(watched, event);
}

void FolderPreview::setFolder(const QUrl &url, bool showHidden)
{
    clear();
    // Re-read: the setting may have changed since this was built.
    m_header->setVisible(VelaSettings::self()->defaultViewMode() == 0);
    if (url.isLocalFile())
        listLocal(url.toLocalFile(), showHidden);
    else
        listRemote(url, showHidden);
}

// ── Local listing (worker thread) ────────────────────────────────────────────

void FolderPreview::listLocal(const QString &path, bool showHidden)
{
    const int generation = m_generation;
    m_watcher->setProperty("generation", generation);

    const auto live = m_liveGeneration;
    QFuture<Listing> future = QtConcurrent::run(WorkerPools::io(),
                                                [path, showHidden, live, generation]() -> Listing {
        Listing out;
        if (live->load() != generation)
            return out; // superseded while queued; the result is dropped anyway

        // Pass 1: names and dir-ness only. On Linux QDirIterator answers
        // isDir() from readdir's d_type, so this is one syscall stream
        // rather than one stat per entry.
        QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
        if (showHidden)
            filters |= QDir::Hidden;

        struct Name { QString name; bool isDir; };
        QList<Name> names;
        QDirIterator it(path, filters);
        while (it.hasNext()) {
            it.next();
            names.append({it.fileName(), it.fileInfo().isDir()});
        }
        if (names.isEmpty() && !QFileInfo(path).isReadable()) {
            out.failed = true;
            return out;
        }
        out.total = names.size();

        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(names.begin(), names.end(), [&collator](const Name &a, const Name &b) {
            if (a.isDir != b.isDir)
                return a.isDir;
            return collator.compare(a.name, b.name) < 0;
        });

        // Pass 2: the cap. Only these entries pay for a stat + mime lookup.
        const int keep = qMin(names.size(), kMaxEntries);
        out.truncated = names.size() > keep;
        out.entries.reserve(keep);
        const QDir dir(path);
        QMimeDatabase db;
        for (int i = 0; i < keep; ++i) {
            // The type from the name, handed to the item: an empty one made
            // iconName() read each file's leading bytes just to pick an icon.
            const QString mime = names[i].isDir
                ? QStringLiteral("inode/directory")
                : db.mimeTypeForFile(names[i].name, QMimeDatabase::MatchExtension).name();
            const KFileItem item(QUrl::fromLocalFile(dir.filePath(names[i].name)),
                                 mime, KFileItem::Unknown);
            out.entries.append(entryFor(item));
        }
        return out;
    });
    m_watcher->setFuture(future);
}

// ── Remote listing (KDirLister, stopped at the cap) ──────────────────────────

void FolderPreview::listRemote(const QUrl &url, bool showHidden)
{
    if (!m_lister) {
        m_lister = new KDirLister(this);
        m_lister->setAutoUpdate(false);
        m_lister->setDelayedMimeTypes(true); // icons by name; no content reads
        // Every handler checks the generation the listing was started for.
        // clear() bumps it and then calls stop(), which can emit
        // listingDirCanceled synchronously — without the check, that presented
        // the previous remote folder's entries in place of the new one.
        connect(m_lister, &KDirLister::itemsAdded, this,
                [this](const QUrl &, const KFileItemList &items) {
            if (m_remoteGeneration != m_generation)
                return;
            m_remoteItems.append(items);
        });
        auto finish = [this]() {
            if (m_remoteGeneration != m_generation)
                return;
            KFileItemList items = std::exchange(m_remoteItems, {});
            QCollator collator;
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            std::sort(items.begin(), items.end(),
                      [&collator](const KFileItem &a, const KFileItem &b) {
                if (a.isDir() != b.isDir())
                    return a.isDir();
                return collator.compare(a.name(), b.name()) < 0;
            });
            Listing l;
            l.total     = items.size();
            l.truncated = items.size() > kMaxEntries;
            const int keep = qMin(int(items.size()), kMaxEntries);
            l.entries.reserve(keep);
            for (int i = 0; i < keep; ++i)
                l.entries.append(entryFor(items.at(i)));
            present(l);
        };
        connect(m_lister, qOverload<const QUrl &>(&KDirLister::listingDirCompleted), this, finish);
        connect(m_lister, qOverload<const QUrl &>(&KDirLister::listingDirCanceled), this, finish);
        connect(m_lister, &KDirLister::jobError, this, [this](KIO::Job *) {
            if (m_remoteGeneration != m_generation)
                return;
            Listing l;
            l.failed = true;
            present(l);
        });
    }
    m_remoteGeneration = m_generation;
    m_remoteItems.clear();
    m_lister->setShowHiddenFiles(showHidden);
    m_lister->openUrl(url, KDirLister::NoFlags);
}

// ── Presentation ─────────────────────────────────────────────────────────────

void FolderPreview::present(const Listing &listing)
{
    m_model->setEntries(listing.entries);
    if (listing.failed) {
        m_footer->setText(i18n("Could not read this folder."));
        m_footer->show();
        return;
    }
    if (listing.entries.isEmpty()) {
        m_footer->hide();
        m_emptyLabel->setGeometry(m_view->viewport()->rect());
        m_emptyLabel->show();
        return;
    }
    // The list scrolls, so only the cap leaves anything out.
    if (!listing.truncated) {
        m_footer->hide();
        return;
    }
    m_footer->setText(i18np("Showing the first %2 of %1 item",
                            "Showing the first %2 of %1 items",
                            listing.total, int(listing.entries.size())));
    m_footer->show();
}

FolderPreview::Entry FolderPreview::entryFor(const KFileItem &item)
{
    Entry e;
    e.item     = item;
    e.url      = item.url();
    e.name     = item.name();
    e.isDir    = item.isDir();
    e.iconName = item.iconName();
    if (!e.isDir)
        e.sizeText = KFormat().formatByteSize(item.size());
    return e;
}
