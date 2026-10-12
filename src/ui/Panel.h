#pragma once

#include "NavigationHistory.h"
#include "core/FileSystemModel.h"
#include "core/SearchEngine.h"
#include "core/VelaSettings.h"

#include <KConfigGroup>
#include <KFileItem>
#include <QElapsedTimer>
#include <QList>
#include <QSet>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>

class BreadcrumbBar;
class FileFilterProxyModel;
class FileSystemModel;
class FilterBar;
class KMessageWidget;
class MatchHighlightDelegate;
class QAbstractItemModel;
class QAbstractItemView;
class QDropEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;
class FileTreeView;
class QTreeView;
class SearchResultsModel;
class ThumbnailLoader;

// What a tab looked like when it was last left. Heavy (a selection can be a
// whole directory's worth of URLs, search results a whole tree's), so only the
// Panel::kMaxTabsWithViewState most recently left tabs keep one.
struct TabViewState {
    QList<QUrl> selection;
    QUrl        current;
    int         scrollX = 0;
    int         scrollY = 0;
    // Subfolder search results, when the tab was showing them and the search
    // had finished. An unfinished search is re-run instead.
    bool          searchCached = false;
    KFileItemList searchResults;
};

struct TabContext {
    // The tab's location is history.current() — there is deliberately no
    // separate url member (it used to be one, updated alongside the history on
    // every navigation: two sources of truth that could drift).
    NavigationHistory history;
    int viewMode = 0; // Panel::ViewMode, int to avoid circular dependency
    // Filter bar, kept for every tab (a few strings): switching tabs used to
    // carry one tab's filter over to the next. A non-empty searchTerm means the
    // tab was showing subfolder-search results for it.
    bool    filterVisible = false;
    QString filterText;
    bool    filterSubdirs = false;
    bool    filterCrossFs = false;
    QString searchTerm;
    std::optional<TabViewState> viewState;
    quint64 id = nextId(); // identity for the recently-left list; survives reordering

    static quint64 nextId() { static quint64 n = 0; return ++n; }
    // Items to select once this tab first loads (a D-Bus "reveal these files").
    // Consumed when the tab is switched to, so a tab opened in the background
    // keeps its reveal until it is actually shown.
    QList<QUrl> reveal;

    TabContext() = default;
    explicit TabContext(const QUrl &u, int mode = 0) : viewMode(mode)
    {
        if (u.isValid())
            history.push(u);
    }
};

class Panel : public QWidget
{
    Q_OBJECT

public:
    // Above this many rows a directory counts as "large": the status line stops
    // tracking the selection live and the loading indicator gets to appear.
    // Below it, everything is cheap enough to stay instant.
    static constexpr int kLargeDirectoryThreshold = 512;

    // Width of the strip kept free at the right end of the details view. It
    // belongs to no column, so a click there lands on the directory rather than
    // on a row. The last column stretches into everything left of it.
    static constexpr int kTrailingDeadZone = 48;

    // How many recently left tabs keep their full view state (scroll,
    // selection, current item, search results). Older tabs keep only their
    // filter text and search term: selections and result lists can each be a
    // whole directory tree's worth of URLs.
    static constexpr int kMaxTabsWithViewState = 5;

    // Details is the tree view; everything else is the QListView in IconMode.
    // There is deliberately no plain "List" mode — Details covers that layout.
    enum ViewMode { Details = 0, MediumIcons = 1, LargeIcons = 2, ExtraLargeIcons = 3 };

    explicit Panel(QWidget *parent = nullptr);

    // ── Embedded mode ─────────────────────────────────────────────────────────
    // A panel living inside the preview column: one tab at `url`, no tab bar,
    // address row, status line or active strip — the file view and everything
    // it does (menus, shortcuts, rename, drag & drop, filter) otherwise as in a
    // tab. Whatever would open a new tab opens it in `tabHost()` instead (the
    // panel the preview follows), and nothing is written to the global column
    // defaults. Not part of the window's panel tree: no session, no activation.
    // Empty until startEmbedded(), so the caller can connect to it first.
    static Panel *createEmbedded(std::function<Panel *()> tabHost, QWidget *parent);
    // Opens the embedded panel's one tab at `url`; `reveal`, when valid, is
    // selected once the folder has listed.
    void startEmbedded(const QUrl &url, const QUrl &reveal);
    bool isEmbedded() const { return m_embedded; }

