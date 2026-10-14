#include "Panel.h"

#include "BreadcrumbBar.h"
#include "MainWindow.h"
#include "FilterBar.h"
#include "FileTreeView.h"
#include "MessageBoxUtil.h"
#include "MatchHighlightDelegate.h"
#include "SortLockHeaderView.h"
#include "core/FileFilterProxyModel.h"
#include "core/FileOperationsManager.h"
#include "core/FileSystemModel.h"
#include "core/SearchEngine.h"
#include "core/SearchResultsModel.h"
#include "core/ThumbnailLoader.h"
#include "core/VelaSettings.h"

#include <algorithm>

#include <KConfigGroup>
#include <KFileItemActions>
#include <KMessageWidget>
#include <KFileItemListProperties>
#include <KIO/ApplicationLauncherJob>
#include <KIO/CopyJob>
#include <KIO/Global>
#include <KIO/DeleteOrTrashJob>
#include <KIO/EmptyTrashJob>
#include <KIO/Job>
#include <KIO/RestoreJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KIO/StatJob>
#include <KIO/StoredTransferJob>
#include <KLocalizedString>
#include <KPropertiesDialog>
#include <KService>
#include <KSharedConfig>
#include <QAbstractItemView>
#include <QAction>
#include <QDebug>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QScrollBar>
#include <QSet>
#include <QSharedPointer>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

Panel::Panel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Active indicator bar
    m_activeIndicator = new QFrame(this);
    m_activeIndicator->setFixedHeight(3);
    m_activeIndicator->setFrameStyle(QFrame::NoFrame);
    m_activeIndicator->setAutoFillBackground(true);
    layout->addWidget(m_activeIndicator);

    // Tab bar
    m_tabBar = new QTabBar(this);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setMovable(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setElideMode(Qt::ElideRight);
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabBar->installEventFilter(this);
    {
        // Fixed tab width, sized for 15 characters, so every tab is the same
        // width regardless of folder name — longer names elide with "…".
        const int charsW = QFontMetrics(m_tabBar->font()).horizontalAdvance(QStringLiteral("MMMMMMMMMMMM"));
        const int tabW = charsW + 40; // room for close button + icon + margins
        m_tabBar->setStyleSheet(QStringLiteral("QTabBar::tab { min-width: %1px; max-width: %1px; }").arg(tabW));
    }
    layout->addWidget(m_tabBar);

    // Address bar row: nav buttons + path field
    auto *addressRow = new QWidget(this);
    m_addressRow = addressRow;
    auto *addressLayout = new QHBoxLayout(addressRow);
    addressLayout->setContentsMargins(2, 2, 2, 2);
    addressLayout->setSpacing(2);

    auto makeNavBtn = [&](const QString &iconName, const QString &tip) -> QToolButton * {
        auto *btn = new QToolButton(addressRow);
        btn->setIcon(QIcon::fromTheme(iconName));
        btn->setToolTip(tip);
        btn->setAutoRaise(true);
        btn->setEnabled(false);
        addressLayout->addWidget(btn);
        return btn;
    };
    m_btnBack    = makeNavBtn(QStringLiteral("go-previous"),  i18n("Back"));
    m_btnForward = makeNavBtn(QStringLiteral("go-next"),      i18n("Forward"));
    m_btnUp      = makeNavBtn(QStringLiteral("go-up"),        i18n("Up"));
    m_btnRefresh = makeNavBtn(QStringLiteral("view-refresh"), i18n("Refresh"));

    m_addressBar = new BreadcrumbBar(addressRow);
    addressLayout->addWidget(m_addressBar, 1);

    m_btnEmptyTrash = new QToolButton(addressRow);
    m_btnEmptyTrash->setIcon(QIcon::fromTheme(QStringLiteral("trash-empty")));
    m_btnEmptyTrash->setText(i18n("Empty Trash"));
    m_btnEmptyTrash->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_btnEmptyTrash->setAutoRaise(true);
    m_btnEmptyTrash->setStyleSheet(QStringLiteral(
        "QToolButton { color: red; } QToolButton:hover { color: darkred; }"));
    m_btnEmptyTrash->hide();
    addressLayout->addWidget(m_btnEmptyTrash);

    layout->addWidget(addressRow);

    // Error notification banner (hidden until a directory fails to load)
    m_errorWidget = new KMessageWidget(this);
    m_errorWidget->setMessageType(KMessageWidget::Error);
    m_errorWidget->setCloseButtonVisible(true);
    m_errorWidget->hide();
    layout->addWidget(m_errorWidget);

    // Models. The directory model, its proxy and the views are per tab (see
    // createTabView); search results belong to the panel.
    m_searchResultsModel = new SearchResultsModel(this);
    m_searchProxyModel = new FileFilterProxyModel(this);
    m_searchProxyModel->setSourceModel(m_searchResultsModel);
    // Results stream in as they are found; grouping folders first would make
    // rows jump around under the cursor mid-search.
    m_searchProxyModel->setFoldersFirst(false);

    m_searchEngine = new SearchEngine(this);

    // One thumbnail loader per panel, shared by the directory and search models
    // so a file previewed in one is still cached in the other. The pixmaps
    // themselves live in a process-wide store, so panels also share each
    // other's results.
    m_thumbLoader = new ThumbnailLoader(this);
    m_searchResultsModel->setThumbnailLoader(m_thumbLoader);

    // One thumbnail size for every icon mode, deliberately, and set once here
    // rather than on every pass — changing it invalidates the whole store.
    //
    // Previously each mode asked for a different size (128 / 256 / 512), so
    // simply switching Medium → Large discarded every thumbnail already
    // generated and started over — which is why the larger views looked like
    // they never loaded. With a single size the cache survives, and thumbnails
    // generated in Medium appear immediately in Large and Extra Large too.
    //
    // 256 rather than 128: it is a freedesktop cache size ("large", the best
    // populated one on a typical KDE system), it maps exactly onto the Extra
    // Large icon size so nothing is upscaled, and it downscales cleanly for the
    // smaller modes.
    m_thumbLoader->setThumbnailSize(256);
    m_thumbLoader->setMaxFileSize(qint64(VelaSettings::self()->thumbnailMaxSizeMiB())
                                  * 1024 * 1024);

    // Highlight delegate (applied to Name column in both views)
    m_delegate = new MatchHighlightDelegate(this);

    // One page per loaded tab (see TabView): each holds that tab's own details
    // and icon views over its own model.
    m_tabStack = new QStackedWidget(this);
    layout->addWidget(m_tabStack, 1);

    // Empty-directory overlay
    m_emptyDirLabel = new QLabel(i18n("This folder is empty"), m_tabStack);
    m_emptyDirLabel->setAlignment(Qt::AlignCenter);
    m_emptyDirLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyDirLabel->setStyleSheet(QStringLiteral("color: palette(placeholderText); font-size: 13px;"));
    m_emptyDirLabel->hide();
    m_tabStack->installEventFilter(this);

    // Filter bar (hidden by default, appears between stack and status bar)
    m_filterBar = new FilterBar(this);
    m_filterBar->hide();
    layout->addWidget(m_filterBar);

    // Status bar — hidden by default; repositionStatusBars() shows it only for
    // non-bottom panels in a vertical split.
    m_statusBar = new QLabel(this);
    m_statusBar->setContentsMargins(4, 2, 4, 2);
    m_statusBar->hide();
    layout->addWidget(m_statusBar);

    setActive(false);

    // ── Connections ──────────────────────────────────────────────────────────

    connect(QGuiApplication::clipboard(), &QClipboard::changed, this, [this](QClipboard::Mode mode) {
        if (mode == QClipboard::Clipboard)
            syncCutFromClipboard();
    });
    syncCutFromClipboard();

    connect(m_addressBar, &BreadcrumbBar::urlActivated, this, &Panel::navigateTo);
    connect(m_addressBar, &BreadcrumbBar::urlActivatedInNewTab, this, &Panel::newTab);

    connect(m_btnBack,    &QToolButton::clicked, this, &Panel::goBack);
    connect(m_btnForward, &QToolButton::clicked, this, &Panel::goForward);
    connect(m_btnUp,      &QToolButton::clicked, this, &Panel::goUp);
    connect(m_btnRefresh, &QToolButton::clicked, this, &Panel::goRefresh);
    connect(m_btnEmptyTrash, &QToolButton::clicked, this, [this]() {
        // DeleteOrTrashJob asks the standard KDE "Empty Trash?" question
        // (honouring the user's kiorc confirmation setting) and reports errors
        // through its widget handler — KIO::emptyTrash() alone did neither.
        auto *job = new KIO::DeleteOrTrashJob({}, KIO::AskUserActionInterface::EmptyTrash,
                                              KIO::AskUserActionInterface::DefaultConfirmation,
                                              window());
        connect(job, &KJob::result, this, [this](KJob *j) {
            if (!j->error())
                m_model->immediateRefresh();
        });
        job->start();
    });
    auto updateNavBtns = [this]() {
        m_btnBack->setEnabled(canGoBack());
        m_btnForward->setEnabled(canGoForward());
        m_btnUp->setEnabled(currentUrl().isValid());
        m_btnRefresh->setEnabled(currentUrl().isValid());
    };
    connect(this, &Panel::navigationStateChanged, this, updateNavBtns);
    connect(this, &Panel::urlChanged, this, [this, updateNavBtns](const QUrl &url) {
        updateNavBtns();
        m_btnEmptyTrash->setVisible(url.scheme() == QLatin1String("trash"));
    });
    // Selection changes only move the status line, which nobody can read
    // mid-drag anyway. Restarting on each change means a rubber-band sweep or a
    // held Shift+Down sums once, when it settles.
    m_statusDebounce = new QTimer(this);
    m_statusDebounce->setSingleShot(true);
    m_statusDebounce->setInterval(100);
    connect(m_statusDebounce, &QTimer::timeout, this, &Panel::refreshStatusText);

    // Advances the loading glyph and, with it, republishes the running item
    // count — so the status line ticks up as the listing streams in instead of
    // sitting on the previous directory's text until the load completes.
    m_loadSpinner = new QTimer(this);
    m_loadSpinner->setInterval(120);
    connect(m_loadSpinner, &QTimer::timeout, this, [this]() {
        ++m_spinnerFrame;
        refreshStatusText();
    });

    // Dragging a divider emits sectionResized for every pixel of the drag, and
    // the destination is a config file — so coalesce, and write only what the
    // user did, never the programmatic resizes that restore or hide a column.
    m_columnWidthSave = new QTimer(this);
    m_columnWidthSave->setSingleShot(true);
    m_columnWidthSave->setInterval(500);
    connect(m_columnWidthSave, &QTimer::timeout, this, &Panel::saveColumnWidthsGlobally);
    connect(m_tabBar, &QTabBar::currentChanged, this, &Panel::onTabChanged);
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &Panel::onTabCloseRequested);
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        m_tabs.move(from, to);
        m_currentTab = m_tabBar->currentIndex();
    });
    connect(m_tabBar, &QTabBar::customContextMenuRequested,
            this, &Panel::onTabContextMenu);

    // Rename tab-navigation
    connect(m_delegate, &MatchHighlightDelegate::tabToNext,
            this, [this]() { startEditingAdjacentItem(+1); });
    connect(m_delegate, &MatchHighlightDelegate::tabToPrev,
            this, [this]() { startEditingAdjacentItem(-1); });

    // Filter bar
    connect(m_filterBar, &FilterBar::filterChanged, this, &Panel::onFilterChanged);
    connect(m_filterBar, &FilterBar::dismissed, this, &Panel::onFilterDismissed);
    connect(m_filterBar, &FilterBar::subdirSearchToggled, this, &Panel::onFilterSubdirToggled);
    connect(m_filterBar, &FilterBar::keyForView, this, [this](QKeyEvent *ke) {
        // Handled exactly as if the view had focus (Shift extends, Enter goes
        // through the view's own Enter handling), but focus stays in the field.
        QKeyEvent copy(ke->type(), ke->key(), ke->modifiers(), ke->text(),
                       ke->isAutoRepeat(), ke->count());
        QApplication::sendEvent(currentView(), &copy);
    });
    connect(m_filterBar, &FilterBar::crossFilesystemsToggled, this, [this]() {
        // Re-run the current subfolder search with the new scope.
        if (m_filterBar->subdirSearch() && !m_filterBar->lineEdit()->text().isEmpty())
            m_filterDebounce->start();
    });

    // ── Thumbnail scheduling ─────────────────────────────────────────────────
    // Debounced so that flicking the scrollbar across a directory doesn't queue
    // a batch for every intermediate position — only where the view settles.
    m_thumbDebounce = new QTimer(this);
    m_thumbDebounce->setSingleShot(true);
    m_thumbDebounce->setInterval(120);
    connect(m_thumbDebounce, &QTimer::timeout, this, &Panel::updateVisibleThumbnails);

    // Only the list view: thumbnails exist only in the icon modes, so wiring the
    // tree's scrollbar here just meant signal traffic on every Details scroll
    // for a slot that always returned immediately.
    // (The list views' scroll bars and each tab's proxy are wired per tab, in
    // createTabView.)
    connect(m_searchProxyModel, &QAbstractItemModel::modelReset,
            this, &Panel::scheduleThumbnailUpdate);
    connect(m_searchProxyModel, &QAbstractItemModel::rowsInserted,
            this, &Panel::scheduleThumbnailUpdate);

    m_pendingFallback = new QTimer(this);
    m_pendingFallback->setSingleShot(true);
    m_pendingFallback->setInterval(1500);
    connect(m_pendingFallback, &QTimer::timeout, this, [this]() {
        // Its completed() runs onDirectoryLoaded → applyPendingSelection,
        // which selects whichever targets exist.
        if (m_pendingAction != PendingAction::None && !m_inSearchMode)
            m_model->immediateRefresh();
    });

    m_filterDebounce = new QTimer(this);
    m_filterDebounce->setSingleShot(true);
    m_filterDebounce->setInterval(300);
    connect(m_filterDebounce, &QTimer::timeout, this, [this]() {
        const QString pattern = m_filterBar->lineEdit()->text();
        if (pattern.isEmpty()) {
            m_filterBar->setHint(QString());
            exitSearchMode();
            return;
        }
        // The walk is unbounded in depth and matches fuzzily, so a one- or
        // two-letter term ("ab" in ~) traverses the whole tree and matches
        // most of it. Wait for a term selective enough to be worth it.
        static constexpr int kMinRecursiveTermLength = 3;
        if (pattern.size() < kMinRecursiveTermLength) {
            exitSearchMode();
            m_filterBar->setHint(i18np("Type at least %1 character to search subfolders",
                                       "Type at least %1 characters to search subfolders",
                                       kMinRecursiveTermLength));
            return;
        }
        m_filterBar->setHint(QString());
        startSubfolderSearch(pattern);
    });

    // Search engine (queued automatically since signal emitted from background thread)
    connect(m_searchEngine, &SearchEngine::resultsBatch, this, &Panel::onSearchResultsBatch);
    connect(m_searchEngine, &SearchEngine::limitReached, this, [this]() {
        m_filterBar->setHint(i18n("Showing the first %1 results — refine the search term",
                                  SearchEngine::kMaxResults));
    });
    // A tab's search re-run on switching back: its selection and scroll apply
    // once the results are all in.
    connect(m_searchEngine, &SearchEngine::finished, this, [this]() {
        if (m_pendingSearchPosition && m_inSearchMode)
            applyViewPosition(std::exchange(m_pendingSearchPosition, std::nullopt));
    });

    setupFileOpShortcuts();

    // Never without a current view: an unassigned one until the first tab
    // claims it (restoreSettings seeds its header before that happens).
    bindTabView(createTabView(0));
}

Panel *Panel::createEmbedded(std::function<Panel *()> tabHost, QWidget *parent)
{
    auto *p = new Panel(parent);
    p->m_embedded = true;
    p->m_tabHost  = std::move(tabHost);
    p->m_activeIndicator->hide();
    p->m_tabBar->hide();
    p->m_addressRow->hide();
    p->m_statusBar->hide();
    // Frameless like the preview's snapshot it replaces, so the swap does
    // not shift the rows by the frame's width.
    p->m_treeView->setFrameShape(QFrame::NoFrame);
    p->m_listView->setFrameShape(QFrame::NoFrame);
    // What a window does for a new panel, which this one never becomes.
    p->applyAppearanceSettings();
    p->applyColumnSettings();
    return p;
}

void Panel::startEmbedded(const QUrl &url, const QUrl &reveal)
{
    if (!m_embedded || !m_tabs.isEmpty())
        return;
    const int mode = qBound(0, VelaSettings::self()->defaultViewMode(),
                            static_cast<int>(ExtraLargeIcons));
    TabContext ctx(url, mode);
    if (reveal.isValid())
        ctx.reveal = {reveal};
    insertTab(ctx);
}

// What is dimmed as "cut" is whatever the clipboard holds as a cut — in every
// panel, whichever one did the cutting. Keeping a per-panel set meant a second
// cut elsewhere left the first panel dimming items no longer on the clipboard.
void Panel::syncCutFromClipboard()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (mime && mime->data(QStringLiteral("x-kde-cut-selection")) == QByteArray("1")) {
        const QList<QUrl> urls = mime->urls();
        m_delegate->setCutUrls(QSet<QUrl>(urls.cbegin(), urls.cend()));
    } else {
        m_delegate->clearCutUrls();
    }
    if (QAbstractItemView *view = currentView())
        view->viewport()->update();
}

// ── Per-tab views ─────────────────────────────────────────────────────────────
//
// Like Dolphin, every tab has its own model and views, so going back to a tab
// shows it as it was — rows, selection, scroll — instead of re-listing and
// re-sorting the folder through one shared model. At most kMaxLoadedTabs exist
// at once: the current tab's and those of the most recently left ones. A tab
// that falls out of that set loses its views and is rebuilt (from its history
// entry's stored position) when it is shown again.
//
// A tab that is not shown is frozen: no directory watch, no auto-update, no
// folder-size jobs. Coming back thaws it and re-lists the differences.
//
// m_model, m_proxyModel, m_treeView, m_listView and m_stack always point at
// the current tab's (bindTabView). Every per-tab connection ignores signals
// from a tab that is not the current one.

class Panel::TabView : public QObject
{
public:
    using QObject::QObject;

    quint64 tabId = 0; // 0: not (yet) a tab's — the panel's initial view
    FileSystemModel      *model = nullptr;
    FileFilterProxyModel *proxy = nullptr;
    FileTreeView         *tree  = nullptr;
    QListView            *list  = nullptr;
    QStackedWidget       *stack = nullptr; // [tree, list]; a page of m_tabStack
    QString               error; // the banner text of a failed listing
    QMetaObject::Connection singleClickTree;
    QMetaObject::Connection singleClickList;
    QSet<int> appliedHiddenColumns;
    bool      appliedHiddenColumnsValid = false;
};

Panel::TabView *Panel::createTabView(quint64 tabId)
{
    auto *stack = new QStackedWidget(m_tabStack);
    auto *tv = new TabView(stack);
    tv->tabId = tabId;
    tv->stack = stack;
    const auto current = [this, tv]() { return tv == m_cur; };

    tv->model = new FileSystemModel(stack);
    tv->model->setThumbnailLoader(m_thumbLoader);
    tv->model->setShowHidden(m_showHidden);
    tv->model->setFoldersFirst(m_foldersFirst);

    tv->proxy = new FileFilterProxyModel(stack);
    tv->proxy->setSourceModel(tv->model);
    tv->proxy->setFoldersFirst(m_foldersFirst);

    // Details view
    tv->tree = new FileTreeView(stack);
    FileTreeView *tree = tv->tree;
    auto *lockHeader = new SortLockHeaderView(Qt::Horizontal, tree);
    tree->setHeader(lockHeader);
    tree->setModel(tv->proxy);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSortingEnabled(true);
    tree->sortByColumn(FileSystemModel::Name, Qt::AscendingOrder);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setAlternatingRowColors(VelaSettings::self()->alternatingRowColors());
    tree->setIconSize(QSize(16, 16));
    // Qt's own stretch is off: it runs the last section all the way to the right
    // edge, leaving no space that belongs to no column. stretchLastColumn() does
    // the same job but stops kTrailingDeadZone short, so there is always a strip
    // where QTreeView::indexAt() returns an invalid index — which is what turns a
    // right-click there into the directory's background menu.
    lockHeader->setStretchLastSection(false);
    lockHeader->setSectionResizeMode(QHeaderView::Interactive);
    lockHeader->setSectionsMovable(true);
    lockHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->setDragEnabled(true);
    tree->setAcceptDrops(true);
    tree->setDropIndicatorShown(true);
    tree->setDragDropMode(QAbstractItemView::DragDrop);
    tree->setDefaultDropAction(Qt::MoveAction);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setItemDelegateForColumn(FileSystemModel::Name, m_delegate);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->installEventFilter(this);
    tree->viewport()->installEventFilter(this);

    // List / icon view
    tv->list = new QListView(stack);
    QListView *list = tv->list;
    list->setModel(tv->proxy);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setSelectionRectVisible(true); // Qt's band is there; this just draws it
    list->setUniformItemSizes(true);
    list->setWordWrap(true);
    list->setDragEnabled(true);
    list->setAcceptDrops(true);
    list->setDropIndicatorShown(true);
    list->setDragDropMode(QAbstractItemView::DragDrop);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setItemDelegate(m_delegate);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->installEventFilter(this);
    list->viewport()->installEventFilter(this);

    if (m_embedded) {
        // Frameless like the preview's snapshot it replaces, so the swap does
        // not shift the rows by the frame's width.
        tree->setFrameShape(QFrame::NoFrame);
        list->setFrameShape(QFrame::NoFrame);
    }

    stack->addWidget(tree);
    stack->addWidget(list);
    m_tabStack->addWidget(stack);

    // Columns as the panel shows them now: one layout per panel, not per tab.
    if (m_cur)
        copyColumnLayout(m_cur, tv);

    // ── Header ───────────────────────────────────────────────────────────────
    // When switching to a new column, override Qt's default Ascending order:
    // date and size columns should default to Descending (latest/largest first).
    // Skip the override when we are restoring the locked sort for a directory.
    connect(lockHeader, &QHeaderView::sortIndicatorChanged, tv,
            [this, current, tree](int col, Qt::SortOrder order) {
        if (!current() || m_applyingLockedSort)
            return;
        if (col == m_lastSortCol || order != Qt::AscendingOrder)
            return; // same column toggled, or already descending — nothing to override
        m_lastSortCol = col;
        const bool wantDescending =
            col == FileSystemModel::DateModified ||
            col == FileSystemModel::DateCreated  ||
            col == FileSystemModel::DateAccessed ||
            col == FileSystemModel::Size;
        if (wantDescending)
            tree->sortByColumn(col, Qt::DescendingOrder);
    });
    connect(lockHeader, &SortLockHeaderView::lockClicked, tv, [this, current](int section) {
        if (current())
            onLockClicked(section);
    });
    // Dragging a divider emits sectionResized for every pixel of the drag, and
    // the destination is a config file — so coalesce, and write only what the
    // user did, never the programmatic resizes that restore or hide a column.
    connect(lockHeader, &QHeaderView::sectionResized, tv, [this, current](int, int, int) {
        if (!current() || m_applyingColumnWidths)
            return; // another tab's, or our own stretch — not a drag
        m_columnWidthSave->start();
        // Widening one column eats into the space the last one may occupy.
        stretchLastColumn();
    });
    connect(lockHeader, &QHeaderView::customContextMenuRequested, tv,
            [this, current](const QPoint &pos) {
        if (current())
            onHeaderContextMenu(pos);
    });

    // ── Views ────────────────────────────────────────────────────────────────
    connect(tree, &QTreeView::activated, this, &Panel::onItemActivated);
    connect(list, &QListView::activated, this, &Panel::onItemActivated);
    connect(tree, &QTreeView::customContextMenuRequested, tv, [this, tree](const QPoint &pos) {
        showContextMenu(tree, pos);
    });
    connect(list, &QListView::customContextMenuRequested, tv, [this, list](const QPoint &pos) {
        showContextMenu(list, pos);
    });
    connect(tree->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Panel::onSelectionChanged);
    connect(list->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Panel::onSelectionChanged);
    applyViewSettings(tv);

    // Thumbnails: only the list view — thumbnails exist only in the icon modes.
    // Batched layout positions items over several event-loop passes with no
    // "layout finished" signal; the growing scroll range doubles as one.
    const auto thumbs = [this, current]() {
        if (current())
            scheduleThumbnailUpdate();
    };
    connect(list->verticalScrollBar(), &QAbstractSlider::valueChanged, tv, thumbs);
    connect(list->verticalScrollBar(), &QAbstractSlider::rangeChanged, tv, thumbs);
    // Rows arriving (directory load, live update) can land in the visible range.
    connect(tv->proxy, &QAbstractItemModel::modelReset, tv, [this, current]() {
        if (!current())
            return;
        applyColumnSettings();
        scheduleThumbnailUpdate();
    });
    connect(tv->proxy, &QAbstractItemModel::rowsInserted, tv, thumbs);

    // ── Model ────────────────────────────────────────────────────────────────
    FileSystemModel *model = tv->model;
    connect(model, &FileSystemModel::directoryLoaded, tv, [this, current](const QUrl &url) {
        if (current())
            onDirectoryLoaded(url);
    });
    connect(model, &FileSystemModel::directoryRefreshed, tv, [this, current](const QUrl &) {
        // Incremental live update: refresh counts and the empty-folder overlay only.
        // Selection, scroll position, and unchanged rows are deliberately left alone.
        if (!current() || m_inSearchMode)
            return;
        updateStatusBar();
        // A create/paste may be waiting for its items to arrive.
        if (m_pendingFallback->isActive())
            resolvePendingIfListed();
        updateEmptyLabel();
    });
    connect(model, &FileSystemModel::loadingProgress, tv, [this, current](int rows) {
        if (current())
            onLoadingProgress(rows);
    });
    connect(model, &FileSystemModel::loadingStarted, tv, [this, current, tv](const QUrl &url) {
        tv->error.clear();
        if (!current())
            return;
        // The rows were just cleared and none have arrived: sorting now is
        // free, and the listing streams in already in its final order. Sorting
        // after the load re-sorted every row and made them jump (Downloads
        // re-ordering by date once full).
        if (m_sortPendingFor.isValid() && m_sortPendingFor == url) {
            m_sortPendingFor.clear();
            applySortPreference(url);
        }
        // A fresh navigation: drop any indicator left over from the last one.
        // Deliberately no indicator yet — a small directory finishes before the
        // threshold is reached and its status line never changes appearance.
        stopLoadingIndicator();
        if (m_errorWidget->isVisible())
            m_errorWidget->animatedHide();
        m_emptyDirLabel->hide();
    });
    connect(model, &FileSystemModel::directoryError, tv,
            [this, current, tv](const QString &message) {
        tv->error = message; // shown again when the tab is
        if (!current())
            return;
        stopLoadingIndicator();
        // A directory that failed or vanished has no rows, but that is "unknown",
        // not "empty" — the two overlays must never be shown together.
        m_emptyDirLabel->hide();
        m_errorWidget->setText(message);
        m_errorWidget->animatedShow();
    });
    connect(model, &FileSystemModel::renamedTo, tv, [this, current](const QUrl &newUrl) {
        if (!current())
            return;
        // Re-select the renamed item. The model row already has the new URL
        // (in-place update), so it resolves directly through the URL index.
        const int row = m_model->rowForUrl(newUrl);
        if (row < 0)
            return;
        const QModelIndex proxy = m_proxyModel->mapFromSource(m_model->index(row, 0));
        if (!proxy.isValid())
            return;
        currentView()->selectionModel()->select(
            proxy, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        currentView()->setCurrentIndex(proxy);
        currentView()->scrollTo(proxy);
    });

    m_views.append(tv);
    return tv;
}

void Panel::bindTabView(TabView *tv)
{
    m_cur        = tv;
    m_model      = tv->model;
    m_proxyModel = tv->proxy;
    m_treeView   = tv->tree;
    m_listView   = tv->list;
    m_stack      = tv->stack;
    m_tabStack->setCurrentWidget(tv->stack);
    // Each tab's header keeps its own sort; the descending-by-default rule for
    // dates and sizes compares against the one shown.
    m_lastSortCol = tv->tree->header()->sortIndicatorSection();
}

// The tab's existing views, the panel's still unclaimed initial one, or new.
Panel::TabView *Panel::viewForTab(quint64 tabId)
{
    for (TabView *tv : std::as_const(m_views)) {
        if (tv->tabId == tabId)
            return tv;
    }
    for (TabView *tv : std::as_const(m_views)) {
        if (tv->tabId == 0) {
            tv->tabId = tabId;
            return tv;
        }
    }
    return createTabView(tabId);
}

void Panel::destroyTabView(TabView *tv)
{
    if (!tv || tv == m_cur)
        return;
    m_views.removeOne(tv);
    tv->model->setFrozen(true); // stop listing, watching and size jobs now
    m_tabStack->removeWidget(tv->stack);
    tv->stack->hide();
    // Later: this can run from inside a signal of something the views hold.
    // Until then every connection's current() check fails, so it stays inert.
    tv->stack->deleteLater();
}

void Panel::destroyViewForTab(quint64 tabId)
{
    for (TabView *tv : std::as_const(m_views)) {
        if (tv->tabId == tabId) {
            destroyTabView(tv);
            return;
        }
    }
}

// Visibility, widths and order of the columns. Not the sort: that is the
// folder's own (its locked/default preference, or what the user picked there).
void Panel::copyColumnLayout(TabView *from, TabView *to)
{
    if (!from || !to || from == to)
        return;
    QHeaderView *src = from->tree->header();
    QHeaderView *dst = to->tree->header();
    const bool wasApplying = m_applyingColumnWidths;
    m_applyingColumnWidths = true;
    for (int visual = 0; visual < src->count(); ++visual) {
        const int logical = src->logicalIndex(visual);
        const int at = dst->visualIndex(logical);
        if (at != visual)
            dst->moveSection(at, visual);
    }
    for (int i = 0; i < src->count(); ++i) {
        dst->setSectionHidden(i, src->isSectionHidden(i));
        if (!src->isSectionHidden(i))
            dst->resizeSection(i, src->sectionSize(i));
    }
    m_applyingColumnWidths = wasApplying;
    to->appliedHiddenColumns      = from->appliedHiddenColumns;
    to->appliedHiddenColumnsValid = from->appliedHiddenColumnsValid;
}

// Per-view settings from VelaSettings: row colours and single-click opening.
void Panel::applyViewSettings(TabView *tv)
{
    VelaSettings *s = VelaSettings::self();
    tv->tree->setAlternatingRowColors(s->alternatingRowColors());
    disconnect(tv->singleClickTree);
    disconnect(tv->singleClickList);
    if (s->singleClick()) {
        tv->singleClickTree = connect(tv->tree, &QTreeView::clicked, this, &Panel::onItemActivated);
        tv->singleClickList = connect(tv->list, &QListView::clicked, this, &Panel::onItemActivated);
    }
}

void Panel::updateEmptyLabel()
{
    const bool empty = !m_inSearchMode && m_model->rowCount() == 0
                       && !m_model->hasListingError() && !m_model->needsReload();
    if (empty) {
        m_emptyDirLabel->setGeometry(m_tabStack->rect());
        m_emptyDirLabel->raise();
    }
    m_emptyDirLabel->setVisible(empty);
}

// Showing a pooled tab whose rows are a complete listing of its folder: what
// onDirectoryLoaded does for the UI, without a load. Its views kept their rows,
// selection and scroll; a light re-list then brings in what changed while it
// was frozen.
void Panel::resumeTabView()
{
    stopLoadingIndicator();
    if (m_model->showHidden() != m_showHidden)
        m_model->setShowHidden(m_showHidden);
    m_model->setFrozen(false);
    m_model->relistLight();
    m_model->revalidateDirectory();
    if (m_folderSizeCalcEnabled)
        m_model->requestAllFolderSizes();

    if (m_errorWidget->isVisible())
        m_errorWidget->animatedHide();
    updateAddressBar();
    if (m_currentTab >= 0 && m_currentTab < m_tabs.size())
        updateTabTitle(m_currentTab);
    applyColumnSettings();
    stretchLastColumn();
    Q_EMIT urlChanged(currentUrl());

    if (m_pendingAction != PendingAction::SelectAndEdit
        && (!m_embedded || isAncestorOf(QApplication::focusWidget())))
        currentView()->setFocus();
    applyPendingSelection();
    applyPendingRestore();
    updateStatusBar();
    updateEmptyLabel();
    scheduleThumbnailUpdate();
}

// ── Navigation ────────────────────────────────────────────────────────────────

void Panel::navigateTo(const QUrl &url)
{
    navigate(url, /*takeFocus=*/true);
}

void Panel::navigateToWithoutFocus(const QUrl &url)
{
    navigate(url, /*takeFocus=*/false);
}

void Panel::navigate(const QUrl &url, bool takeFocus)
{
    if (m_currentTab < 0)
        return;
    const QUrl from = currentUrl();
    rememberPositionBeforeLeaving();
    clearFilterOnNavigation();
    currentTab().history.push(url);
    // Before loadUrl(): a cached listing can complete inside it.
    armPositionRestore(from);
    loadUrl(url, takeFocus);
}

void Panel::navigateAndReveal(const QUrl &folder, const QUrl &fileToReveal)
{
    setPendingReveal(fileToReveal);
    navigateTo(folder);
}

void Panel::setPendingReveal(const QUrl &fileToReveal)
{
    if (fileToReveal.isValid())
        setPendingReveal(QList<QUrl>{fileToReveal});
}

void Panel::setPendingReveal(const QList<QUrl> &filesToReveal)
{
    if (filesToReveal.isEmpty())
        return;
    m_pendingUrls   = filesToReveal;
    m_pendingAction = PendingAction::Select;
}

void Panel::goBack()
{
    if (m_currentTab < 0 || !currentTab().history.canGoBack())
        return;
    const QUrl from = currentUrl();
    rememberPositionBeforeLeaving();
    clearFilterOnNavigation();
    const QUrl url = currentTab().history.back();
    armPositionRestore(from);
    loadUrl(url);
    Q_EMIT navigationStateChanged();
}

void Panel::goForward()
{
    if (m_currentTab < 0 || !currentTab().history.canGoForward())
        return;
    const QUrl from = currentUrl();
    rememberPositionBeforeLeaving();
    clearFilterOnNavigation();
    const QUrl url = currentTab().history.forward();
    armPositionRestore(from);
    loadUrl(url);
    Q_EMIT navigationStateChanged();
}

void Panel::goUp()
{
    const QUrl cur = currentUrl();
    if (!cur.isValid())
        return;
    // Strip the trailing slash *first*: on "/home/user/" RemoveFilename is a
    // no-op (the filename is already empty), so applying it first meant Alt+Up
    // silently "navigated" to the same directory before it could move anywhere.
    const QUrl parent =
        cur.adjusted(QUrl::StripTrailingSlash).adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
    if (parent == cur.adjusted(QUrl::StripTrailingSlash) || parent.path().isEmpty())
        return;
    navigateTo(parent);
}

void Panel::goHome()    { navigateTo(QUrl::fromLocalFile(QDir::homePath())); }
void Panel::goRefresh()
{
    if (m_currentTab < 0 || !currentUrl().isValid())
        return;
    clearFilterOnNavigation();
    // A failed or vanished directory has nothing listed to update; open it
    // afresh exactly like a navigation would.
    if (m_model->hasListingError()) {
        loadUrl(currentUrl());
        return;
    }
    // Not navigateTo(): its openUrl() lets KCoreDirLister answer a watched
    // directory from its cache, so where inotify can't see changes (NFS, sshfs,
    // FUSE) F5 showed the stale listing. updateDirectory re-lists from the
    // source and applies only the differences, keeping selection and scroll.
    m_model->immediateRefresh();
    m_model->revalidateDirectory();
}

bool Panel::canGoBack() const
{
    return m_currentTab >= 0 && m_tabs.at(m_currentTab).history.canGoBack();
}

bool Panel::canGoForward() const
{
    return m_currentTab >= 0 && m_tabs.at(m_currentTab).history.canGoForward();
}

QUrl Panel::currentUrl() const
{
    if (m_currentTab < 0 || m_currentTab >= m_tabs.size())
        return {};
    return m_tabs.at(m_currentTab).history.current();
}

// ── Tabs ──────────────────────────────────────────────────────────────────────

void Panel::newTab(const QUrl &url)
{
    if (m_embedded) {
        // An embedded panel has one hidden tab; a new one belongs to the panel
        // the preview follows. A reveal armed for it (Open Containing Folder
        // in New Tab) goes along.
        Panel *host = m_tabHost ? m_tabHost() : nullptr;
        if (!host)
            return;
        if (m_pendingAction == PendingAction::Select && !m_pendingUrls.isEmpty()) {
            host->setPendingReveal(m_pendingUrls);
            m_pendingUrls.clear();
            m_pendingAction = PendingAction::None;
        }
        host->newTab(url);
        return;
    }
    // A new tab starts in the configured default view mode, not whatever the
    // current tab happens to be showing.
    const int mode = qBound(0, VelaSettings::self()->defaultViewMode(),
                            static_cast<int>(ExtraLargeIcons));
    TabContext ctx(url, mode);
    const int insertAt = m_currentTab + 1;
    m_tabs.insert(insertAt, ctx);

    const QString title = tabTitleForUrl(url);
    m_tabBar->blockSignals(true);
    m_tabBar->insertTab(insertAt, title);
    m_tabBar->blockSignals(false);

    switchToTab(insertAt);
    Q_EMIT tabCountChanged();
}

void Panel::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs.size() <= 1) {
        Q_EMIT closeRequested();
        return;
    }

    const quint64 closedId = m_tabs.at(index).id;
    m_recentTabIds.removeAll(closedId);
    m_tabs.removeAt(index);
    m_tabBar->blockSignals(true);
    m_tabBar->removeTab(index);
    m_tabBar->blockSignals(false);

    if (index != m_currentTab) {
        // A background tab: the visible one stays put and is not reloaded.
        // Only its index shifts when a tab before it went away.
        destroyViewForTab(closedId);
        if (index < m_currentTab)
            --m_currentTab;
        m_tabBar->blockSignals(true);
        m_tabBar->setCurrentIndex(m_currentTab);
        m_tabBar->blockSignals(false);
        Q_EMIT tabCountChanged();
        return;
    }

    const int newIndex = qBound(0, index > 0 ? index - 1 : 0, m_tabs.size() - 1);
    m_currentTab = -1; // nothing to save for the closed tab
    switchToTab(newIndex);
    destroyViewForTab(closedId); // no longer current
    Q_EMIT tabCountChanged();
}