    // Navigation
    void navigateTo(const QUrl &url);
    void navigateAndReveal(const QUrl &folder, const QUrl &fileToReveal);
    // Arm the post-load reveal without triggering navigation, for callers that
    // are about to load the directory themselves (e.g. a freshly created window
    // inserting its first tab).
    void setPendingReveal(const QUrl &fileToReveal);
    void setPendingReveal(const QList<QUrl> &filesToReveal);
    QUrl currentUrl() const;
    void goBack();
    void goForward();
    void goUp();
    bool canGoBack() const;
    bool canGoForward() const;

    // Tabs
    void newTab(const QUrl &url);
    void closeTab(int index);
    void closeOtherTabs(int keepIndex);
    // Moves every tab showing `root` or anything below it to `fallback` — the
    // current one by a real navigation, the others by rewriting their
    // location. Used before a device is unmounted and after it disappears.
    void relocateTabsUnder(const QUrl &root, const QUrl &fallback);
    void duplicateTab(int index);
    void switchToTab(int index);
    int currentTabIndex() const;
    int tabCount() const;

    // View
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const;

    // Folder grouping
    bool foldersFirst() const;
    void setFoldersFirst(bool first);

    // Folder size analysis
    bool folderSizeCalcEnabled() const { return m_folderSizeCalcEnabled; }
    void setFolderSizeCalcEnabled(bool enabled);

    // Item counts (updated after each directory load / selection change)
    int folderCount() const { return m_folderCount; }
    int fileCount()   const { return m_fileCount; }

    // Active state
    void setActive(bool active);
    bool isActive() const;

    // Re-checks that the displayed directory still exists, raising the standard
    // "does not exist" banner if it doesn't. The model watches the directory
    // live, but inotify cannot see everything (a moved-away parent, a remote
    // location), so this is called at the moments the panel comes back into
    // the user's attention: window activation, panel focus, tab/dir switch.
    void revalidateDirectory();

    // Persistence
    void saveSettings(const QString &groupName) const;
    void restoreSettings(const QString &groupName);
    void saveSettings(KConfigGroup &group) const;
    void restoreSettings(KConfigGroup &group);
    void applyColumnSettings();
    // Publishes this panel's column visibility as the global default for new
    // windows. Called only when the user actually toggles a column.
    void saveColumnVisibilityGlobally() const;
    // The same, for column widths. Written when the user drags a divider, and
    // read by every freshly created panel — a new window, a split, a tear-off —
    // none of which run session restore.
    void saveColumnWidthsGlobally() const;

    FileSystemModel *model() const;

    // File operations
    QList<QUrl> selectedUrls() const;
    KFileItemList selectedItems() const;
    // The item the preview pane should show: the selected one when exactly
    // one row is selected, null otherwise. `count` receives the selected row
    // count either way. Counts from the selection ranges, so a Ctrl+A over a
    // huge directory does not materialise every index just to learn it is
    // not exactly one.
    KFileItem soleSelectedItem(int &count) const;
    void openSelected();
    // What activating `item` in the view does: enter a folder, open a file
    // with its default application.
    void openItem(const KFileItem &item);
    // Right-click on `item` from outside the view (the preview): selects it,
    // as a right-click in the view would, then shows the same menu. With a
    // null item, the folder's background menu. With `hostMenu`, the item's
    // menu is appended to it as a submenu and `hostMenu` is shown instead.
    // Nothing for an item no longer listed here (hostMenu still shows).
    void showItemContextMenu(const KFileItem &item, const QPoint &globalPos,
                             QMenu *hostMenu = nullptr);
    void clearSelection();
    // Keyboard focus to the file view.
    void focusView();
    void renameSelected();
    void trashSelected();
    void deleteSelectedPermanently();
    void copySelectedToClipboard();
    void cutSelectedToClipboard();
    void pasteFromClipboard();
    void createNewFolder();
    void createNewFile();
    void openTerminalHere();
    void openTerminalAt(const QString &path); // launches the configured terminal in `path`
    // Appends the pinned "Open in <app>" entries for `urls` — flat ones directly,
    // the rest under one "Open In" submenu. Shared by the folder item menu, the
    // background menu (for the open folder) and the file item menu.
    void addOpenInActions(QMenu &menu, const QList<VelaSettings::OpenInApp> &apps,
                          const QList<QUrl> &urls);

    // Search & filter
    // Ctrl+F: the filter bar in "search subfolders" mode.
    void showSubfolderSearch();
    void clearFilter();

    // Keyboard shortcut targets
    void focusAddressBar();
    void toggleHidden();
    void goHome();
    void goRefresh();

    QTabBar *tabBar() const { return m_tabBar; }