void Panel::closeOtherTabs(int keepIndex)
{
    if (keepIndex < 0 || keepIndex >= m_tabs.size() || m_tabs.size() <= 1)
        return;

    // Remove everything at once and load at most one directory — closing them
    // one by one reloaded the directory each time the active tab went away.
    const bool keepIsCurrent = (keepIndex == m_currentTab);
    if (m_currentTab >= 0 && m_currentTab < m_tabs.size())
        m_tabs[m_currentTab].viewMode = static_cast<int>(m_viewMode);

    const TabContext kept = m_tabs.at(keepIndex);
    QList<quint64> closedIds;
    for (const TabContext &t : std::as_const(m_tabs)) {
        if (t.id != kept.id)
            closedIds.append(t.id);
    }
    m_tabs = {kept};
    m_recentTabIds.clear();
    m_tabBar->blockSignals(true);
    for (int i = m_tabBar->count() - 1; i >= 0; --i) {
        if (i != keepIndex)
            m_tabBar->removeTab(i);
    }
    m_tabBar->setCurrentIndex(0);
    m_tabBar->blockSignals(false);

    if (keepIsCurrent) {
        m_currentTab = 0;
    } else {
        m_currentTab = -1; // the closed current tab has nothing to save
        switchToTab(0);
    }
    for (quint64 id : std::as_const(closedIds))
        destroyViewForTab(id);
    Q_EMIT tabCountChanged();
}

// ── Per-tab view state ────────────────────────────────────────────────────────

void Panel::saveTabState(int index)
{
    TabContext &tab = m_tabs[index];
    tab.filterVisible = m_filterBar->isVisible();
    tab.filterText    = m_filterBar->lineEdit()->text();
    tab.filterSubdirs = m_filterBar->subdirSearch();
    tab.filterCrossFs = m_filterBar->crossFilesystems();
    tab.searchTerm.clear();
    if (m_inSearchMode) {
        tab.searchTerm = m_searchResultsModel->searchTerm();
    } else if (tab.filterVisible && tab.filterSubdirs && m_filterDebounce->isActive()) {
        tab.searchTerm = tab.filterText; // typed, but the search had not started yet
    }

    tab.searchState.reset();
    if (m_inSearchMode) {
        // What is on screen belongs to the results, not to the directory.
        tab.history.setCurrentPosition(std::nullopt);
        TabSearchState st;
        st.position = captureViewPosition(/*fullSelection=*/true);
        // Only finished results are worth keeping; a partial list would look
        // complete. An unfinished search is re-run on the way back.
        if (!m_searchEngine->isRunning()) {
            st.cached  = true;
            st.results = m_searchResultsModel->items();
        }
        tab.searchState = std::move(st);
    } else {
        tab.history.setCurrentPosition(captureViewPosition(/*fullSelection=*/true));
    }
    rememberViewState(tab.id);
}

void Panel::rememberViewState(quint64 id)
{
    m_recentTabIds.removeAll(id);
    m_recentTabIds.prepend(id);
    // The current tab is loaded too, so the recently left ones get one place
    // less: kMaxLoadedTabs in all.
    while (m_recentTabIds.size() > kMaxLoadedTabs - 1) {
        const quint64 evicted = m_recentTabIds.takeLast();
        destroyViewForTab(evicted);
        for (TabContext &t : m_tabs) {
            if (t.id == evicted) {
                t.history.trimCurrentPosition(); // down to what any entry keeps
                t.searchState.reset();           // filter text and search term stay
                break;
            }
        }
    }
}

void Panel::resetFilterQuietly()
{
    m_filterDebounce->stop();
    m_searchEngine->cancel();
    if (m_inSearchMode)
        exitSearchMode();
    {
        // Blocked so clearing the field is not taken for the user typing.
        const QSignalBlocker block(m_filterBar);
        m_filterBar->clearFilter();
        m_filterBar->setSubdirSearch(false);
        m_filterBar->setCrossFilesystems(false);
        m_filterBar->setHint(QString());
        m_filterBar->hide();
    }
    m_proxyModel->setFilterPattern(QString());
    m_delegate->clearHighlight();
}

// Every navigation — including F5 and re-entering the same folder — starts
// unfiltered: a pattern typed for one folder silently hid entries in the next,
// and a subdir search term stayed in the bar with nothing searched for it.
// Tab switches are not navigations and keep their tab's filter.
void Panel::clearFilterOnNavigation()
{
    const bool hadFocus = m_filterBar->isAncestorOf(QApplication::focusWidget());
    resetFilterQuietly();
    if (hadFocus)
        currentView()->setFocus();
}

void Panel::applyPendingRestore()
{
    if (!m_pendingRestore)
        return;
    PendingRestore r = std::move(*m_pendingRestore);
    m_pendingRestore.reset();
    if (currentUrl() != r.url)
        return; // navigated elsewhere before this load completed

    if (r.filterVisible) {
        const QSignalBlocker block(m_filterBar);
        m_filterBar->show();
        m_filterBar->setSubdirSearch(r.filterSubdirs);
        m_filterBar->setCrossFilesystems(r.filterCrossFs);
        m_filterBar->lineEdit()->setText(r.filterText);
    }

    if (!r.searchTerm.isEmpty()) {
        if (r.search && r.search->cached) {
            showCachedSearch(r.searchTerm, r.search->results);
            applyViewPosition(r.search->position);
        } else {
            // Not among the recently left tabs, or left mid-search: run it again.
            startSubfolderSearch(r.searchTerm);
            if (r.search)
                m_pendingSearchPosition = std::move(r.search->position);
        }
        return;
    }

    if (r.filterVisible && !r.filterSubdirs && !r.filterText.isEmpty()) {
        m_proxyModel->setFilterPattern(r.filterText);
        m_delegate->setHighlightText(r.filterText);
    }
    applyViewPosition(r.position, r.selectChild);
    updateStatusBar();
}

// The entry directly inside `ancestor` on the way down to `descendant`
// (/a, /a/b/c → /a/b), or nothing if `ancestor` is not above `descendant`.
static QUrl childTowards(const QUrl &ancestor, const QUrl &descendant)
{
    const QUrl base = ancestor.adjusted(QUrl::StripTrailingSlash);
    const QUrl leaf = descendant.adjusted(QUrl::StripTrailingSlash);
    if (!base.isValid() || !base.isParentOf(leaf))
        return {};
    QString prefix = base.path();
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    const QString name = leaf.path().mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
    if (name.isEmpty())
        return {};
    QUrl child = base;
    child.setPath(prefix + name);
    return child;
}

ViewPosition Panel::captureViewPosition(bool fullSelection) const
{
    QAbstractItemView *view = currentView();
    ViewPosition pos;
    pos.current  = view->currentIndex().siblingAtColumn(0).data(FileSystemModel::UrlRole).toUrl();
    pos.scrollX  = view->horizontalScrollBar()->value();
    pos.scrollY  = view->verticalScrollBar()->value();
    pos.viewMode = static_cast<int>(m_viewMode);

    // Topmost from the selection ranges, not the selected indexes: a Ctrl+A is
    // one range however many rows it covers.
    QModelIndex top;
    if (QItemSelectionModel *sm = view->selectionModel()) {
        for (const QItemSelectionRange &range : sm->selection()) {
            if (!top.isValid() || range.top() < top.row())
                top = range.model()->index(range.top(), 0, range.parent());
        }
    }
    if (!top.isValid())
        return pos;
    const QUrl topUrl = top.data(FileSystemModel::UrlRole).toUrl();
    if (fullSelection) {
        pos.selection = selectedUrls();
        // Topmost first: it is what survives a trim.
        const qsizetype i = pos.selection.indexOf(topUrl);
        if (i > 0)
            pos.selection.move(i, 0);
    } else if (topUrl.isValid()) {
        pos.selection = {topUrl};
    }
    return pos;
}

void Panel::rememberPositionBeforeLeaving()
{
    if (m_currentTab < 0)
        return;
    // Search results are not the directory: nothing of them belongs on its entry.
    currentTab().history.setCurrentPosition(
        m_inSearchMode ? std::nullopt
                       : std::optional<ViewPosition>(captureViewPosition(/*fullSelection=*/false)));
}

void Panel::armPositionRestore(const QUrl &from)
{
    NavigationHistory &history = currentTab().history;
    // An explicit reveal (Open Containing Folder, a search result) says what to
    // select; the entry's own position must not override it.
    if (m_pendingAction != PendingAction::None) {
        history.takeCurrentPosition();
        m_pendingRestore.reset();
        return;
    }
    PendingRestore restore;
    restore.url         = currentUrl();
    restore.position    = history.takeCurrentPosition();
    restore.selectChild = childTowards(restore.url, from);
    if (restore.position || restore.selectChild.isValid())
        m_pendingRestore = std::move(restore);
    else
        m_pendingRestore.reset();
}

void Panel::applyViewPosition(const std::optional<ViewPosition> &position, const QUrl &selectChild)
{
    QAbstractItemView *view = currentView();
    QItemSelectionModel *sm = view->selectionModel();
    if (!sm)
        return;

    // Through the URL index, not a walk over the rows: O(selected), which
    // matters for a restored Ctrl+A over a large directory.
    const auto proxyIndexFor = [this](const QUrl &url) -> QModelIndex {
        if (m_inSearchMode) {
            const int row = m_searchResultsModel->rowForUrl(url);
            return row < 0 ? QModelIndex()
                           : m_searchProxyModel->mapFromSource(m_searchResultsModel->index(row, 0));
        }
        const int row = m_model->rowForUrl(url);
        return row < 0 ? QModelIndex() : m_proxyModel->mapFromSource(m_model->index(row, 0));
    };

    QList<QUrl> targets;
    QUrl currentTarget;
    if (selectChild.isValid()) {
        // Going up: the folder we came from wins over whatever was selected
        // here before. Hidden or filtered out, nothing is restored at all.
        if (!proxyIndexFor(selectChild).isValid())
            return;
        targets    = {selectChild};
        currentTarget = selectChild;
    } else if (position) {
        targets    = position->selection;
        currentTarget = position->current;
    } else {
        return;
    }

    QItemSelection selection;
    QModelIndex anchor; // topmost selected; targets are stored topmost first
    for (const QUrl &url : std::as_const(targets)) {
        const QModelIndex idx = proxyIndexFor(url);
        if (!idx.isValid())
            continue;
        selection.select(idx, idx);
        if (!anchor.isValid())
            anchor = idx;
    }
    sm->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QModelIndex current = proxyIndexFor(currentTarget);
    if (!current.isValid())
        current = anchor;
    if (current.isValid())
        sm->setCurrentIndex(current, QItemSelectionModel::NoUpdate);

    // Stands down any scroll retry from an earlier restore.
    const int generation = ++m_restoreGeneration;
    const QPersistentModelIndex keepVisible(anchor);

    if (!position || position->viewMode != static_cast<int>(m_viewMode)) {
        // Pixel offsets from another view mode mean nothing here.
        if (anchor.isValid())
            view->scrollTo(anchor);
        return;
    }

    // The scroll range fills in as the view lays rows out — in the icon modes
    // over several event-loop passes — so retry until it can hold the old
    // position, for up to a second. Then, if the selected item is not on
    // screen at that position, bring it in.
    const int scrollX = position->scrollX, scrollY = position->scrollY;
    // The retry refers to itself weakly; each queued call holds the strong
    // reference, so the closure is freed with the last pending timer.
    auto apply = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = apply;
    *apply = [this, view, scrollX, scrollY, generation, keepVisible, weak](int attempt) {
        if (generation != m_restoreGeneration || view != currentView())
            return;
        QScrollBar *v = view->verticalScrollBar();
        QScrollBar *h = view->horizontalScrollBar();
        if ((v->maximum() < scrollY || h->maximum() < scrollX) && attempt < 20) {
            if (auto self = weak.lock())
                QTimer::singleShot(50, this, [self, attempt]() { (*self)(attempt + 1); });
            return;
        }
        v->setValue(scrollY);
        h->setValue(scrollX);
        if (keepVisible.isValid()
            && !view->viewport()->rect().contains(view->visualRect(keepVisible)))
            view->scrollTo(keepVisible);
    };
    QTimer::singleShot(0, this, [apply]() { (*apply)(0); });
}

void Panel::relocateTabsUnder(const QUrl &root, const QUrl &fallback)
{
    const QUrl base = root.adjusted(QUrl::StripTrailingSlash);
    const auto isUnder = [&base](const QUrl &u) {
        return u.adjusted(QUrl::StripTrailingSlash) == base || base.isParentOf(u);
    };
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (!isUnder(m_tabs.at(i).history.current()))
            continue;
        if (i == m_currentTab) {
            // A real navigation: stops the listing, the directory watch and any
            // search running below the mountpoint, and moves the preview off it.
            if (m_inSearchMode)
                exitSearchMode();
            navigateTo(fallback);
        } else {
            m_tabs[i].history.push(fallback);
            // Filter and search results were for the old place.
            m_tabs[i].searchState.reset();
            m_tabs[i].filterVisible = false;
            m_tabs[i].filterText.clear();
            m_tabs[i].filterSubdirs = false;
            m_tabs[i].filterCrossFs = false;
            m_tabs[i].searchTerm.clear();
            m_recentTabIds.removeAll(m_tabs.at(i).id);
            // Its frozen views still list the old place; they would also keep
            // the lister holding a path on the device going away.
            destroyViewForTab(m_tabs.at(i).id);
            updateTabTitle(i);
        }
    }
}

void Panel::duplicateTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    newTab(m_tabs.at(index).history.current());
}

void Panel::switchToTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_currentTab == index)
        return;

    // Out of the recently-left list before the tab being left goes in: that
    // insertion may evict the oldest entry's views, and they must not be the
    // ones about to be shown.
    m_recentTabIds.removeAll(m_tabs.at(index).id);

    // Save the tab being left, then clear what belongs to it: the filter bar
    // and the search used to carry over into whichever tab came next.
    if (m_currentTab >= 0 && m_currentTab < m_tabs.size()) {
        m_tabs[m_currentTab].viewMode = static_cast<int>(m_viewMode);
        saveTabState(m_currentTab);
    }
    resetFilterQuietly();

    m_currentTab = index;

    m_tabBar->blockSignals(true);
    m_tabBar->setCurrentIndex(index);
    m_tabBar->blockSignals(false);

    const TabContext &tab = m_tabs.at(index);

    // The tab's own views. The one being left is frozen, keeping its rows,
    // selection and scroll for when it is shown again.
    TabView *previous = m_cur;
    TabView *tv = viewForTab(tab.id);
    if (tv != previous) {
        if (previous)
            previous->model->setFrozen(true);
        copyColumnLayout(previous, tv);
        bindTabView(tv);
    }
    stopLoadingIndicator();

    setViewMode(static_cast<ViewMode>(tab.viewMode));

    // Armed before the load, so the listing's first completed() applies it.
    if (!tab.reveal.isEmpty()) {
        setPendingReveal(tab.reveal);
        m_tabs[index].reveal.clear();
    }

    // The saved state is consumed here; it is re-saved when the tab is left.
    PendingRestore restore;
    restore.url           = tab.history.current();
    restore.filterVisible = tab.filterVisible;
    restore.filterText    = tab.filterText;
    restore.filterSubdirs = tab.filterSubdirs;
    restore.filterCrossFs = tab.filterCrossFs;
    restore.searchTerm    = tab.searchTerm;
    restore.position      = m_tabs[index].history.takeCurrentPosition();
    restore.search        = std::exchange(m_tabs[index].searchState, std::nullopt);
    if (restore.filterVisible || restore.position || restore.search)
        m_pendingRestore = std::move(restore);
    else
        m_pendingRestore.reset();

    const QUrl url = tab.history.current();
    if (url.isValid()) {
        // Pooled and complete: show it as it was and re-list the differences.
        // Anything else — new views, a listing left unfinished or failed, or a
        // tab moved elsewhere while frozen (unmount) — loads afresh.
        const bool reusable = m_model->directory() == url && !m_model->needsReload();
        if (reusable) {
            resumeTabView();
        } else {
            // From the source, not KIO's directory cache: a tab's frozen
            // lister stopped watching, so the cache entry it left behind when
            // its views were dropped can be stale, and serving from it showed
            // the folder as it was before the tab was left.
            loadUrl(url, /*takeFocus=*/true, /*reload=*/true);
        }
    }

    Q_EMIT navigationStateChanged();
    Q_EMIT previewItemChanged();
}

int Panel::currentTabIndex() const { return m_currentTab; }
int Panel::tabCount() const        { return m_tabs.size(); }

TabContext Panel::takeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return {};

    // A dragged-away tab takes its look with it.
    if (index == m_currentTab)
        saveTabState(index);
    m_recentTabIds.removeAll(m_tabs.at(index).id);
    TabContext ctx = m_tabs.at(index);

    if (m_tabs.size() == 1) {
        // Remove the last tab directly and signal the container to close this panel.
        // Its views stay as the panel's unassigned current one, frozen.
        m_cur->tabId = 0;
        m_model->setFrozen(true);
        m_tabs.removeAt(0);
        m_tabBar->blockSignals(true);
        m_tabBar->removeTab(0);
        m_tabBar->blockSignals(false);
        m_currentTab = -1;
        Q_EMIT closeRequested();
    } else {
        closeTab(index);
    }

    return ctx;
}

void Panel::clearAllTabs()
{
    // Every tab's views go, except the current ones, which become the panel's
    // unassigned view for whichever tab is inserted next.
    const QList<TabView *> views = m_views;
    for (TabView *tv : views)
        destroyTabView(tv);
    m_cur->tabId = 0;
    m_model->setFrozen(true);
    m_recentTabIds.clear();
    m_pendingRestore.reset();

    m_tabBar->blockSignals(true);
    m_tabs.clear();
    while (m_tabBar->count() > 0)
        m_tabBar->removeTab(0);
    m_tabBar->blockSignals(false);
    m_currentTab = -1;
}

void Panel::cancelDrag()
{
    m_dragging     = false;
    m_dragTabIndex = -1;
}

void Panel::insertTab(const TabContext &ctx, bool activate)
{
    m_tabs.append(ctx);
    if (ctx.history.currentPosition() || ctx.searchState)
        rememberViewState(ctx.id);

    const QString title = tabTitleForUrl(ctx.history.current());
    const int idx = m_tabs.size() - 1;

    m_tabBar->blockSignals(true);
    m_tabBar->insertTab(idx, title);
    m_tabBar->blockSignals(false);

    if (activate)
        switchToTab(idx);
    Q_EMIT tabCountChanged();
}

// ── View modes ────────────────────────────────────────────────────────────────

void Panel::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    if (m_currentTab >= 0 && m_currentTab < m_tabs.size())
        m_tabs[m_currentTab].viewMode = static_cast<int>(mode);

    if (mode == Details) {
        m_stack->setCurrentIndex(0);
    } else {
        m_stack->setCurrentIndex(1);
        applyListViewMode();
    }
    applyThumbnailViewMode();
    // Re-query the icons that are actually on screen. O(1) here, versus a
    // dataChanged spanning the whole model, which the proxy maps row by row.
    if (QAbstractItemView *view = currentView())
        view->viewport()->update();
    Q_EMIT viewModeChanged(static_cast<int>(mode));
}

Panel::ViewMode Panel::viewMode() const { return m_viewMode; }

bool Panel::foldersFirst() const { return m_foldersFirst; }
void Panel::setFoldersFirst(bool first)
{
    // A panel-wide choice: every loaded tab follows, as new ones do.
    m_foldersFirst = first;
    for (TabView *tv : std::as_const(m_views)) {
        tv->model->setFoldersFirst(first);  // keeps streaming-insert order consistent
        tv->proxy->setFoldersFirst(first);  // actually controls displayed sort order
    }
}

void Panel::setFolderSizeCalcEnabled(bool enabled)
{
    if (m_folderSizeCalcEnabled == enabled)
        return;
    m_folderSizeCalcEnabled = enabled;
    if (enabled)
        m_model->requestAllFolderSizes();
    else
        m_model->clearFolderSizes();
    Q_EMIT folderSizeCalcChanged(enabled);
}

void Panel::applyListViewMode()
{
    // Lay out incrementally rather than positioning every item before the first
    // frame can be drawn. Switching a directory of several thousand entries into
    // an icon mode otherwise blocks for the whole layout pass — which is why the
    // switch stuttered on large folders regardless of whether they held any
    // images at all. Batched spreads that across event-loop iterations, so the
    // view appears immediately and fills in.
    m_listView->setLayoutMode(QListView::Batched);
    m_listView->setBatchSize(256);

    // Scroll by pixels, not by items. With the default ScrollPerItem a wheel
    // notch advances QApplication::wheelScrollLines() *items*, which in a
    // wrapped icon grid is typically less than one row — so the bigger the
    // icons, the less the view moved per notch. Pixel scrolling also makes the
    // motion smooth rather than snapping cell to cell.
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    switch (m_viewMode) {
    case MediumIcons:
        m_listView->setViewMode(QListView::IconMode);
        m_listView->setIconSize(QSize(64, 64));
        m_listView->setGridSize(QSize(96, 96));
        m_listView->setFlow(QListView::LeftToRight);
        m_listView->setWrapping(true);
        m_listView->setResizeMode(QListView::Adjust);
        m_listView->setUniformItemSizes(true);
        break;
    case LargeIcons:
        m_listView->setViewMode(QListView::IconMode);
        m_listView->setIconSize(QSize(128, 128));
        m_listView->setGridSize(QSize(160, 160));
        m_listView->setFlow(QListView::LeftToRight);
        m_listView->setWrapping(true);
        m_listView->setResizeMode(QListView::Adjust);
        m_listView->setUniformItemSizes(true);
        break;
    case ExtraLargeIcons:
        m_listView->setViewMode(QListView::IconMode);
        m_listView->setIconSize(QSize(256, 256));
        m_listView->setGridSize(QSize(288, 288));
        m_listView->setFlow(QListView::LeftToRight);
        m_listView->setWrapping(true);
        m_listView->setResizeMode(QListView::Adjust);
        m_listView->setUniformItemSizes(true);
        break;
    default:
        break;
    }

    // Starting value for the wheel step. QAbstractSlider scrolls
    // wheelScrollLines() (3 by default) single steps per notch, so a third of a
    // row gives about one row per notch. QListView may recompute this itself on
    // the next relayout now that we're in ScrollPerPixel mode; either value is a
    // sane distance, whereas ScrollPerItem in a wrapped grid was not.
    const QSize grid = m_listView->gridSize();
    const int rowHeight = grid.height() > 0
                            ? grid.height()
                            : m_listView->fontMetrics().height() + 6;
    m_listView->verticalScrollBar()->setSingleStep(qMax(1, rowHeight / 3));
}

// ── Active state & dimming ────────────────────────────────────────────────────

void Panel::setActive(bool active)
{
    const bool wasActive = m_active;
    m_active = active;
    if (active && !wasActive)
        revalidateDirectory();
    QPalette pal = m_activeIndicator->palette();
    pal.setColor(QPalette::Window,
                 active ? palette().color(QPalette::Highlight)
                        : palette().color(QPalette::Mid));
    m_activeIndicator->setPalette(pal);
}

void Panel::revalidateDirectory()
{
    if (m_inSearchMode)
        return; // the view is showing search results, not the directory
    m_model->revalidateDirectory();
}

bool Panel::isActive() const { return m_active; }

// ── Settings ─────────────────────────────────────────────────────────────────

void Panel::saveSettings(const QString &groupName) const
{
    KConfigGroup group = KSharedConfig::openConfig()->group(groupName);
    saveSettings(group);
}

void Panel::restoreSettings(const QString &groupName)
{
    KConfigGroup group = KSharedConfig::openConfig()->group(groupName);
    restoreSettings(group);
}

void Panel::saveSettings(KConfigGroup &group) const
{
    group.writeEntry("TabCount", m_tabs.size());
    group.writeEntry("CurrentTab", m_currentTab);

    for (int i = 0; i < m_tabs.size(); ++i) {
        group.writeEntry(QStringLiteral("Tab%1URL").arg(i),
                         m_tabs.at(i).history.current().toString());
        group.writeEntry(QStringLiteral("Tab%1ViewMode").arg(i), m_tabs.at(i).viewMode);
    }

    QHeaderView *header = m_treeView->header();
    QList<int> hidden, widths;
    for (int i = 0; i < m_model->columnCount(); ++i) {
        if (header->isSectionHidden(i))
            hidden.append(i);
        widths.append(header->sectionSize(i));
    }
    group.writeEntry("HiddenColumns", hidden);
    group.writeEntry("ColumnWidths", widths);
}

// PanelDetails/HiddenColumns is the global reference any new window (tearoff,
// session restore) reads. It only changes when the user toggles a column, so
// it is written there rather than from saveSettings() — which runs for every
// panel on every session autosave and wrote identical data N times.
void Panel::saveColumnVisibilityGlobally() const
{
    if (m_embedded)
        return; // a preview's narrow view must not set every window's defaults
    QHeaderView *header = m_treeView->header();
    QList<int> hidden;
    for (int i = 0; i < m_model->columnCount(); ++i) {
        if (header->isSectionHidden(i))
            hidden.append(i);
    }

    KConfigGroup pd = KSharedConfig::openConfig()->group(QStringLiteral("PanelDetails"));
    if (pd.readEntry("HiddenColumns", QList<int>()) == hidden)
        return;
    pd.writeEntry("HiddenColumns", hidden);
}

// The width counterpart of saveColumnVisibilityGlobally(). A hidden section
// reports a size of zero, which would erase a perfectly good stored width, so
// hidden columns keep whatever was stored for them last.
void Panel::saveColumnWidthsGlobally() const
{
    if (m_embedded)
        return; // a preview's narrow view must not set every window's defaults
    QHeaderView *header = m_treeView->header();
    KConfigGroup pd = KSharedConfig::openConfig()->group(QStringLiteral("PanelDetails"));
    const QList<int> stored = pd.readEntry("ColumnWidths", QList<int>());

    QList<int> widths;
    widths.reserve(m_model->columnCount());
    for (int i = 0; i < m_model->columnCount(); ++i) {
        if (header->isSectionHidden(i))
            widths.append(stored.value(i, 0));
        else
            widths.append(header->sectionSize(i));
    }

    if (stored == widths)
        return;
    pd.writeEntry("ColumnWidths", widths);
}

void Panel::stretchLastColumn()
{
    if (m_applyingColumnWidths)
        return; // re-entered from our own resizeSection below

    QHeaderView *header = m_treeView->header();
    if (header->count() == 0)
        return;

    // The rightmost *visible* column in visual order — not logical index
    // count-1, which may be hidden or moved.
    int last = -1;
    for (int visual = header->count() - 1; visual >= 0; --visual) {
        const int logical = header->logicalIndex(visual);
        if (!header->isSectionHidden(logical)) {
            last = logical;
            break;
        }
    }
    if (last < 0)
        return;

    int used = 0;
    for (int i = 0; i < header->count(); ++i) {
        if (i != last && !header->isSectionHidden(i))
            used += header->sectionSize(i);
    }

    const int available = m_treeView->viewport()->width() - kTrailingDeadZone - used;
    const int target = qMax(header->minimumSectionSize(), available);
    if (target == header->sectionSize(last))
        return;

    m_applyingColumnWidths = true; // not a user drag: don't persist, don't recurse
    header->resizeSection(last, target);
    m_applyingColumnWidths = false;
}

// Seeds a panel that never ran session restore — the case for every new window,
// split and tear-off, all of which are built with tearOff=true precisely to
// avoid resurrecting the saved session.
void Panel::applyGlobalColumnWidths()
{
    if (m_globalWidthsApplied)
        return;
    m_globalWidthsApplied = true;

    const KConfigGroup pd = KSharedConfig::openConfig()->group(QStringLiteral("PanelDetails"));
    const QList<int> widths = pd.readEntry("ColumnWidths", QList<int>());
    if (widths.isEmpty())
        return; // nothing stored yet — leave Qt's defaults alone

    m_applyingColumnWidths = true;
    QHeaderView *header = m_treeView->header();
    for (int i = 0; i < widths.size() && i < m_model->columnCount(); ++i) {
        if (widths.at(i) > 0)
            header->resizeSection(i, widths.at(i));
    }
    m_applyingColumnWidths = false;
    stretchLastColumn();
}

void Panel::restoreSettings(KConfigGroup &group)
{
    QHeaderView *header = m_treeView->header();
    // Sections are set directly below, behind applyColumnSettings()'s back.
    m_cur->appliedHiddenColumnsValid = false;
    // This window's own saved widths outrank the global seed, so claim the
    // one-shot slot before loading tabs — switchToTab() below resets the model,
    // which calls applyColumnSettings() and would otherwise seed over them.
    m_globalWidthsApplied = true;
    m_applyingColumnWidths = true;
    if (group.hasKey("HiddenColumns")) {
        const QList<int> hidden = group.readEntry("HiddenColumns", QList<int>());
        const QSet<int> hiddenSet(hidden.cbegin(), hidden.cend());
        for (int i = 0; i < m_model->columnCount(); ++i) {
            if (i == FileSystemModel::Name)
                continue;
            hiddenSet.contains(i) ? header->hideSection(i) : header->showSection(i);
        }
    } else {
        for (int col : {FileSystemModel::Type,        FileSystemModel::Permissions,
                        FileSystemModel::DateCreated, FileSystemModel::DateAccessed,
                        FileSystemModel::Extension,   FileSystemModel::Owner}) {
            header->hideSection(col);
        }
    }
    const QList<int> widths = group.readEntry("ColumnWidths", QList<int>());
    for (int i = 0; i < widths.size() && i < m_model->columnCount(); ++i) {
        if (widths.at(i) > 0)
            header->resizeSection(i, widths.at(i));
    }
    m_applyingColumnWidths = false;
    stretchLastColumn();

    const int tabCount = group.readEntry("TabCount", 0);
    const int currentTab = group.readEntry("CurrentTab", 0);

    if (tabCount > 0) {
        for (int i = 0; i < tabCount; ++i) {
            const QString urlStr = group.readEntry(QStringLiteral("Tab%1URL").arg(i), QString());
            // Clamped: a value written before the List mode was removed can be
            // out of range, and applyListViewMode() would then leave the view
            // in its unconfigured default.
            const int vm = qBound(0, group.readEntry(QStringLiteral("Tab%1ViewMode").arg(i), 0),
                                  static_cast<int>(ExtraLargeIcons));
            const QUrl url = urlStr.isEmpty() ? QUrl() : QUrl(urlStr);
            TabContext ctx(url, vm);
            m_tabs.append(ctx);
            m_tabBar->addTab(tabTitleForUrl(url));
        }
        m_currentTab = -1;
        switchToTab(qBound(0, currentTab, m_tabs.size() - 1));
    }
}

// ── Accessors ─────────────────────────────────────────────────────────────────

FileSystemModel *Panel::model() const { return m_model; }

// ── Search & filter helpers ───────────────────────────────────────────────────

QAbstractItemModel *Panel::activeModel() const
{
    return m_inSearchMode
               ? static_cast<QAbstractItemModel *>(m_searchProxyModel)
               : static_cast<QAbstractItemModel *>(m_proxyModel);
}

void Panel::setActiveViewModel(QAbstractItemModel *model)
{
    QItemSelectionModel *oldTreeSel = m_treeView->selectionModel();
    QItemSelectionModel *oldListSel = m_listView->selectionModel();
    // Disconnect old selection model signals
    if (oldTreeSel)
        disconnect(oldTreeSel, nullptr, this, nullptr);
    if (oldListSel)
        disconnect(oldListSel, nullptr, this, nullptr);

    m_treeView->setModel(model);
    m_listView->setModel(model);

    // setModel() creates a fresh selection model and leaves the old one alive
    // (documented Qt behaviour). Every search on/off leaked two, each still
    // connected to its model's row signals, so row changes got slower the more
    // one searched. Only delete when the view really replaced it — setModel()
    // with the model already set is a no-op that keeps the same one.
    if (oldTreeSel && oldTreeSel != m_treeView->selectionModel())
        oldTreeSel->deleteLater();
    if (oldListSel && oldListSel != m_listView->selectionModel())
        oldListSel->deleteLater();

    // Reconnect to new selection models
    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Panel::onSelectionChanged);
    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &Panel::onSelectionChanged);
    Q_EMIT previewItemChanged();
}

void Panel::exitSearchMode()
{
    if (!m_inSearchMode)
        return;
    m_inSearchMode = false;
    ++m_searchSession;
    m_pendingSearchPosition.reset();
    m_searchEngine->cancel();
    m_searchResultsModel->clear();
    m_searchResultCount = 0;

    setActiveViewModel(m_proxyModel);
    m_treeView->setSortingEnabled(true);
    m_delegate->clearHighlight();
    updateStatusBar();
}

void Panel::showSubfolderSearch()
{
    showFilterBar(true);
}

void Panel::showFilterBar(bool withSubdirs)
{
    if (!m_filterBar->isVisible()) {
        m_filterBar->clearFilter();
        m_filterBar->show();
    }
    m_filterBar->setSubdirSearch(withSubdirs);
    m_filterBar->lineEdit()->setFocus();
    m_filterBar->lineEdit()->selectAll();
}

void Panel::clearFilter()
{
    m_filterDebounce->stop();
    m_searchEngine->cancel();
    if (m_inSearchMode)
        exitSearchMode();
    m_filterBar->clearFilter();
    m_filterBar->setHint(QString());
    m_filterBar->hide();
    m_proxyModel->setFilterPattern(QString());
    m_delegate->clearHighlight();
    currentView()->setFocus();
    updateStatusBar();
}

void Panel::updateHighlightDelegate()
{
    const QString term = m_inSearchMode
                             ? m_searchResultsModel->searchTerm()
                             : m_proxyModel->filterPattern();
    m_delegate->setHighlightText(term);
}

// ── Event filter ──────────────────────────────────────────────────────────────

bool Panel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabStack && event->type() == QEvent::Resize) {
        m_emptyDirLabel->setGeometry(m_tabStack->rect());
        return false;
    }
    if (watched == m_tabBar) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                const int idx = m_tabBar->tabAt(me->pos());
                if (idx >= 0) {
                    m_dragStartPos = me->pos();
                    m_dragTabIndex = idx;
                    m_dragging = false;
                }
            }
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                if (m_dragging) {
                    // The drop itself is detected by MainWindow's drag poll
                    // (Wayland-safe); here the release is only swallowed.
                    m_dragging = false;
                    m_dragTabIndex = -1;
                    return true;
                }
                m_dragTabIndex = -1;
            }
            if (me->button() == Qt::MiddleButton) {
                const int idx = m_tabBar->tabAt(me->pos());
                if (idx >= 0)
                    closeTab(idx);
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && m_dragTabIndex >= 0) {
            auto *me = static_cast<QMouseEvent *>(event);
            const QPoint delta = me->pos() - m_dragStartPos;
            if (!m_dragging && std::abs(delta.y()) > 8) {
                if (m_tabs.size() <= 1 && !m_dragLastTabAllowed) {
                    m_dragTabIndex = -1;
                    return false;
                }
                m_dragging = true;
                Q_EMIT tabDragStarted(this, m_dragTabIndex);
            }
            if (m_dragging)
                return true; // tracked by MainWindow's drag poll
        }
    }

    // A viewport resize changes how many rows are on screen.
    if (m_treeView && m_listView
        && (watched == m_treeView->viewport() || watched == m_listView->viewport())
        && event->type() == QEvent::Resize) {
        scheduleThumbnailUpdate();
        if (watched == m_treeView->viewport())
            stretchLastColumn(); // the space the last column may fill just changed
    }

    // Ctrl+Scroll on either viewport → cycle through view modes.
    if (m_treeView && m_listView
        && (watched == m_treeView->viewport() || watched == m_listView->viewport())
        && event->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::ControlModifier) {
            // One mode per full notch (120). A touchpad sends many small
            // deltas per gesture; stepping on each one jumped straight to the
            // last mode. Horizontal scroll (y == 0) is consumed but ignored.
            const int delta = we->angleDelta().y();
            if (delta == 0)
                return true;
            if ((delta > 0) != (m_zoomWheelAccum > 0))
                m_zoomWheelAccum = 0; // direction reversed: start over
            m_zoomWheelAccum += delta;
            int steps = 0;
            while (qAbs(m_zoomWheelAccum) >= 120) {
                steps += m_zoomWheelAccum < 0 ? 1 : -1;
                m_zoomWheelAccum += m_zoomWheelAccum < 0 ? 120 : -120;
            }
            if (steps != 0) {
                const int next = qBound(0, static_cast<int>(m_viewMode) + steps,
                                        static_cast<int>(ExtraLargeIcons));
                setViewMode(static_cast<ViewMode>(next));
            }
            return true;
        }
    }

    // Middle-click on a directory item → open in a new tab.
    if (m_treeView && m_listView
        && (watched == m_treeView->viewport() || watched == m_listView->viewport())
        && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            QAbstractItemView *view = (watched == m_treeView->viewport())
                ? static_cast<QAbstractItemView *>(m_treeView)
                : static_cast<QAbstractItemView *>(m_listView);
            const QModelIndex idx = view->indexAt(me->pos());
            if (idx.isValid()) {
                const QModelIndex col0 = idx.siblingAtColumn(0);
                if (activeModel()->data(col0, FileSystemModel::IsDirRole).toBool()) {
                    const QUrl url = activeModel()->data(col0, FileSystemModel::UrlRole).toUrl();
                    newTab(url);
                    return true;
                }
            }
        }
    }

    if ((watched == m_treeView || watched == m_listView)) {
        if (event->type() == QEvent::DragEnter) {
            auto *de = static_cast<QDragEnterEvent *>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            auto *de = static_cast<QDropEvent *>(event);
            if (de->mimeData()->hasUrls()) {
                handleDrop(de, watched);
                return true;
            }
        }

        // Quick filter: printable keys while file view has focus → show filter bar
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);

            // Space toggles the window's preview column. This takes the key
            // away from QAbstractItemView, whose own Space merely re-selects
            // the current row — nothing anyone reaches for.
            if (ke->key() == Qt::Key_Space && ke->modifiers() == Qt::NoModifier) {
                Q_EMIT previewToggleRequested();
                return true;
            }

            // Enter/Return opens the selected item (only when not editing inline).
            // With nothing selected there is nothing to open, so let the event
            // fall through rather than swallowing it.
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                // hasSelection() is O(1); materialising the selected rows here
                // walked every selected index on each keypress.
                const QItemSelectionModel *sm = currentView()->selectionModel();
                if (!sm || !sm->hasSelection())
                    return false;
                openSelected();
                return true;
            }

            // Escape clears/hides filter bar if visible
            if (ke->key() == Qt::Key_Escape && m_filterBar->isVisible()) {
                clearFilter();
                return true;
            }

            // Backspace removes a character from the active filter bar
            if (ke->key() == Qt::Key_Backspace && m_filterBar->isVisible()) {
                QApplication::sendEvent(m_filterBar->lineEdit(), event);
                return true;
            }

            // Printable chars (no ctrl/alt) start the quick filter
            const QString text = ke->text();
            if (!text.isEmpty() && (ke->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
                const QChar c = text.at(0);
                if (c.isPrint() && !c.isSpace()) {
                    if (!m_filterBar->isVisible())
                        showFilterBar(false);
                    QApplication::sendEvent(m_filterBar->lineEdit(), event);
                    return true;
                }
            }
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::BackButton) {
            goBack();
            return true;
        }
        if (me->button() == Qt::ForwardButton) {
            goForward();
            return true;
        }
        if (!m_active)
            Q_EMIT activated();
    }
    if (event->type() == QEvent::FocusIn) {
        if (!m_active)
            Q_EMIT activated();
    }
    return QWidget::eventFilter(watched, event);
}