    int  panelNumber() const              { return m_panelNumber; }
    void setPanelNumber(int n)            { m_panelNumber = n; }
    // Allow dragging the last remaining tab (safe when other panels exist).
    void setDragLastTabAllowed(bool allow){ m_dragLastTabAllowed = allow; }

    // Creates "New Folder" (auto-incremented), selects it, starts inline rename.
    void createNewFolderAutoNamed();

    // Live-apply appearance settings (called from MainWindow::applySettings).
    void applyAppearanceSettings();

    // Called by MainWindow to hide the per-panel status bar when the global
    // bottom bar takes over for bottom-of-column panels.
    void setStatusBarVisible(bool visible);
    QString statusText() const;

    // Extract a tab's context (for drag/tear-off). Caller takes ownership.
    TabContext takeTab(int index);
    // Insert a tab context at the end, and switch to it unless `activate` is
    // false (the tab then loads when first switched to).
    void insertTab(const TabContext &ctx, bool activate = true);
    // Remove all tabs without emitting closeRequested (used when repopulating).
    void clearAllTabs();
    // Cancel an in-progress tab drag (called by MainWindow when tearing off).
    void cancelDrag();

Q_SIGNALS:
    void urlChanged(const QUrl &url);
    // A navigation's first successful listing (not re-lists, not failures).
    void directoryVisited(const QUrl &url);
    void activated();
    void navigationStateChanged();
    void statusChanged(const QString &text);
    void columnVisibilityChanged();
    void viewModeChanged(int mode);
    void itemCountsChanged(int folders, int files);
    void folderSizeCalcChanged(bool enabled);
    // Tab drag lifecycle (manual tracking — no QDrag).
    void tabDragStarted(Panel *sourcePanel, int tabIndex);
    // Emitted when the user tries to close the panel's last remaining tab.
    void closeRequested();
    void tabCountChanged();
    // Space on a file view. The window owns the preview column, so the panel
    // only reports the request.
    void previewToggleRequested();
    // Whatever the preview should be showing for this panel may have changed:
    // selection, tab, directory, search/normal mode.
    void previewItemChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void onItemActivated(const QModelIndex &index);
    void onDirectoryLoaded(const QUrl &url);
    void onSelectionChanged();
    void onHeaderContextMenu(const QPoint &pos);
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onTabContextMenu(const QPoint &pos);

    void onFilterChanged(const QString &pattern);
    void onFilterDismissed();
    void startSubfolderSearch(const QString &term);
    // Search mode showing `items` for `term` without running the engine
    // (cached results of a tab being switched back to).
    void showCachedSearch(const QString &term, const KFileItemList &items);
    void onSearchResultsBatch(const KFileItemList &items);
    void onLockClicked(int section);
    void onFilterSubdirToggled(bool on);

private:
    void loadUrl(const QUrl &url);
    // Re-lists the directory if `job` (a trash/delete whose rows were removed
    // optimistically) fails, so files it did not remove reappear.
    void reconcileAfterFailure(KJob *job);
    // ── Per-tab view state ────────────────────────────────────────────────────
    // Records how the current tab looks (filter, search, and — for the most
    // recently left tabs — scroll, selection and results) before leaving it.
    void saveTabState(int index);
    // Puts `id` at the front of the recently-left list, dropping the full
    // view state of whichever tab falls off the end.
    void rememberViewState(quint64 id);
    // Resets the filter bar and any search without the side effects of the
    // user doing it (no debounce, no new search).
    void resetFilterQuietly();
    // Applied once the switched-to tab's directory has loaded.
    void applyPendingTabRestore();
    void applyViewState(const TabViewState &state);

    // Drops `urls` from whichever model is on screen ahead of `job` (a trash or
    // delete), and restores what survives if the job fails.
    void removeOptimistically(const QList<QUrl> &urls, KJob *job);
    // Validation shared by New Folder and New File. See the definition.
    QString newEntryNameProblem(const QString &name, QUrl &dest) const;
    void showFilterBar(bool withSubdirs);
    void applyListViewMode();
    void updateStatusBar();
    void updateAddressBar();
    void updateTabTitle(int index);
    QString tabTitleForUrl(const QUrl &url) const;
    TabContext &currentTab();
    const TabContext &currentTab() const;
    QAbstractItemView *currentView() const;
    void handleDrop(QDropEvent *event, QObject *targetView);
    // Modal confirmation for a drop. Always shown — see the definition.
    bool confirmDrop(const QList<QUrl> &sources, const QUrl &dest, bool copy) const;
    void setupFileOpShortcuts();
    void showContextMenu(QObject *watched, const QPoint &pos);
    void buildFileContextMenu(QMenu &menu, const KFileItemList &items, QWidget *pluginParent);
    void buildEmptyContextMenu(QMenu &menu, QWidget *pluginParent);