// ── Private slots ─────────────────────────────────────────────────────────────


void Panel::onItemActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    if (m_inSearchMode) {
        // Navigate to the parent directory and select the result. Read through
        // activeModel(): rows are proxy rows now that results are sortable.
        const QModelIndex resultIdx = index.siblingAtColumn(0);
        const QUrl itemUrl =
            activeModel()->data(resultIdx, SearchResultsModel::UrlRole).toUrl();
        if (!itemUrl.isValid())
            return;
        const bool isDir =
            activeModel()->data(resultIdx, SearchResultsModel::IsDirRole).toBool();
        if (isDir) {
            // Dirs: navigate into them directly
            m_pendingUrls.clear();
            m_pendingAction = PendingAction::None;
            exitSearchMode();
            navigateTo(itemUrl);
        } else {
            // Files: navigate to parent, then select the file
            const QUrl parentUrl =
                itemUrl.adjusted(QUrl::StripTrailingSlash | QUrl::RemoveFilename);
            m_pendingUrls   = {itemUrl};
            m_pendingAction = PendingAction::Select;
            exitSearchMode();
            navigateTo(parentUrl);
        }
        return;
    }

    const QModelIndex col0 = index.siblingAtColumn(0);
    const QUrl url = activeModel()->data(col0, FileSystemModel::UrlRole).toUrl();
    if (activeModel()->data(col0, FileSystemModel::IsDirRole).toBool()) {
        navigateTo(url);
    } else {
        openItem(activeModel()->data(col0, FileSystemModel::FileItemRole).value<KFileItem>());
    }
}

void Panel::onLockClicked(int section)
{
    const QString key = currentUrl().toString();
    if (key.isEmpty())
        return;
    const Qt::SortOrder order = m_treeView->header()->sortIndicatorOrder();
    VelaSettings::setDirSortPref(key, section, order);
    auto *lockHeader = static_cast<SortLockHeaderView *>(m_treeView->header());
    lockHeader->setLockedSection(section);
}

void Panel::applySortPreference(const QUrl &url)
{
    const auto [col, order] = VelaSettings::dirSortPref(url.toString());
    auto *lockHeader = static_cast<SortLockHeaderView *>(m_treeView->header());
    lockHeader->setLockedSection(col);
    m_lastSortCol = col;
    m_applyingLockedSort = true;
    m_treeView->sortByColumn(col, order);
    m_applyingLockedSort = false;
}

void Panel::onDirectoryLoaded(const QUrl &)
{
    // The listing is complete: the animation stops and the status line goes back
    // to the full breakdown, whose folder/file split is only valid now that every
    // row is in.
    stopLoadingIndicator();

    // First successful listing since navigating here. urlChanged fires before
    // the listing even starts (and again on every re-list), so a mistyped
    // address used to land in Recent. Create/paste/F5 re-list and land here
    // too, but are not visits.
    if (std::exchange(m_visitPending, false))
        Q_EMIT directoryVisited(currentUrl());

    updateAddressBar();
    updateStatusBar();
    if (m_currentTab >= 0 && m_currentTab < m_tabs.size())
        updateTabTitle(m_currentTab);
    Q_EMIT urlChanged(currentUrl());
    Q_EMIT navigationStateChanged();
    Q_EMIT previewItemChanged();

    // Reaching onDirectoryLoaded means the listing succeeded (failures emit
    // directoryError and never get here), so any error banner from the previous
    // navigation is stale. Don't gate the empty-folder label on its visibility:
    // when it's still mid-fade from animatedHide(), a fast empty-dir load would
    // otherwise suppress the label (e.g. missing dir → empty dir).
    const bool empty = (m_model->rowCount() == 0);
    m_emptyDirLabel->setGeometry(m_tabStack->rect());
    m_emptyDirLabel->setVisible(empty);
    m_emptyDirLabel->raise();

    applyColumnSettings();

    // Auto-calculate folder sizes when the feature is enabled.
    if (m_folderSizeCalcEnabled)
        m_model->requestAllFolderSizes();

    // Return focus to the file view so the user can quick-filter or navigate
    // with the keyboard immediately after arriving via sidebar, address bar,
    // back/forward, etc.  Skip when an inline rename is about to steal focus.
    // An embedded panel (the preview) takes it only when focus is already
    // inside it: its listings must not pull the keyboard out of the tab.
    if (m_takeFocusOnLoad && m_pendingAction != PendingAction::SelectAndEdit
        && (!m_embedded || isAncestorOf(QApplication::focusWidget())))
        currentView()->setFocus();

    applyPendingSelection();
    applyPendingRestore();
}

// After a create/paste in `dir`: select `urls` (or start a rename) once they
// are listed. The watcher (KDirWatch/KDirNotify) already delivers new items as
// live deltas, so waiting for them replaces the full re-list create/paste used
// to force — no directory read, no folder-size requests for every subfolder.
// If they have not all shown up shortly (a mount without change notification),
// fall back to that re-list.
void Panel::awaitPendingUrls(const QUrl &dir, const QList<QUrl> &urls, PendingAction action)
{
    if (urls.isEmpty() || m_inSearchMode || currentUrl() != dir)
        return; // the user has moved on; don't hijack the new view's selection
    m_pendingUrls   = urls;
    m_pendingAction = action;
    if (!resolvePendingIfListed())
        m_pendingFallback->start();
}

// Applies the pending selection once every target is in the model. Partial
// arrivals wait (a paste lands in several batches) until the fallback fires.
bool Panel::resolvePendingIfListed()
{
    if (m_pendingAction == PendingAction::None || m_pendingUrls.isEmpty())
        return true;
    for (const QUrl &url : std::as_const(m_pendingUrls)) {
        if (m_model->rowForUrl(url) < 0)
            return false;
    }
    m_pendingFallback->stop();
    applyPendingSelection();
    return true;
}

// Resolves whatever was queued for after the load — search-result reveal,
// created/pasted files, or a fresh folder awaiting its inline rename. Previously
// three near-identical members each drove their own full-model scan; now the
// model resolves URLs in O(1) and this runs once.
void Panel::applyPendingSelection()
{
    if (m_pendingAction == PendingAction::None || m_pendingUrls.isEmpty())
        return;

    const PendingAction action = m_pendingAction;
    const QList<QUrl> targets = std::exchange(m_pendingUrls, {});
    m_pendingAction = PendingAction::None;

    QItemSelectionModel *sm = currentView()->selectionModel();
    if (!sm)
        return;

    QModelIndex firstProxy;
    QItemSelection selection;
    for (const QUrl &url : targets) {
        const int row = m_model->rowForUrl(url);
        if (row < 0)
            continue;
        const QModelIndex proxyIdx = m_proxyModel->mapFromSource(m_model->index(row, 0));
        if (!proxyIdx.isValid())
            continue;
        selection.select(proxyIdx,
                         proxyIdx.siblingAtColumn(m_proxyModel->columnCount() - 1));
        if (!firstProxy.isValid())
            firstProxy = proxyIdx;
    }

    if (!firstProxy.isValid())
        return;

    sm->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    if (action == PendingAction::SelectAndEdit) {
        const QModelIndex nameIdx = firstProxy.siblingAtColumn(FileSystemModel::Name);
        currentView()->setCurrentIndex(nameIdx);
        currentView()->scrollTo(nameIdx);
        currentView()->edit(nameIdx);
    } else {
        sm->setCurrentIndex(firstProxy, QItemSelectionModel::NoUpdate);
        currentView()->scrollTo(firstProxy);
    }
}

void Panel::onSelectionChanged()
{
    // Every tab's views are connected; only the shown one's selection counts.
    if (auto *sm = qobject_cast<QItemSelectionModel *>(sender());
        sm && sm != currentView()->selectionModel())
        return;
    m_selectionDirty = true;
    Q_EMIT previewItemChanged();

    // Only the selected-size/count part of the status line can have changed;
    // re-walking every row here made arrow-key navigation O(rows) per keypress.
    //
    // In a small directory the sum is trivial, so keep the status line live —
    // a debounce there is all latency and no benefit. Above the threshold it
    // coalesces instead: a rubber-band drag or a held Shift+Down emits this on
    // every intermediate selection and only the last one is ever read, so a
    // Ctrl+A over a huge directory pays the sum exactly once.
    if (activeModel()->rowCount() < kLargeDirectoryThreshold)
        refreshStatusText(); // also cancels any pending debounced pass
    else
        m_statusDebounce->start();
}

// Number of selected rows and the total bytes they hold.
//
// Deliberately avoids QItemSelectionModel::selectedIndexes(): that materialises
// one QModelIndex per row *per column* — nine per file here — and this then
// discards eight of every nine. The ranges already carry the row spans, and
// QItemSelection::merge keeps them non-overlapping, so the count is arithmetic
// on a handful of ranges (exactly one, after a Ctrl+A).
//
// The byte sum is still O(selected rows) — the proxy sorts, so a contiguous
// range of view rows maps to scattered source rows and each has to be resolved
// individually — but each step is now a mapToSource plus a vector read, with no
// QVariant and no KFileItem copy in the loop.
std::pair<int, KIO::filesize_t> Panel::selectionSummary() const
{
    QItemSelectionModel *sm = currentView()->selectionModel();
    if (!sm)
        return {0, 0};

    QAbstractItemModel *am = activeModel();
    auto *proxy = qobject_cast<QSortFilterProxyModel *>(am);
    auto *source = qobject_cast<FileItemModelBase *>(proxy ? proxy->sourceModel() : am);

    int count = 0;
    KIO::filesize_t bytes = 0;

    const QItemSelection selection = sm->selection();
    for (const QItemSelectionRange &range : selection) {
        // The details view selects whole rows (all columns), the icon views only
        // column 0. Counting the Name column once per row covers both without
        // double-counting the other eight.
        if (!range.isValid() || range.left() > FileSystemModel::Name
            || range.right() < FileSystemModel::Name) {
            continue;
        }
        count += range.bottom() - range.top() + 1;
        if (!source)
            continue;
        for (int row = range.top(); row <= range.bottom(); ++row) {
            const QModelIndex idx = am->index(row, FileSystemModel::Name, range.parent());
            bytes += source->fileSizeAtRow(proxy ? proxy->mapToSource(idx).row() : row);
        }
    }
    return {count, bytes};
}

void Panel::onHeaderContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QHeaderView *header = m_treeView->header();
    for (int i = 0; i < m_model->columnCount(); ++i) {
        const QString title = m_model->headerData(i, Qt::Horizontal).toString();
        QAction *action = menu.addAction(title);
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(i));
        if (i == FileSystemModel::Name)
            action->setEnabled(false);
        connect(action, &QAction::triggered, this, [this, header, i](bool checked) {
            checked ? header->showSection(i) : header->hideSection(i);
            // This is the one place column visibility actually changes.
            m_cur->appliedHiddenColumnsValid = false;
            saveColumnVisibilityGlobally();
            Q_EMIT columnVisibilityChanged();
        });
    }
    menu.exec(m_treeView->header()->mapToGlobal(pos));
}

// Column visibility changes only when the user toggles a column, but this used
// to be wired to modelReset *and* called from onDirectoryLoaded — so every
// navigation opened the config group twice to re-apply state that hadn't
// changed. Cache the resolved set and only touch the header when it differs.
void Panel::applyColumnSettings()
{
    // Ahead of the hidden-column early-out below: a panel that never ran session
    // restore still needs its widths seeded, and that has to happen even when the
    // visible columns already match.
    applyGlobalColumnWidths();

    KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("PanelDetails"));

    QSet<int> hiddenSet;
    if (group.hasKey(QStringLiteral("HiddenColumns"))) {
        const QList<int> hidden = group.readEntry("HiddenColumns", QList<int>());
        hiddenSet = QSet<int>(hidden.cbegin(), hidden.cend());
    } else {
        hiddenSet = {FileSystemModel::DateCreated, FileSystemModel::DateAccessed,
                     FileSystemModel::Extension,   FileSystemModel::Owner};
    }
    hiddenSet.remove(FileSystemModel::Name); // Name is never hideable

    if (m_cur->appliedHiddenColumnsValid && hiddenSet == m_cur->appliedHiddenColumns)
        return;
    m_cur->appliedHiddenColumns = hiddenSet;
    m_cur->appliedHiddenColumnsValid = true;

    QHeaderView *header = m_treeView->header();
    m_applyingColumnWidths = true; // hide/show resizes sections; not a user drag
    for (int i = 0; i < m_model->columnCount(); ++i) {
        if (i == FileSystemModel::Name)
            continue;
        hiddenSet.contains(i) ? header->hideSection(i) : header->showSection(i);
    }
    m_applyingColumnWidths = false;
    stretchLastColumn();
}

void Panel::onTabChanged(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (index == m_currentTab)
        return;
    switchToTab(index);
}

void Panel::onTabCloseRequested(int index)
{
    closeTab(index);
}

void Panel::onTabContextMenu(const QPoint &pos)
{
    const int idx = m_tabBar->tabAt(pos);
    if (idx < 0)
        return;

    QMenu menu(this);

    auto *actionClose = menu.addAction(i18n("Close Tab"));
    actionClose->setEnabled(m_tabs.size() > 1);
    connect(actionClose, &QAction::triggered, this, [this, idx]() { closeTab(idx); });

    auto *actionCloseOthers = menu.addAction(i18n("Close Other Tabs"));
    actionCloseOthers->setEnabled(m_tabs.size() > 1);
    connect(actionCloseOthers, &QAction::triggered, this, [this, idx]() { closeOtherTabs(idx); });

    menu.addSeparator();

    auto *actionDuplicate = menu.addAction(i18n("Duplicate Tab"));
    connect(actionDuplicate, &QAction::triggered, this, [this, idx]() { duplicateTab(idx); });

    menu.addSeparator();

    auto *actionCopyPath = menu.addAction(i18n("Copy Path"));
    connect(actionCopyPath, &QAction::triggered, this, [this, idx]() {
        const QUrl url = m_tabs.at(idx).history.current();
        // PreferLocalFile: a plain path for local tabs, the full URL for
        // sftp/smb/… — toLocalFile() copied an empty string for those.
        QGuiApplication::clipboard()->setText(url.toDisplayString(QUrl::PreferLocalFile));
    });

    menu.exec(m_tabBar->mapToGlobal(pos));
}

// ── Filter slots ──────────────────────────────────────────────────────────────

void Panel::onFilterChanged(const QString &pattern)
{
    if (m_filterBar->subdirSearch()) {
        // Subdirectory mode: cancel any running search and debounce a new one
        m_filterDebounce->stop();
        m_searchEngine->cancel();
        if (pattern.isEmpty()) {
            exitSearchMode();
        } else {
            m_filterDebounce->start();
        }
        updateStatusBar();
    } else {
        // Plain mode: filter visible items in current directory only
        if (m_inSearchMode)
            exitSearchMode();
        m_filterBar->setHint(QString());
        m_proxyModel->setFilterPattern(pattern);
        m_delegate->setHighlightText(pattern);
        keepFilterSelection(/*revealKept=*/true);
        currentView()->viewport()->update();
        updateStatusBar();
    }
}

void Panel::keepFilterSelection(bool revealKept)
{
    if (m_filterBar->lineEdit()->text().isEmpty())
        return;
    QAbstractItemView *view = currentView();
    QItemSelectionModel *sm = view->selectionModel();
    if (!sm)
        return;
    // Rows the filter hides leave the selection by themselves, so whatever is
    // still selected still matches.
    if (sm->hasSelection()) {
        const QModelIndex current = sm->currentIndex();
        if (revealKept && current.isValid() && sm->isRowSelected(current.row(), current.parent()))
            view->scrollTo(current);
        return;
    }
    const QModelIndex first = activeModel()->index(0, 0);
    if (!first.isValid())
        return;
    sm->setCurrentIndex(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(first);
}

void Panel::onFilterDismissed()
{
    clearFilter();
}

void Panel::onFilterSubdirToggled(bool on)
{
    const QString pattern = m_filterBar->lineEdit()->text();
    if (on) {
        // Switching to subdirectory mode: clear proxy filter, start search
        m_proxyModel->setFilterPattern(QString());
        m_delegate->clearHighlight();
        if (!pattern.isEmpty())
            m_filterDebounce->start();
    } else {
        // Switching to plain mode: exit search, apply proxy filter
        m_filterDebounce->stop();
        m_searchEngine->cancel();
        if (m_inSearchMode)
            exitSearchMode();
        m_filterBar->setHint(QString());
        m_proxyModel->setFilterPattern(pattern);
        m_delegate->setHighlightText(pattern);
        currentView()->viewport()->update();
    }
    updateStatusBar();
}

// ── Search slots ──────────────────────────────────────────────────────────────

void Panel::startSubfolderSearch(const QString &term)
{
    // The engine walks the local filesystem. On sftp/smb/… it used to finish
    // at once and report "0 results", which reads as "nothing matches".
    if (!currentUrl().isLocalFile()) {
        exitSearchMode();
        m_filterBar->setHint(i18n("Searching subfolders works in local folders only"));
        return;
    }
    showCachedSearch(term, {});
    m_searchEngine->search(currentUrl(), term, m_filterBar->crossFilesystems());
}

void Panel::showCachedSearch(const QString &term, const KFileItemList &items)
{
    // Switch to search results mode
    m_pendingSearchPosition.reset();
    ++m_searchSession;
    m_searchResultsModel->clear();
    m_searchResultsModel->setSearchTerm(term);
    m_searchResultCount = 0;
    m_emptyDirLabel->hide();

    m_inSearchMode = true;
    setActiveViewModel(m_searchProxyModel);
    // Sortable in search mode: the results model now exposes the same columns
    // and sort roles as the directory model, so there is nothing to disable.
    m_treeView->setSortingEnabled(true);

    m_delegate->setHighlightText(term);
    currentView()->viewport()->update();

    if (!items.isEmpty()) {
        m_searchResultsModel->addResults(items);
        m_searchResultCount = items.size();
    }
    updateStatusBar();
}

void Panel::onSearchResultsBatch(const KFileItemList &items)
{
    m_searchResultsModel->addResults(items);
    m_searchResultCount += items.size();
    // Selects the first result once; later batches keep it (rows sort in
    // around it, the selection follows the item).
    keepFilterSelection(/*revealKept=*/false);
    updateStatusBar();
}

// ── Private helpers ───────────────────────────────────────────────────────────

void Panel::loadUrl(const QUrl &url, bool takeFocus, bool reload)
{
    // Before setDirectory(): a cached listing can complete inside it.
    m_takeFocusOnLoad = takeFocus;
    // A navigation gets the folder's locked/default sort, applied when its
    // listing starts (see loadingStarted) — create/paste/F5 and a reload after
    // a failed trash re-list without it, so they keep a sort the user picked.
    m_sortPendingFor = url;
    m_visitPending = true;
    m_pendingFallback->stop();      // a create/paste wait belongs to the old directory
    m_model->setDirectory(url, reload);
    // INTENDED — do not flag the extra stat: it is one worker-thread stat per
    // navigation, and it catches what the listing cannot (see below).
    // Report a missing directory straight away rather than waiting for the
    // listing job to fail — and cover the case where the lister answers from
    // its cache without ever hitting the disk.
    m_model->revalidateDirectory();
    updateAddressBar();
    Q_EMIT urlChanged(url);
    Q_EMIT navigationStateChanged();
}

// ── Thumbnails ────────────────────────────────────────────────────────────────

// Upper bound on how long a pending thumbnail pass may be deferred by further
// triggers. QListView::Batched positions items over many event-loop passes and
// bumps the scroll range each time, which lands here faster than the debounce
// interval — so a plain restart-on-every-trigger debounce never fired at all
// until the whole directory had been laid out.
static constexpr int kThumbMaxDeferMs = 400;

void Panel::scheduleThumbnailUpdate()
{
    if (!m_thumbLoader || m_viewMode < MediumIcons || activeModel()->rowCount() == 0)
        return;

    if (!m_thumbDebounce->isActive()) {
        m_thumbPendingSince.start();
        m_thumbDebounce->start();
        return;
    }

    if (m_thumbPendingSince.isValid() && m_thumbPendingSince.elapsed() >= kThumbMaxDeferMs) {
        m_thumbDebounce->stop();
        updateVisibleThumbnails();
        return;
    }
    m_thumbDebounce->start();
}

// Thumbnails exist only in the icon modes. Details and List keep 16px MIME
// icons, so browsing a large source tree in Details generates nothing at all.
//
// This runs *during* the view-mode switch, so it must stay O(1): no model
// traversal, no loader reconfiguration, no job creation. Everything with a cost
// attached happens later, on the debounced pass, once the view has relaid out.
void Panel::applyThumbnailViewMode()
{
    if (!m_thumbLoader)
        return;

    const bool wantThumbs = (m_viewMode >= MediumIcons);
    m_model->setThumbnailsEnabled(wantThumbs);
    m_searchResultsModel->setThumbnailsEnabled(wantThumbs);

    if (!wantThumbs) {
        // Leaving icon mode: stop generating, and drop any pending request.
        m_thumbDebounce->stop();
        m_thumbLoader->cancel();
        return;
    }

    scheduleThumbnailUpdate();
}

bool Panel::visibleRowRange(int rowCount, int &firstRow, int &lastRow) const
{
    const QRect viewportRect = m_listView->viewport()->rect();
    if (!viewportRect.isValid() || rowCount == 0)
        return false;

    const QSize grid = m_listView->gridSize();

    // Fast path: a wrapped, uniform icon grid is pure arithmetic. The previous
    // implementation probed a lattice of points with indexAt() — several hundred
    // spatial lookups per pass, repeated on every scroll tick, to compute
    // something the geometry already tells us exactly.
    if (grid.width() > 0 && grid.height() > 0 && m_listView->uniformItemSizes()
        && m_listView->isWrapping() && m_listView->flow() == QListView::LeftToRight
        && m_listView->verticalScrollMode() == QAbstractItemView::ScrollPerPixel) {
        // An explicitly-set gridSize *is* the cell size — QListView ignores
        // spacing() in that layout — so no spacing term here.
        const int cellW = grid.width();
        const int cellH = grid.height();
        const int cols  = qMax(1, viewportRect.width() / cellW);
        const int top   = m_listView->verticalScrollBar()->value();

        // One cell row of slack above and two below. That absorbs any
        // off-by-one against QListView's own margins *and* doubles as the
        // prefetch: the old code looked ahead only in the scroll-down
        // direction, so scrolling back up always showed a band of MIME icons
        // first.
        const int firstCellRow = qMax(0, top / cellH - 1);
        const int lastCellRow  = (top + viewportRect.height()) / cellH + 2;

        firstRow = firstCellRow * cols;
        if (firstRow <= rowCount - 1) {
            lastRow = qMin(rowCount - 1, (lastCellRow + 1) * cols - 1);
            return true;
        }
        // Arithmetic put us past the end (stale scroll value mid-relayout) —
        // fall through and probe instead of requesting nothing.
    }

    // Fallback: probe a lattice of points across the viewport.
    //
    // Not indexAt(topLeft) plus a forward walk: with a large grid that corner
    // regularly falls in the gap *between* items, indexAt returns an invalid
    // index, and the walk then restarted from row 0 — so Extra Large kept
    // requesting thumbnails for the top of the directory no matter where the
    // scrollbar was. Probing is also immune to Batched layout leaving rows
    // without geometry.
    //
    // A step of half a grid cell cannot skip an item, and the probe count
    // depends on viewport size, never on directory size.
    const int stepX = qMax(16, (grid.width()  > 0 ? grid.width()  : 64) / 2);
    const int stepY = qMax(16, (grid.height() > 0 ? grid.height() : 64) / 2);

    int minRow = -1;
    int maxRow = -1;
    for (int y = 0; y < viewportRect.height() + stepY; y += stepY) {
        for (int x = 0; x < viewportRect.width() + stepX; x += stepX) {
            const QModelIndex idx = m_listView->indexAt(
                QPoint(qMin(x, viewportRect.width() - 1), qMin(y, viewportRect.height() - 1)));
            if (!idx.isValid())
                continue;
            const int r = idx.row();
            if (minRow < 0 || r < minRow)
                minRow = r;
            if (r > maxRow)
                maxRow = r;
        }
    }

    if (minRow < 0)
        return false; // nothing laid out yet — a later trigger will retry

    // Symmetric look-ahead so scrolling in either direction doesn't reveal
    // empty icons, without drifting toward "thumbnail the whole directory".
    static constexpr int kLookAhead = 12;
    firstRow = qMax(0, minRow - kLookAhead);
    lastRow  = qMin(maxRow + kLookAhead, rowCount - 1);
    return true;
}

void Panel::updateVisibleThumbnails()
{
    m_thumbPendingSince.invalidate();

    if (!m_thumbLoader || m_viewMode < MediumIcons)
        return;

    // Cheap and non-destructive now: a dpr change only affects the pixel size
    // asked of future jobs, so it no longer discards everything already cached
    // the moment the window is dragged to a differently-scaled screen.
    m_thumbLoader->setDevicePixelRatio(devicePixelRatioF());

    QAbstractItemModel *am = activeModel();
    const int rowCount = am->rowCount();
    if (rowCount == 0)
        return;

    int firstRow = 0;
    int lastRow  = 0;
    if (!visibleRowRange(rowCount, firstRow, lastRow))
        return;

    KFileItemList visible;
    visible.reserve(lastRow - firstRow + 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        const KFileItem item =
            am->data(am->index(row, 0), FileSystemModel::FileItemRole).value<KFileItem>();
        if (!item.isNull())
            visible.append(item);
    }

    m_thumbLoader->requestBatch(visible);
}

QAbstractItemView *Panel::currentView() const
{
    if (!m_stack)
        return nullptr; // only during construction, before the first view
    return m_stack->currentIndex() == 0
               ? static_cast<QAbstractItemView *>(m_treeView)
               : static_cast<QAbstractItemView *>(m_listView);
}

void Panel::updateAddressBar()
{
    m_addressBar->setUrl(currentUrl());
}

void Panel::updateTabTitle(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    m_tabBar->setTabText(index, tabTitleForUrl(m_tabs.at(index).history.current()));
}

QString Panel::tabTitleForUrl(const QUrl &url) const
{
    if (!url.isValid())
        return QStringLiteral("?");
    if (url.isLocalFile()) {
        const QString name = QFileInfo(url.toLocalFile()).fileName();
        return name.isEmpty() ? QStringLiteral("/") : name;
    }
    return url.host().isEmpty() ? url.toString() : url.host();
}

QModelIndexList Panel::selectedRowIndexes() const
{
    QItemSelectionModel *sm = currentView()->selectionModel();
    if (!sm)
        return {};

    // NOT selectedRows(): that returns only rows where *every* column is
    // selected. QTreeView defaults to SelectRows so it satisfies that, but
    // QListView (List and all icon modes) only ever selects column 0, so
    // selectedRows() came back empty there and every selection-based operation
    // silently did nothing.
    //
    // Filtering selectedIndexes() by column is correct for both: the details
    // view contributes one index per column per row, the list view exactly one.
    // No de-duplication is needed because each (row, column) appears once.
    //
    // From the selection ranges, not selectedIndexes(): that built one index per
    // row *per column* (nine per file) just for eight of them to be dropped —
    // after a Ctrl+A over 100k files, 900k indexes for every copy, cut, trash
    // or right-click. Only ranges covering the Name column count, which covers
    // both the details view (whole rows) and the icon views (column 0).
    QModelIndexList rows;
    const QItemSelection selection = sm->selection();
    QSet<int> seen; // ranges may overlap after toggling; used only if there are several
    const bool dedupe = selection.size() > 1;
    for (const QItemSelectionRange &range : selection) {
        if (!range.isValid() || range.left() > FileSystemModel::Name
            || range.right() < FileSystemModel::Name)
            continue;
        const QAbstractItemModel *model = range.model();
        for (int row = range.top(); row <= range.bottom(); ++row) {
            if (dedupe) {
                if (seen.contains(row))
                    continue;
                seen.insert(row);
            }
            rows.append(model->index(row, FileSystemModel::Name, range.parent()));
        }
    }
    return rows;
}

void Panel::recountItems()
{
    // Called on every search batch and every live add/delete, so it must not
    // walk the rows: 100k results arrive in ~1000 batches, and a walk per batch
    // made the search O(N²). With no filter the model's own counters answer in
    // O(1); only an active quick-filter needs a walk over the visible rows, and
    // that reads the items directly rather than boxing QVariants.
    QAbstractItemModel *am = activeModel();
    const int total = am->rowCount();

    auto *proxy = qobject_cast<FileFilterProxyModel *>(am);
    auto *base = proxy ? qobject_cast<FileItemModelBase *>(proxy->sourceModel()) : nullptr;
    if (base && !proxy->hasFilter()) {
        m_folderCount = base->dirCount();
    } else if (base) {
        m_folderCount = 0;
        for (int row = 0; row < total; ++row) {
            const int src = proxy->mapToSource(proxy->index(row, 0)).row();
            if (src >= 0 && base->itemAt(src).isDir())
                ++m_folderCount;
        }
    } else {
        m_folderCount = 0;
        for (int row = 0; row < total; ++row) {
            if (am->data(am->index(row, 0), FileSystemModel::IsDirRole).toBool())
                ++m_folderCount;
        }
    }
    m_fileCount = total - m_folderCount;
    Q_EMIT itemCountsChanged(m_folderCount, m_fileCount);
}

void Panel::updateStatusBar()
{
    // Rows changed under the selection (load, refresh, filter, mode switch), so
    // the cached summary can no longer be trusted.
    m_selectionDirty = true;

    // Counts are a function of the model, so recount here and let selection
    // changes go through refreshStatusText() instead.
    recountItems();
    refreshStatusText();
}

// Streaming progress from the model. The indicator only appears once the
// listing proves to be large: below the threshold the directory is finished
// before it would ever be seen, and a spinner that flashes for 40 ms on every
// navigation is worse than no spinner at all.
void Panel::onLoadingProgress(int rowCount)
{
    if (m_loadingIndicator || rowCount < kLargeDirectoryThreshold)
        return;
    m_loadingIndicator = true;
    m_spinnerFrame = 0;
    m_loadSpinner->start();
    refreshStatusText(); // replace the previous directory's stale text at once
}

void Panel::stopLoadingIndicator()
{
    m_loadSpinner->stop();
    m_loadingIndicator = false;
    m_spinnerFrame = 0;
}

void Panel::refreshStatusText()
{
    // A direct call supersedes any pending debounced one.
    m_statusDebounce->stop();

    QAbstractItemModel *am = activeModel();
    const int total = am->rowCount();

    if (m_selectionDirty) {
        std::tie(m_cachedSelCount, m_cachedSelSize) = selectionSummary();
        m_selectionDirty = false;
    }
    const int selCount = m_cachedSelCount;
    const KIO::filesize_t selSize = m_cachedSelSize;

    // While a large directory is still streaming in, the running total is the
    // only honest thing to show: rowCount() is O(1), but the folder/file split
    // comes from a full walk of the model, so it waits for the load to finish.
    // A selection made mid-load is still reported — the animation stays up
    // alongside it, so it remains obvious that the listing is incomplete.
    // U+25D0/D3/D1/D2 — circle filled left, bottom, right, top: a quarter-turn
    // per frame. Spelled out by codepoint rather than pasted inline so the
    // animation cannot be silently broken by a source re-encoding.
    static constexpr char16_t kSpinnerFrames[] = {0x25D0, 0x25D3, 0x25D1, 0x25D2};
    const QString spinner =
        m_loadingIndicator
            ? QString(QChar(kSpinnerFrames[m_spinnerFrame % 4])) + QStringLiteral("  ")
            : QString();

    QString text;
    if (m_inSearchMode) {
        text = i18n("Search: %1 results", total);
    } else if (m_filterBar->isVisible() && m_proxyModel->hasFilter()) {
        text = i18n("Filter: %1 of %2 items", total, m_model->rowCount());
    } else if (m_loadingIndicator) {
        text = i18n("%1 items…", total);
    } else {
        text = i18n("%1 items  |  %2 folders  %3 files", total, m_folderCount, m_fileCount);
    }
    if (selCount > 0)
        text += i18n("  |  %1 selected  (%2)", selCount, KIO::convertSize(selSize));

    m_statusBar->setText(spinner + text);
    Q_EMIT statusChanged(m_statusBar->text());
}

void Panel::setStatusBarVisible(bool visible)
{
    m_statusBar->setVisible(visible);
}

QString Panel::statusText() const
{
    return m_statusBar->text();
}

TabContext &Panel::currentTab()
{
    return m_tabs[m_currentTab];
}

const TabContext &Panel::currentTab() const
{
    return m_tabs.at(m_currentTab);
}

void Panel::startEditingAdjacentItem(int direction)
{
    QAbstractItemView *view = currentView();
    const QModelIndex cur = view->currentIndex();
    const int count = m_proxyModel->rowCount();
    if (count == 0)
        return;

    const int base = cur.isValid() ? cur.row() : 0;
    // Wrap-around using modulo (handles both +1 and -1 directions)
    const int nextRow = ((base + direction) % count + count) % count;

    QTimer::singleShot(0, this, [this, view, nextRow]() {
        if (nextRow >= m_proxyModel->rowCount())
            return;
        const QModelIndex next = m_proxyModel->index(nextRow, FileSystemModel::Name);
        view->selectionModel()->setCurrentIndex(
            next, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view->scrollTo(next);
        view->edit(next);
    });
}

// ── File operations ───────────────────────────────────────────────────────────

QList<QUrl> Panel::selectedUrls() const
{
    QList<QUrl> urls;
    QAbstractItemModel *am = activeModel();
    const QModelIndexList rows = selectedRowIndexes();
    urls.reserve(rows.size());
    for (const QModelIndex &idx : rows) {
        const QUrl url = am->data(idx, FileSystemModel::UrlRole).toUrl();
        if (url.isValid())
            urls.append(url);
    }
    return urls;
}

void Panel::renameSelected()
{
    if (m_addressBar->isEditing() || m_inSearchMode)
        return;

    if (selectedRowIndexes().size() > 1) {
        QMessageBox box(QMessageBox::Information, i18n("Rename"),
                        i18n("Renaming several items at once is not supported. "
                             "Select a single item to rename it."),
                        QMessageBox::Ok, this);
        prepareMessageBox(box);
        box.exec();
        return;
    }

    const QModelIndex idx = currentView()->currentIndex();
    if (!idx.isValid())
        return;
    // Start inline editing on the Name column of the current row
    currentView()->edit(idx.siblingAtColumn(FileSystemModel::Name));
}

void Panel::trashSelected()
{
    const KFileItemList items = selectedItems();
    if (items.isEmpty())
        return;
    QList<QUrl> urls;
    urls.reserve(items.size());
    for (const KFileItem &item : items)
        urls.append(item.url());

    // Already in the trash: Delete can only mean "for good".
    if (currentUrl().scheme() == QLatin1String("trash")) {
        deleteSelectedPermanently();
        return;
    }

    // The trash only takes files with a local path behind them. On sftp, smb
    // and the like KIO::trash fails, the optimistic removal is rolled back,
    // and nothing happens. Say so and offer the one thing that can work.
    const bool canTrash = std::all_of(items.cbegin(), items.cend(), [](const KFileItem &item) {
        return item.mostLocalUrl().isLocalFile();
    });
    if (!canTrash) {
        QMessageBox box(QMessageBox::Warning, i18n("No Trash Here"),
                        i18np("“%2” cannot be moved to the Trash: this location has no trash.",
                              "These %1 items cannot be moved to the Trash: this location has no trash.",
                              urls.size(), urls.first().fileName()),
                        QMessageBox::NoButton, this);
        box.setInformativeText(i18n("They can be deleted permanently instead. This cannot be undone."));
        QPushButton *del = box.addButton(i18n("Delete Permanently"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        prepareMessageBox(box);
        box.exec();
        if (box.clickedButton() == del)
            removeOptimistically(urls, FileOperationsManager::instance()->del(urls, window()));
        return;
    }

    if (VelaSettings::self()->confirmDelete()) {
        QMessageBox box(QMessageBox::Question, i18n("Move to Trash"),
                        i18np("Move %2 to Trash?", "Move %1 items to Trash?", urls.size(),
                              urls.size() == 1 ? urls.first().fileName() : QString()),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setDefaultButton(QMessageBox::Yes);
        prepareMessageBox(box);
        if (box.exec() != QMessageBox::Yes)
            return;
    }

    removeOptimistically(urls, FileOperationsManager::instance()->trash(urls, window()));
}

// Rows go before the job has run, so the view reacts instantly. In search mode
// they are removed from the search results — the rows on screen — not from the
// directory model underneath (which used to leave trashed files visible in the
// results). The directory model catches up by itself through its lister.
void Panel::removeOptimistically(const QList<QUrl> &urls, KJob *job)
{
    if (!m_inSearchMode) {
        m_model->optimisticallyRemove(urls);
        reconcileAfterFailure(job);
        return;
    }

    const KFileItemList removed = m_searchResultsModel->takeItems(urls);
    updateStatusBar();
    if (!job || removed.isEmpty())
        return;

    // Search results cannot be re-listed. After a failure — possibly partial —
    // stat each removed item and put back the ones still there, provided the
    // same search is still on screen.
    const int session = m_searchSession;
    connect(job, &KJob::result, this, [this, removed, session](KJob *j) {
        if (!j->error())
            return;
        for (const KFileItem &item : removed) {
            auto *stat = KIO::stat(item.url(), KIO::StatJob::SourceSide,
                                   KIO::StatDefaultDetails, KIO::HideProgressInfo);
            connect(stat, &KJob::result, this, [this, stat, session](KJob *) {
                if (stat->error() || !m_inSearchMode || m_searchSession != session)
                    return;
                const QUrl url = stat->url();
                if (m_searchResultsModel->rowForUrl(url) >= 0)
                    return;
                m_searchResultsModel->addResults({KFileItem(stat->statResult(), url)});
                updateStatusBar();
            });
        }
    });
}

// The rows were removed before the job ran. If it fails — no trash on that
// mount, permission denied, cancelled half-way — some or all of the files are
// still there but invisible. Re-list rather than re-insert: after a partial
// failure only the listing knows which ones actually went. A full reload, not
// immediateRefresh(): the lister never saw those rows leave, so its diff would
// report nothing and leave them hidden.
void Panel::reconcileAfterFailure(KJob *job)
{
    if (!job)
        return;
    const QUrl dir = currentUrl();
    connect(job, &KJob::result, this, [this, dir](KJob *j) {
        if (j->error() && currentUrl() == dir && !m_inSearchMode)
            m_model->setDirectory(dir, /*reload=*/true);
    });
}

void Panel::deleteSelectedPermanently()
{
    const QList<QUrl> urls = selectedUrls();
    if (urls.isEmpty())
        return;

    // Permanent deletion always confirms — confirmDelete only governs the
    // recoverable Trash path.
    QMessageBox box(QMessageBox::Warning, i18n("Permanently Delete"),
                    i18np("Permanently delete %2?", "Permanently delete %1 items?", urls.size(),
                          urls.size() == 1 ? urls.first().fileName() : QString()),
                    QMessageBox::Yes | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Cancel);
    prepareMessageBox(box);
    if (box.exec() != QMessageBox::Yes)
        return;
    removeOptimistically(urls, FileOperationsManager::instance()->del(urls, window()));
}

void Panel::copySelectedToClipboard()
{
    if (m_addressBar->isEditing())
        return;
    const QList<QUrl> urls = selectedUrls();
    if (urls.isEmpty())
        return;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    QGuiApplication::clipboard()->setMimeData(mime);
}

void Panel::cutSelectedToClipboard()
{
    if (m_addressBar->isEditing())
        return;
    const QList<QUrl> urls = selectedUrls();
    if (urls.isEmpty())
        return;
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    mime->setData(QStringLiteral("x-kde-cut-selection"), QByteArray("1"));
    QGuiApplication::clipboard()->setMimeData(mime);
    m_delegate->setCutUrls(QSet<QUrl>(urls.begin(), urls.end()));
    currentView()->viewport()->update();
}

void Panel::pasteFromClipboard()
{
    if (m_addressBar->isEditing())
        return;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls())
        return;
    const QList<QUrl> urls = mime->urls();
    if (urls.isEmpty())
        return;
    const bool isCut = mime->data(QStringLiteral("x-kde-cut-selection")) == QByteArray("1");
    const QUrl destUrl = currentUrl();
    const QUrl destDir = destUrl.adjusted(QUrl::StripTrailingSlash);
    const auto parentOf = [](const QUrl &u) {
        return u.adjusted(QUrl::RemoveFilename).adjusted(QUrl::StripTrailingSlash);
    };

    QList<KIO::CopyJob *> jobs;
    if (isCut) {
        // Moving a file into the folder it already sits in would move it onto
        // itself, which KIO reports as an error. Same filter the drop code
        // applies; the rest of a mixed selection still moves.
        QList<QUrl> toMove;
        for (const QUrl &src : urls) {
            if (parentOf(src) != destDir)
                toMove.append(src);
        }
        if (toMove.isEmpty())
            return; // nothing consumed, so the cut stays on the clipboard
        if (KIO::CopyJob *job = FileOperationsManager::instance()->move(toMove, destUrl, window())) {
            jobs.append(job);
            // A cut is consumed by its paste: the sources no longer exist, so a
            // second paste could only fail. Clear the clipboard once the move
            // succeeded — unless something else was copied in the meantime.
            connect(job, &KJob::result, this, [this, urls](KJob *j) {
                if (j->error())
                    return;
                QClipboard *cb = QGuiApplication::clipboard();
                const QMimeData *now = cb->mimeData();
                if (now && now->data(QStringLiteral("x-kde-cut-selection")) == QByteArray("1")
                    && now->urls() == urls)
                    cb->clear();
                m_delegate->clearCutUrls();
                currentView()->viewport()->update();
            });
        }
    } else {
        // Copies into the source's own folder are duplicates: KIO names them
        // "x (1)" at copy time. Everything else is a plain copy, where a name
        // conflict goes through the overwrite prompt.
        QList<QUrl> sameDir, otherDir;
        for (const QUrl &src : urls)
            (parentOf(src) == destDir ? sameDir : otherDir).append(src);
        if (KIO::CopyJob *job = FileOperationsManager::instance()->duplicate(sameDir, destUrl, window()))
            jobs.append(job);
        if (KIO::CopyJob *job = FileOperationsManager::instance()->copy(otherDir, destUrl, window()))
            jobs.append(job);
    }

    if (jobs.isEmpty())
        return;

    // Select what actually landed, as the jobs report it: with auto-rename, or
    // "Rename" / "Skip" answered in the conflict prompt, the names cannot be
    // known up front. Only top-level entries count — copyingDone also fires
    // for every file inside a copied folder.
    auto landed    = QSharedPointer<QList<QUrl>>::create();
    auto remaining = QSharedPointer<int>::create(jobs.size());
    const auto record = [landed, destDir, parentOf](const QUrl &to) {
        if (parentOf(to) == destDir && !landed->contains(to))
            landed->append(to);
    };
    for (KIO::CopyJob *job : std::as_const(jobs)) {
        connect(job, &KIO::CopyJob::copyingDone, this,
                [record](KIO::Job *, const QUrl &, const QUrl &to, const QDateTime &, bool, bool) {
            record(to);
        });
        connect(job, &KIO::CopyJob::copyingLinkDone, this,
                [record](KIO::Job *, const QUrl &, const QString &, const QUrl &to) { record(to); });
        connect(job, &KJob::result, this, [this, remaining, landed, destUrl](KJob *) {
            if (--(*remaining) == 0 && !landed->isEmpty())
                awaitPendingUrls(destUrl, *landed, PendingAction::Select);
        });
    }
}

// Why `name` cannot become a new entry in the current folder, or an empty
// string when it can. `dest` receives the entry's URL. The existence check is
// for feedback only — mkdir/put still refuse to replace, which covers an entry
// appearing in between.
QString Panel::newEntryNameProblem(const QString &name, QUrl &dest) const
{
    if (name.contains(QLatin1Char('/')) || name == QLatin1String(".")
        || name == QLatin1String("..")) {
        return i18n("\"%1\" is not a valid name.", name);
    }
    dest = FileOperationsManager::childUrl(currentUrl(), name);

    bool exists = m_model->rowForUrl(dest) >= 0;
    if (!exists && dest.isLocalFile()) {
        // Also catches hidden entries and dangling symlinks the view isn't showing.
        const QFileInfo fi(dest.toLocalFile());
        exists = fi.exists() || fi.isSymLink();
    }
    if (exists)
        return i18n("\"%1\" already exists.", name);
    return {};
}

void Panel::createNewFolder()
{
    // Re-prompt until the name is usable, keeping what the user typed so they
    // only have to correct it — the same rules as New File.
    QString name = i18n("New Folder");
    QString prompt = i18n("Folder name:");
    QUrl dest;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, i18n("New Folder"), prompt,
                                     QLineEdit::Normal, name, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        const QString problem = newEntryNameProblem(name, dest);
        if (problem.isEmpty())
            break;
        prompt = problem + QLatin1Char('\n') + i18n("Folder name:");
    }

    KJob *job = FileOperationsManager::instance()->mkdir(currentUrl(), name, window());
    if (job) {
        const QUrl dir = currentUrl();
        connect(job, &KJob::result, this, [this, dest, dir](KJob *j) {
            if (!j->error())
                awaitPendingUrls(dir, {dest}, PendingAction::Select);
        });
    }
}

// ── Drag and drop ─────────────────────────────────────────────────────────────

void Panel::handleDrop(QDropEvent *event, QObject *targetView)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    QUrl destUrl = currentUrl();
    auto *view = static_cast<QAbstractItemView *>(targetView);
    // The drop is delivered to the view, so its position is in view
    // coordinates, while indexAt() reads viewport coordinates. In the details
    // view the two differ by the header height — dropping onto a folder landed
    // a row or two above it, and on the topmost rows landed on nothing at all
    // and silently fell back to the current directory.
    const QPoint viewportPos = view->viewport()->mapFrom(view, event->position().toPoint());
    const QModelIndex idx = view->indexAt(viewportPos);
    if (idx.isValid()) {
        const QModelIndex col0 = idx.siblingAtColumn(0);
        if (activeModel()->data(col0, FileSystemModel::IsDirRole).toBool())
            destUrl = activeModel()->data(col0, FileSystemModel::UrlRole).toUrl();
    }

    const QList<QUrl> sources = event->mimeData()->urls();
    if (sources.isEmpty()) {
        event->ignore();
        return;
    }

    // Ctrl copies, everything else moves. Alt deliberately does nothing: it used
    // to open a Copy / Move / Link menu, so the same gesture could mean three
    // different things depending on a key the user may not have realised was
    // down. Read from the event rather than QApplication::keyboardModifiers(),
    // which reports whatever is held when the handler runs rather than what the
    // drop was actually made with.
    const bool copy = (event->modifiers() & Qt::ControlModifier)
                      || event->dropAction() == Qt::CopyAction;

    // Tell the source the drop landed before anything else happens. The drag
    // session is still live at this point — the source application is blocked
    // waiting for this handler to return, and on Wayland the compositor still
    // holds the pointer grab — so the confirmation must not run here. Cancelling
    // afterwards is purely our business; the source has nothing left to undo for
    // a file-URL drag.
    event->acceptProposedAction();

    // Moving a file into the folder it already sits in is a no-op, so it should
    // not ask. Dropping a mixed selection still moves the rest. A *copy* onto
    // the same folder is left alone: that one means "duplicate this", which is
    // a real operation KIO handles by renaming the copy.
    QList<QUrl> pending = sources;
    if (!copy) {
        const QUrl destDir = destUrl.adjusted(QUrl::StripTrailingSlash);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&destDir](const QUrl &u) {
                                         return u.adjusted(QUrl::RemoveFilename)
                                                 .adjusted(QUrl::StripTrailingSlash) == destDir;
                                     }),
                      pending.end());
        if (pending.isEmpty())
            return;
    }

    // Deferred to the next event-loop turn so the drag has fully unwound before
    // a modal dialog opens.
    QTimer::singleShot(0, this, [this, pending, destUrl, copy]() {
        if (!confirmDrop(pending, destUrl, copy))
            return;
        if (copy)
            FileOperationsManager::instance()->copy(pending, destUrl, window());
        else
            FileOperationsManager::instance()->move(pending, destUrl, window());
    });
}