    // Returns the active model (proxy in normal mode, search results in search mode)
    QAbstractItemModel *activeModel() const;
    // Reconnects selection model signals after switching the view model
    void setActiveViewModel(QAbstractItemModel *model);
    void exitSearchMode();
    // Selects `url`'s row alone and scrolls to it. False when it is not listed.
    bool selectUrl(const QUrl &url);
    void updateHighlightDelegate();
    void startEditingAdjacentItem(int direction); // +1 next, -1 previous

    // Rows currently selected, de-duplicated, column 0 only. Cheaper and simpler
    // than filtering selectedIndexes(), which returns one index per column.
    QModelIndexList selectedRowIndexes() const;

    // Item counts are a property of the model, not the selection, so they are
    // recomputed only when rows change — not on every arrow-key press.
    void recountItems();
    // Status text depends on both; cheap, uses the cached counts.
    void refreshStatusText();
    // Selected row count and their total bytes, computed from the selection
    // ranges rather than from a materialised index list. See the definition.
    std::pair<int, KIO::filesize_t> selectionSummary() const;

    // Loading indicator, shown only once a listing turns out to be large.
    void onLoadingProgress(int rowCount);
    void stopLoadingIndicator();

    // Grows (or shrinks) the last visible column so the columns end exactly one
    // dead-zone short of the right edge. Replaces QHeaderView's own
    // stretchLastSection, which always runs to the edge and leaves nothing to
    // click on.
    void stretchLastColumn();

    // Seeds this panel's header from the global widths. Runs once per panel:
    // re-running it on every directory load would undo a resize the user made
    // in this window.
    void applyGlobalColumnWidths();

    // What to do with m_pendingUrls once the directory finishes loading.
    enum class PendingAction {
        None,
        Select,      // select them all, scroll to the first
        SelectAndEdit, // select the first and start an inline rename on it
    };
    // Single post-load resolution pass, replacing three separate full-model scans.
    void applyPendingSelection();
    // Create/paste: select `urls` in `dir` when the watcher delivers them.
    void awaitPendingUrls(const QUrl &dir, const QList<QUrl> &urls, PendingAction action);
    bool resolvePendingIfListed(); // true when nothing is left pending

    // ── Thumbnails ────────────────────────────────────────────────────────────
    // Thumbnail work is driven entirely by what is on screen: the visible row
    // range is recomputed (debounced) on scroll, resize, view-mode change and
    // model change, and handed to the loader, which drops anything previously
    // queued. Scrolling through a large photo directory therefore never queues
    // more than roughly a screenful at a time.
    void scheduleThumbnailUpdate();
    void updateVisibleThumbnails();
    // Rows worth generating thumbnails for. O(1) from the grid geometry in the
    // icon modes; falls back to probing the viewport when the layout can't be
    // reasoned about. Returns false when nothing is laid out yet.
    bool visibleRowRange(int rowCount, int &firstRow, int &lastRow) const;
    // Keeps loader size / model enablement in step with the current view mode.
    void applyThumbnailViewMode();

    QTabBar        *m_tabBar          = nullptr;
    QFrame         *m_activeIndicator = nullptr;
    BreadcrumbBar  *m_addressBar      = nullptr;
    QToolButton    *m_btnBack         = nullptr;
    QToolButton    *m_btnForward      = nullptr;
    QToolButton    *m_btnUp           = nullptr;
    QToolButton    *m_btnRefresh      = nullptr;
    QToolButton    *m_btnEmptyTrash   = nullptr;
    KMessageWidget *m_errorWidget     = nullptr;
    QStackedWidget *m_stack           = nullptr;
    FileTreeView   *m_treeView        = nullptr;
    QListView      *m_listView        = nullptr;
    QLabel         *m_emptyDirLabel   = nullptr;
    QLabel         *m_statusBar       = nullptr;

    FileSystemModel *m_model = nullptr;
    FileFilterProxyModel *m_proxyModel = nullptr;
    SearchResultsModel *m_searchResultsModel = nullptr;
    // Search results go through their own proxy so the details view stays
    // sortable in search mode (the models are column-compatible now).
    FileFilterProxyModel *m_searchProxyModel = nullptr;
    SearchEngine *m_searchEngine = nullptr;
    FilterBar *m_filterBar = nullptr;
    MatchHighlightDelegate *m_delegate = nullptr;
    ThumbnailLoader *m_thumbLoader = nullptr;