// Every drop confirms — there is no "just do it" path and no setting to disable
// this. A drag is easy to start by accident and its destination depends on
// whatever row happened to be under the cursor, which is exactly the
// combination that silently moves a folder somewhere unintended.
bool Panel::confirmDrop(const QList<QUrl> &sources, const QUrl &dest, bool copy) const
{
    const int count = sources.size();
    const QString destName =
        dest.fileName().isEmpty() ? dest.toDisplayString(QUrl::PreferLocalFile) : dest.fileName();

    QString text;
    if (count == 1) {
        const QString name = sources.first().fileName();
        text = copy ? i18n("Copy “%1” to “%2”?", name, destName)
                    : i18n("Move “%1” to “%2”?", name, destName);
    } else {
        text = copy ? i18np("Copy %1 item to “%2”?", "Copy %1 items to “%2”?", count, destName)
                    : i18np("Move %1 item to “%2”?", "Move %1 items to “%2”?", count, destName);
    }

    // Sources can come from several directories at once (a multi-selection in
    // search results, or a drag from another application), so only claim a
    // single origin when there genuinely is one.
    const QUrl firstParent = sources.first().adjusted(QUrl::RemoveFilename);
    bool sameParent = true;
    for (const QUrl &u : sources) {
        if (u.adjusted(QUrl::RemoveFilename) != firstParent) {
            sameParent = false;
            break;
        }
    }
    const QString fromText = sameParent
        ? firstParent.adjusted(QUrl::StripTrailingSlash).toDisplayString(QUrl::PreferLocalFile)
        : i18n("(several locations)");

    QMessageBox box(QMessageBox::Question,
                    copy ? i18n("Confirm Copy") : i18n("Confirm Move"),
                    text, QMessageBox::NoButton, window());
    box.setInformativeText(i18n("From:\t%1\nTo:\t%2", fromText,
                                dest.toDisplayString(QUrl::PreferLocalFile)));

    // Full list behind the expander, so the dialog is the same size for two
    // files as for two thousand.
    static constexpr int kMaxListed = 1000;
    QStringList names;
    names.reserve(qMin(count, kMaxListed));
    for (const QUrl &u : sources) {
        if (names.size() >= kMaxListed) {
            names.append(i18n("… and %1 more", count - kMaxListed));
            break;
        }
        const QString name = u.fileName();
        names.append(name.isEmpty() ? u.toDisplayString(QUrl::PreferLocalFile) : name);
    }
    box.setDetailedText(names.join(QLatin1Char('\n')));

    QPushButton *confirmBtn =
        box.addButton(copy ? i18n("Copy") : i18n("Move"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(confirmBtn);
    prepareMessageBox(box);
    box.exec();
    return box.clickedButton() == confirmBtn;
}

KFileItemList Panel::selectedItems() const
{
    KFileItemList items;
    QAbstractItemModel *am = activeModel();
    const QModelIndexList rows = selectedRowIndexes();
    items.reserve(rows.size());
    for (const QModelIndex &idx : rows) {
        const KFileItem item =
            am->data(idx, FileSystemModel::FileItemRole).value<KFileItem>();
        if (!item.isNull())
            items.append(item);
    }
    return items;
}

KFileItem Panel::soleSelectedItem(int &count) const
{
    count = 0;
    QItemSelectionModel *sm = currentView()->selectionModel();
    if (!sm)
        return {};
    QModelIndex only;
    const QItemSelection selection = sm->selection();
    for (const QItemSelectionRange &range : selection) {
        // Same column rule as selectionSummary(): count the Name column once
        // per row, whichever view produced the range.
        if (!range.isValid() || range.left() > FileSystemModel::Name
            || range.right() < FileSystemModel::Name) {
            continue;
        }
        count += range.bottom() - range.top() + 1;
        // No early return once count passes 1: the caller shows this number as
        // "N items selected", so every range has to be added up. Ranges are a
        // handful even for a selection of thousands of rows.
        if (count == 1)
            only = range.model()->index(range.top(), FileSystemModel::Name, range.parent());
    }
    if (count != 1 || !only.isValid())
        return {};
    return activeModel()->data(only, FileSystemModel::FileItemRole).value<KFileItem>();
}

void Panel::openSelected()
{
    // One item only. Opening a multi-selection started one job per file, so
    // Enter on 300 selected images launched 300 viewer instances. Opening
    // several on purpose stays available through the context menu's
    // application entries, which hand the whole list to one app.
    int count = 0;
    const KFileItem item = soleSelectedItem(count);
    if (count == 0)
        return;
    if (count > 1) {
        QMessageBox box(QMessageBox::Information, i18n("Open"),
                        i18np("Opening several items at once is not supported "
                              "(%1 item selected). Select a single item to open it.",
                              "Opening several items at once is not supported "
                              "(%1 items selected). Select a single item to open it.",
                              count),
                        QMessageBox::Ok, this);
        prepareMessageBox(box);
        box.exec();
        return;
    }

    openItem(item);
}

void Panel::openItem(const KFileItem &item)
{
    if (item.isNull())
        return;
    if (item.isDir()) {
        navigateTo(item.url());
        return;
    }
    // Only pass a type that is already known. item.mimetype() would sniff
    // the file on the GUI thread; an empty type lets the job determine it
    // asynchronously.
    auto *job = new KIO::OpenUrlJob(item.url(), item.isMimeTypeKnown()
                                                    ? item.currentMimeType().name()
                                                    : QString());
    job->setUiDelegate(
        KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window()));
    job->start();
}

bool Panel::selectUrl(const QUrl &url)
{
    QModelIndex idx;
    if (m_inSearchMode) {
        const int row = m_searchResultsModel->rowForUrl(url);
        if (row >= 0)
            idx = m_searchProxyModel->mapFromSource(m_searchResultsModel->index(row, 0));
    } else {
        const int row = m_model->rowForUrl(url);
        if (row >= 0)
            idx = m_proxyModel->mapFromSource(m_model->index(row, 0));
    }
    if (!idx.isValid())
        return false;
    QAbstractItemView *view = currentView();
    view->selectionModel()->setCurrentIndex(
        idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(idx);
    return true;
}

void Panel::clearSelection()
{
    currentView()->clearSelection();
}

void Panel::focusView()
{
    currentView()->setFocus(Qt::OtherFocusReason);
}

void Panel::showItemContextMenu(const KFileItem &item, const QPoint &globalPos, QMenu *hostMenu)
{
    // As in showContextMenu: plugins parent what they create to this, and it
    // must outlive every menu holding their actions.
    QWidget pluginParent(this);

    if (item.isNull()) {
        if (m_errorWidget->isVisible())
            return;
        clearSelection();
        QMenu menu(this);
        buildEmptyContextMenu(menu, &pluginParent);
        menu.exec(globalPos);
        return;
    }
    if (!selectUrl(item.url())) {
        if (hostMenu)
            hostMenu->exec(globalPos);
        return;
    }
    const KFileItemList items = selectedItems();
    if (items.isEmpty())
        return;
    if (!hostMenu) {
        QMenu menu(this);
        buildFileContextMenu(menu, items, &pluginParent);
        menu.exec(globalPos);
        return;
    }
    hostMenu->addSeparator();
    // The type is known for a listed item; iconName() would otherwise sniff.
    const QIcon icon = item.isMimeTypeKnown() ? QIcon::fromTheme(item.iconName()) : QIcon();
    QMenu *fileMenu = hostMenu->addMenu(icon, item.name());
    buildFileContextMenu(*fileMenu, items, &pluginParent);
    hostMenu->exec(globalPos);
    // The caller's menu outlives this scope; the plugin objects do not.
    delete fileMenu;
}

void Panel::openTerminalHere()
{
    openTerminalAt(currentUrl().toLocalFile());
}

void Panel::openTerminalAt(const QString &path)
{
    if (path.isEmpty())
        return;
    const QStringList parts = VelaSettings::self()->terminalCommand()
                                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return;
    // Static overload (program, args, workingDirectory) — the terminal inherits
    // the working directory and opens there, whichever terminal is configured.
    QProcess::startDetached(parts.first(), parts.mid(1), path);
}

void Panel::addOpenInActions(QMenu &menu, const QList<VelaSettings::OpenInApp> &apps,
                             const QList<QUrl> &urls)
{
    if (apps.isEmpty() || urls.isEmpty())
        return;

    // Launching through the desktop entry rather than a hand-split command line
    // is what makes this correct: %F/%U placeholders, Terminal=true, Path= and
    // startup notification all come from the .desktop file.
    const auto launch = [this, urls](const KService::Ptr &service) {
        auto *job = new KIO::ApplicationLauncherJob(service);
        job->setUrls(urls);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
    };
    const auto iconFor = [](const KService::Ptr &service) {
        return QIcon::fromTheme(service->icon(),
                                QIcon::fromTheme(QStringLiteral("application-x-executable")));
    };

    QMenu *submenu = nullptr;
    for (const auto &app : apps) {
        // An uninstalled app is kept in Settings (greyed) but has nothing to
        // offer here.
        const KService::Ptr service = KService::serviceByStorageId(app.desktopId);
        if (!service)
            continue;
        QAction *action;
        if (app.inSubmenu) {
            if (!submenu) {
                submenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("document-open")),
                                       i18n("Open In"));
            }
            action = submenu->addAction(iconFor(service), service->name());
        } else {
            action = menu.addAction(iconFor(service), i18n("Open in %1", service->name()));
        }
        connect(action, &QAction::triggered, this, [launch, service]() { launch(service); });
    }
}

void Panel::applyAppearanceSettings()
{
    VelaSettings *s = VelaSettings::self();

    for (TabView *tv : std::as_const(m_views))
        applyViewSettings(tv); // row colours, single-click opening
    setActive(m_active);

    if (m_thumbLoader) {
        m_thumbLoader->setMaxFileSize(qint64(s->thumbnailMaxSizeMiB()) * 1024 * 1024);
        // Raising the limit makes previously-skipped files eligible; ask for a
        // pass so they fill in without waiting for a scroll.
        scheduleThumbnailUpdate();
    }

    // Apply the show-hidden default only when it changes, so a manual Ctrl+H
    // toggle survives an unrelated settings Apply.
    const int wantHidden = s->showHiddenDefault() ? 1 : 0;
    if (m_appliedShowHiddenDefault != wantHidden) {
        m_appliedShowHiddenDefault = wantHidden;
        setShowHidden(wantHidden == 1);
    }
    for (TabView *tv : std::as_const(m_views))
        tv->model->invalidateDateColumns();
}

void Panel::focusAddressBar()
{
    m_addressBar->enterEditMode();
}

void Panel::toggleHidden()
{
    setShowHidden(!showHidden());
}

bool Panel::showHidden() const
{
    return m_showHidden;
}

// One setting for the panel. Frozen tabs pick it up when shown again.
void Panel::setShowHidden(bool show)
{
    m_showHidden = show;
    if (m_model->showHidden() != show)
        m_model->setShowHidden(show);
}

void Panel::createNewFolderAutoNamed()
{
    // Find an unused "New Folder" name. The model already holds the full
    // directory listing, so resolve the collision against that instead of
    // stat()ing the filesystem once per candidate.
    const QString base = i18n("New Folder");
    const QUrl dirUrl = currentUrl();
    const auto candidateUrl = [&dirUrl](const QString &n) {
        return FileOperationsManager::childUrl(dirUrl, n);
    };

    QString name = base;
    for (int counter = 1; m_model->rowForUrl(candidateUrl(name)) >= 0; ++counter)
        name = i18n("%1 (%2)", base, counter);

    const QUrl dest = candidateUrl(name);

    // Once created, select the folder and start the inline rename on it.
    KJob *job = FileOperationsManager::instance()->mkdir(currentUrl(), name, window());
    if (job) {
        const QUrl dir = currentUrl();
        connect(job, &KJob::result, this, [this, dest, dir](KJob *j) {
            if (!j->error())
                awaitPendingUrls(dir, {dest}, PendingAction::SelectAndEdit);
        });
    }
}

void Panel::createNewFile()
{
    // Re-prompt until the name is usable, keeping what the user typed so they
    // only have to correct it.
    QString name = i18n("new_file");
    QString prompt = i18n("File name:");
    QUrl dest;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, i18n("New File"), prompt,
                                     QLineEdit::Normal, name, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        const QString problem = newEntryNameProblem(name, dest);
        if (problem.isEmpty())
            break;
        prompt = problem + QLatin1Char('\n') + i18n("File name:");
    }

    // Any other failure (permission denied, read-only mount) is reported by
    // the standard KIO error dialog.
    KJob *job = FileOperationsManager::instance()->createEmptyFile(dest, window());
    const QUrl dir = currentUrl();
    connect(job, &KJob::result, this, [this, dest, dir](KJob *j) {
        if (!j->error())
            awaitPendingUrls(dir, {dest}, PendingAction::Select);
    });
}

// ── Context menu ──────────────────────────────────────────────────────────────

void Panel::showContextMenu(QObject *watched, const QPoint &pos)
{
    auto *view = static_cast<QAbstractItemView *>(watched);
    const QModelIndex idx = view->indexAt(pos);
    const QPoint globalPos = view->viewport()->mapToGlobal(pos);

    // Parent widget handed to KFileItemActions, and through it to every
    // KAbstractFileItemAction plugin. Plugins parent what they create to it —
    // Ark's Compress submenu, its actions, and the NameFinderJob/StatJob whose
    // results relabel and enable those actions — and KIO never takes ownership.
    // Left unset it was nullptr: Qt refused the plugins' connect() calls
    // ("invalid nullptr parameter"), so those results were dropped, and the
    // objects leaked on every right-click.
    //
    // Not `this`: everything would pile up until the Panel dies. Not the menu:
    // the "Compress to…" dialog would be parented to a hidden popup. A hidden
    // child of the Panel is destroyed with this scope, yet its window() is the
    // main window, so plugin dialogs still get a proper transient parent. Ark
    // shows that dialog with exec() from inside menu.exec(), and the archive job
    // it starts has no parent, so nothing outlives this scope that needs it.
    // Declared before the menu so the menu is destroyed first.
    QWidget pluginParent(this);

    if (!idx.isValid()) {
        if (m_errorWidget->isVisible())
            return;
        view->clearSelection();
        QMenu menu(this);
        buildEmptyContextMenu(menu, &pluginParent);
        menu.exec(globalPos);
    } else {
        if (!view->selectionModel()->isSelected(idx))
            view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        const KFileItemList items = selectedItems();
        if (items.isEmpty())
            return;
        QMenu menu(this);
        buildFileContextMenu(menu, items, &pluginParent);
        menu.exec(globalPos);
    }
}

void Panel::buildFileContextMenu(QMenu &menu, const KFileItemList &items, QWidget *pluginParent)
{
    const bool single = (items.size() == 1);
    const QList<QUrl> urls = selectedUrls();

    if (currentUrl().scheme() == QLatin1String("trash")) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("restoration")), i18n("Restore"), this,
                       [urls]() { KIO::restoreFromTrash(urls); });
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Permanently"),
                       this, &Panel::deleteSelectedPermanently);
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-properties")), i18n("Properties"),
                       this, [this, urls]() { KPropertiesDialog::showDialog(urls, this); });
        return;
    }

    auto *openAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                      i18n("Open"));
    connect(openAction, &QAction::triggered, this, &Panel::openSelected);

    auto *fileItemActions = new KFileItemActions(&menu);
    fileItemActions->setParentWidget(pluginParent);
    KFileItemListProperties props(items);
    fileItemActions->setItemListProperties(props);
    fileItemActions->insertOpenWithActionsTo(nullptr, &menu, {});

    if (single && items.first().isDir()) {
        const QUrl dirUrl = items.first().url();

        auto *newTabAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("tab-new")), i18n("Open in New Tab"));
        connect(newTabAction, &QAction::triggered, this, [this, dirUrl]() {
            newTab(dirUrl);
        });

        auto *newWindowAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("window-new")), i18n("Open in New Window"));
        connect(newWindowAction, &QAction::triggered, this, [this, dirUrl]() {
            if (auto *mw = qobject_cast<MainWindow *>(window()))
                mw->openInNewWindow(dirUrl);
        });

        auto *termAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("utilities-terminal")), i18n("Open Terminal Here"));
        connect(termAction, &QAction::triggered, this, [this, dirUrl]() {
            openTerminalAt(dirUrl.toLocalFile());
        });
        addOpenInActions(menu, VelaSettings::self()->folderOpenIn(), {dirUrl});
    }

    // Files get their own pinned list. Any selection with no folder in it
    // qualifies — the launcher passes every URL, so "Open in VS Code" on three
    // files opens all three.
    const bool filesOnly = std::none_of(items.cbegin(), items.cend(),
                                        [](const KFileItem &item) { return item.isDir(); });
    if (filesOnly)
        addOpenInActions(menu, VelaSettings::self()->fileOpenIn(), urls);

    // Search results (Ctrl+F, and the filter bar's subdirectory mode) come from
    // all over the tree, so the folder holding an item is generally not the
    // folder on screen. Offered only when the two actually differ: in a plain
    // listing these would just reopen the directory you are already in.
    if (single) {
        const QUrl itemUrl = items.first().url();
        const QUrl parentDir =
            itemUrl.adjusted(QUrl::RemoveFilename).adjusted(QUrl::StripTrailingSlash);
        const bool elsewhere = parentDir.isValid() && !parentDir.path().isEmpty()
                            && parentDir != currentUrl().adjusted(QUrl::StripTrailingSlash);
        if (elsewhere) {
            menu.addSeparator();

            auto *folderTabAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("tab-new")),
                i18n("Open Containing Folder in New Tab"));
            connect(folderTabAction, &QAction::triggered, this, [this, parentDir, itemUrl]() {
                // Armed before the tab is created: newTab() starts the listing
                // immediately, and navigateTo() deliberately preserves a pending
                // reveal, so the item is selected as soon as the folder lands.
                setPendingReveal(itemUrl);
                newTab(parentDir);
            });

            auto *folderWindowAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("window-new")),
                i18n("Open Containing Folder in New Window"));
            connect(folderWindowAction, &QAction::triggered, this, [this, parentDir, itemUrl]() {
                if (auto *mw = qobject_cast<MainWindow *>(window()))
                    mw->openInNewWindow(parentDir, itemUrl);
            });
        }
    }

    menu.addSeparator();

    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-cut")), i18n("Cut"),
                   this, &Panel::cutSelectedToClipboard);
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy"),
                   this, &Panel::copySelectedToClipboard);
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                   single ? i18n("Copy Absolute Path") : i18n("Copy Absolute Paths"),
                   this, [urls]() {
                       QStringList paths;
                       paths.reserve(urls.size());
                       for (const QUrl &u : urls)
                           paths << (u.isLocalFile() ? u.toLocalFile() : u.toString());
                       QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
                   });

    // Only for SSH-backed selections (sftp, and fish, KIO's older SSH
    // protocol): the path as the remote machine sees it, without scheme, user
    // or host. Other remote schemes (smb, trash, …) have no such path.
    const bool allSsh = std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &u) {
        return u.scheme() == QLatin1String("sftp") || u.scheme() == QLatin1String("fish");
    });
    if (allSsh) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                       single ? i18n("Copy Absolute Path on Remote Machine")
                              : i18n("Copy Absolute Paths on Remote Machine"),
                       this, [urls]() {
                           QStringList paths;
                           paths.reserve(urls.size());
                           for (const QUrl &u : urls)
                               paths << u.path();
                           QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
                       });
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-paste")), i18n("Paste"),
                   this, &Panel::pasteFromClipboard);

    menu.addSeparator();

    // Always offered: with several items selected, renameSelected() explains
    // that multi-item rename is not supported.
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename (F2)"),
                   this, &Panel::renameSelected);

    menu.addAction(QIcon::fromTheme(QStringLiteral("user-trash")), i18n("Move to Trash"),
                   this, &Panel::trashSelected);
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Permanently"),
                   this, &Panel::deleteSelectedPermanently);

    menu.addSeparator();
    fileItemActions->addActionsTo(&menu);
    menu.addSeparator();

    menu.addAction(QIcon::fromTheme(QStringLiteral("document-properties")), i18n("Properties"),
                   this, [this, urls]() {
                       KPropertiesDialog::showDialog(urls, this);
                   });
}

void Panel::buildEmptyContextMenu(QMenu &menu, QWidget *pluginParent)
{
    if (currentUrl().scheme() != QLatin1String("trash")) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18n("New Folder (F7)"),
                       this, &Panel::createNewFolder);
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-new")),
                       i18n("New Empty File (Ctrl+Alt+N)"), this, &Panel::createNewFile);
        menu.addAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")),
                       i18n("Open Terminal Here"), this, &Panel::openTerminalHere);

        // "Open With" for the folder currently being viewed — the same submenu a
        // folder offers when right-clicked in the item list, which is otherwise
        // unreachable for the open folder without navigating up to its parent.
        // The MIME type is stated rather than resolved: this URL is the directory
        // the panel is listing, so there is nothing to stat.
        {
            const KFileItem dirItem(currentUrl(), QStringLiteral("inode/directory"));
            auto *dirActions = new KFileItemActions(&menu);
            dirActions->setParentWidget(pluginParent);
            dirActions->setItemListProperties(KFileItemListProperties(KFileItemList{dirItem}));
            // before == nullptr appends at the current end of the menu, which is
            // here — grouped with Open Terminal Here, the other action that acts
            // on the folder itself rather than creating something in it.
            dirActions->insertOpenWithActionsTo(nullptr, &menu, {});
        }
        addOpenInActions(menu, VelaSettings::self()->folderOpenIn(), {currentUrl()});

        menu.addSeparator();

        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-paste")), i18n("Paste"),
                       this, &Panel::pasteFromClipboard);
    }

    menu.addSeparator();

    // Sort By submenu
    auto *sortMenu = menu.addMenu(i18n("Sort By"));
    {
        const int curCol = m_treeView->header()->sortIndicatorSection();
        const Qt::SortOrder curOrder = m_treeView->header()->sortIndicatorOrder();
        auto addSort = [&](const QString &label, int col) {
            auto *a = sortMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(curCol == col);
            connect(a, &QAction::triggered, this, [this, col, curCol, curOrder]() {
                const Qt::SortOrder order =
                    (col == curCol && curOrder == Qt::AscendingOrder)
                        ? Qt::DescendingOrder
                        : Qt::AscendingOrder;
                m_treeView->sortByColumn(col, order);
            });
        };
        addSort(i18n("Name"),          FileSystemModel::Name);
        addSort(i18n("Size"),          FileSystemModel::Size);
        addSort(i18n("Type"),          FileSystemModel::Type);
        addSort(i18n("Date Modified"), FileSystemModel::DateModified);
        addSort(i18n("Permissions"),   FileSystemModel::Permissions);
    }

    // View submenu
    auto *viewMenu = menu.addMenu(i18n("View"));
    {
        auto *hiddenAct = viewMenu->addAction(i18n("Show Hidden Files"));
        hiddenAct->setCheckable(true);
        hiddenAct->setChecked(showHidden());
        connect(hiddenAct, &QAction::toggled, this, &Panel::setShowHidden);

        viewMenu->addSeparator();
        auto addViewMode = [&](const QString &label, ViewMode vm) {
            auto *a = viewMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(m_viewMode == vm);
            connect(a, &QAction::triggered, this, [this, vm]() { setViewMode(vm); });
        };
        addViewMode(i18n("Details"),          Details);
        addViewMode(i18n("Medium Icons"),     MediumIcons);
        addViewMode(i18n("Large Icons"),      LargeIcons);
        addViewMode(i18n("Extra Large Icons"), ExtraLargeIcons);
    }

    menu.addSeparator();

    menu.addAction(QIcon::fromTheme(QStringLiteral("document-properties")),
                   i18n("Properties"), this, [this]() {
                       KPropertiesDialog::showDialog({currentUrl()}, this);
                   });
}

void Panel::setupFileOpShortcuts()
{
    auto addSc = [this](const QKeySequence &key, auto slot) {
        auto *sc = new QShortcut(key, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, slot);
    };

    addSc(Qt::Key_F2,                                  &Panel::renameSelected);
    addSc(Qt::Key_Delete,                              &Panel::trashSelected);
    addSc(Qt::SHIFT | Qt::Key_Delete,                  &Panel::deleteSelectedPermanently);
    addSc(QKeySequence::Copy,                          &Panel::copySelectedToClipboard);
    addSc(QKeySequence(Qt::CTRL | Qt::Key_X),          &Panel::cutSelectedToClipboard);
    addSc(QKeySequence::Paste,                         &Panel::pasteFromClipboard);
    addSc(Qt::Key_F7,                                  &Panel::createNewFolder);
    // Note: Ctrl+Alt+N, Ctrl+H, Ctrl+., Ctrl+L, Backspace, Ctrl+N, Ctrl+F
    // are registered in MainWindow's KActionCollection so they appear in the
    // shortcut editor and support multiple key bindings.
}