    QTimer *m_filterDebounce     = nullptr;
    // Re-lists if created/pasted items have not been delivered by the watcher.
    QTimer *m_pendingFallback    = nullptr;
    QTimer *m_thumbDebounce      = nullptr;
    // Coalesces status-line updates while a selection is still changing.
    QTimer *m_statusDebounce     = nullptr;
    // Drives the status-line loading animation, and with it the running item
    // count, while a large directory streams in.
    QTimer *m_loadSpinner        = nullptr;
    // Coalesces the global width write while a divider is being dragged.
    QTimer *m_columnWidthSave    = nullptr;
    // True while the code (not the user) is resizing sections, so that restoring
    // or hiding a column is never mistaken for a deliberate resize.
    bool m_applyingColumnWidths  = false;
    bool m_globalWidthsApplied   = false;
    int  m_spinnerFrame          = 0;
    bool m_loadingIndicator      = false;
    // Last computed selection summary. The loading animation republishes the
    // status line eight times a second, and re-summing a Ctrl+A over a large
    // directory at that rate would cost more than the listing itself.
    int             m_cachedSelCount = 0;
    KIO::filesize_t m_cachedSelSize  = 0;
    bool            m_selectionDirty = true;
    // How long the currently-pending thumbnail pass has been deferred. The
    // debounce restarts on every trigger, and Batched layout emits them faster
    // than the interval, so without a ceiling the pass is starved for the whole
    // layout of a large directory.
    QElapsedTimer m_thumbPendingSince;
    int  m_panelNumber           = 0;
    bool m_dragLastTabAllowed    = false;
    int  m_folderCount           = 0;
    int  m_fileCount             = 0;
    bool m_folderSizeCalcEnabled = false;

    QList<TabContext> m_tabs;
    int m_currentTab = -1;
    // Tab drag tracking (manual, no QDrag)
    QPoint m_dragStartPos;
    int m_dragTabIndex = -1;
    bool m_dragging = false;
    ViewMode m_viewMode = Details;
    int m_lastSortCol = FileSystemModel::Name;
    bool m_applyingLockedSort = false;
    // Directory the locked/default sort was last applied for. It is applied
    // once per navigation, not on every full re-list: create/paste re-list the
    // same directory and must not undo a sort the user just picked.
    QUrl m_lockedSortAppliedFor;
    // Ctrl+wheel view switching accumulates angle delta until a full notch
    // (120), so a touchpad's stream of small deltas moves one mode, not all.
    int m_zoomWheelAccum = 0;
    bool m_active = false;
    bool m_inSearchMode = false;
    bool m_embedded = false;
    std::function<Panel *()> m_tabHost; // embedded only
    QWidget *m_addressRow = nullptr;
    // -1 = never applied; otherwise the show-hidden default this panel last saw.
    // Lets applyAppearanceSettings honour the default only when it actually
    // changes, instead of clobbering a manual Ctrl+H toggle on every Apply.
    int m_appliedShowHiddenDefault = -1;
    // Last column-visibility set pushed to the header, so repeated calls from
    // modelReset / onDirectoryLoaded become no-ops instead of config reads.
    QSet<int> m_appliedHiddenColumns;
    bool      m_appliedHiddenColumnsValid = false;
    QMetaObject::Connection m_singleClickTreeConn;
    QMetaObject::Connection m_singleClickListConn;

    // One mechanism for "after the directory loads, do X to these URLs" —
    // used by search-result navigation, create/paste, and Ctrl+N rename alike.
    QList<QUrl>   m_pendingUrls;
    PendingAction m_pendingAction = PendingAction::None;

    int m_searchResultCount = 0;
    // Bumped whenever the search results are replaced or dropped, so late
    // callbacks can tell they belong to a search that is no longer shown.
    int m_searchSession = 0;

    // Tab ids holding a full TabViewState, most recently left first.
    QList<quint64> m_recentTabIds;
    struct PendingTabRestore {
        QUrl    url; // applied only if the load that completes is this one
        bool    filterVisible = false;
        QString filterText;
        bool    filterSubdirs = false;
        bool    filterCrossFs = false;
        QString searchTerm;
        std::optional<TabViewState> state;
    };
    std::optional<PendingTabRestore> m_pendingTabRestore;
    // Selection/scroll to apply when a re-run search finishes.
    std::optional<TabViewState> m_pendingSearchViewState;
    // Bumped per restore, so a scroll retry from an older one stands down.
    int m_restoreGeneration = 0;
};
