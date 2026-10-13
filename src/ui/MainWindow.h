#pragma once

#include "DropZoneOverlay.h"
#include "Panel.h"

#include <KConfigGroup>
#include <KXmlGuiWindow>
#include <QMap>
#include <QPointer>

class CommandPalette;
class CommandRegistry;
class DropZoneOverlay;
class Panel;
class PanelContainer;
class PreviewPane;
class QAction;
class QLabel;
class QPoint;
class QSplitter;
class QToolButton;
class QVBoxLayout;
class SettingsDialog;
class Sidebar;
class TerminalPanel;
class ViewModeSlider;

class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // What a pending autosave needs to write. Geometry-only saves are triggered
    // by resize/move and must not rewrite every window's tab tree.
    enum class SaveScope { GeometryOnly, Full };

    explicit MainWindow(QWidget *parent = nullptr, bool tearOff = false);
    ~MainWindow() override;

    // Open command-line targets (folders, or files to reveal). When reuseActive
    // is true the first target loads into this window's active panel (used on the
    // initial launch); every other target spawns a new window.
    void openTargets(const QStringList &paths, bool reuseActive);

    // ── App-lifetime D-Bus entry points ───────────────────────────────────────
    // Static, so they route to a currently-live window instead of a captured one
    // and keep working after any individual window (including the first) closes.
    static void openInNewWindow(const QUrl &folder, const QUrl &fileToReveal = {});
    // One new window, one tab per target, in order; the first tab is shown.
    struct FolderTarget {
        QUrl folder;
        QList<QUrl> reveal; // items to select in that folder
    };
    static void openTabsInNewWindow(const QList<FolderTarget> &targets);
    static void openTargetsInNewWindows(const QStringList &paths); // each in a new window
    static void raiseExisting();                                    // raise a live window
    // Any currently-live window, or nullptr. For parenting dialogs.
    static QWidget *anyLiveWindow();

    // ── Drag-and-drop target API ──────────────────────────────────────────────
    // Called by the dragging window on whichever MainWindow the cursor is over.
    void showDragOverlays();
    void hideDragOverlays();
    void updateDragOverlayHover(const QPoint &globalPos);
    DropZoneOverlay *dragOverlayAt(const QPoint &globalPos) const;
    void acceptTabDrop(const TabContext &ctx, DropZoneOverlay::DropZone zone, Panel *target);


    // Frameless edge resize for a mouse move/press on `watched`, one of this
    // window's widgets. Called by the shared application event filter; true
    // when the event was consumed (a resize started).
    bool handleEdgeEvent(QWidget *watched, QMouseEvent *me);

protected:
    void closeEvent(QCloseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;

public Q_SLOTS:
    void raise();               // show + raise + activate this window (shadows QWidget::raise)
    void navigateTo(const QUrl &url);

private Q_SLOTS:
    void goBack();
    void goForward();
    void goUp();
    void goHome();
    void refresh();
    void onPanelActivated();
    void updateActions();
    void showShortcutsDialog();
    // Copies `changed` actions' shortcuts onto the same actions in every
    // other window.
    void propagateShortcuts(const QList<QAction *> &changed);
    // Edit ▸ Undo, after the user confirms.
    void undoLastOperation();
    void showSettingsDialog();
    void applySettings();
    // Re-applies settings in *every* open window. Settings are global, but the
    // dialog's signal only ever reached the window that opened it.
    static void applySettingsToAllWindows();

    void onTabDragStarted(Panel *source, int tabIndex);
    void updateWindowTitle();
    static void renumberWindows();

private:
    void setupUI();
    void setupActions();
    void applyHeaderStyle(QWidget *header);
    void applyWindowMask();
#ifndef NDEBUG
    void onCursorInfoTick();
#endif
    void populateCommandRegistry();
    void updateOperationsActions();
    void saveSettings();
    void restoreSettings();

    // Per-window group-based save/restore (used by session management).
    void saveToGroup(KConfigGroup &group) const;
    void restoreFromGroup(const KConfigGroup &group);

    // Save all open windows to the Session config group.
    static void saveSession(SaveScope scope = SaveScope::Full);
    // Debounced autosave: (re)start a 5s timer; when it settles, saveSession().
    // Triggered by navigation so an unclean exit (crash/kill) doesn't lose tabs.
    // The timer only counts down while the application is focused (see
    // onAppFocusChanged); a Full request always wins over a pending GeometryOnly.
    static void scheduleSessionSave(SaveScope scope = SaveScope::Full);
    // Pauses/resumes the autosave countdown with application focus.
    static void onAppFocusChanged(QWidget *old, QWidget *now);

    void saveGeometryToGroup(KConfigGroup &group) const;
    // Restore all windows from the Session config group into this window (Window_0)
    // and create additional windows as needed. Returns true if session existed.
    bool restoreSession();

    // Moves every tab of every panel in this window off `mountpoint`.
    void relocatePanelsOffMount(const QString &mountpoint);

    Panel *activePanel() const;
    // What the window's folder actions (Back, Up, F5, New Folder, Ctrl+H, …)
    // act on: the preview's folder view holding focus, else the active panel.
    // Tab actions and the view-mode slider stay with the active panel.
    Panel *actionPanel() const;
    Panel *otherPanel() const;
    void setActivePanel(Panel *panel);

    // The preview column (Space). One per window, right of every panel,
    // following the active panel. Opening takes half the panels' width the
    // first time and the last dragged width after that.
    void togglePreview();
    void setPreviewVisible(bool visible, int width = -1);

    // The terminal below the panels (F4). One per window, following the active
    // panel. Hiding it ends its shell. `height` < 0 uses the remembered height,
    // or a third of the panels column the first time.
    void setTerminalVisible(bool visible, int height = -1);

    static MainWindow *windowUnderCursor();

    QSplitter       *m_outerSplitter = nullptr;
    Sidebar         *m_sidebar       = nullptr;
    Panel           *m_leftPanel     = nullptr;
    Panel           *m_activePanel   = nullptr;
    PanelContainer  *m_panelRoot     = nullptr;
    PreviewPane     *m_preview       = nullptr;
    QSplitter       *m_terminalSplitter = nullptr; // panels above, terminal below
    TerminalPanel   *m_terminal      = nullptr;
    int              m_terminalHeight = -1;        // last height while shown

    // ── Tab drag state ────────────────────────────────────────────────────────
    QList<DropZoneOverlay *> m_dragOverlays;   // overlays shown in THIS window as a drop target
    QPointer<Panel>      m_dragSourcePanel;
    int                  m_dragTabIndex = -1;
    TabContext           m_dragTabCtx;          // tab taken from source at drag-start
    QPointer<MainWindow> m_hoveredWindow;       // window currently under the floating tab

    void connectPanel(Panel *panel);
    void cleanupDrag();

    // Tab-bar-in-header management (issue 5: split panels share the top bar).
    void extractTabBarIntoHeader(Panel *panel);
    void returnTabBarToPanel(Panel *panel);
    void repositionTabBars();
    void repositionStatusBars();
    void renumberPanels();

    // Split out of repositionTabBars(), which had grown to one 165-line
    // function doing three unrelated jobs.
    QToolButton *createAddTabButton(Panel *panel, QWidget *parent);
    void syncTabBarOwnership(const QList<Panel *> &headerPanels);
    void layOutHeaderTabBars(const QList<Panel *> &headerPanels);
    void moveTabBarToHeader(Panel *panel);
    void moveTabBarIntoPanel(Panel *panel);

    // Poll-based drag tracking — Wayland-safe (QCursor::pos / QApplication::mouseButtons
    // work globally regardless of which surface has pointer focus).
    void startDragPoll();
    void stopDragPoll();
    void onDragPoll();
    QTimer *m_dragPollTimer = nullptr;

    QWidget     *m_titleBar            = nullptr;
    QWidget     *m_sidebarHeader       = nullptr;
    QWidget     *m_sidebarHeaderSpacer = nullptr;
    QWidget     *m_tabBarArea          = nullptr;    // manual-geometry container for all tab bars
    QMap<Panel *, QToolButton *> m_addTabBtns;       // per-panel '+' buttons inside m_tabBarArea
    QMap<Panel *, QWidget *>     m_embeddedTabRows;  // container rows for vertically-split panels
    QToolButton *m_maxBtn              = nullptr;
    QToolButton *m_velaBtn          = nullptr;
    QWidget     *m_centralContainer   = nullptr;
    QWidget     *m_wireframeOverlay   = nullptr;
#ifndef NDEBUG
    QLabel      *m_cursorInfoLabel  = nullptr;
#endif

    // Bottom status bar (mirrors tab-bar-in-header approach)
    QWidget *m_statusBarArea    = nullptr;  // manual-geometry container for per-column labels
    QWidget *m_statusBarSpacer  = nullptr;  // fixed-width spacer matching sidebar width
    QMap<Panel *, QLabel *> m_bottomStatusLabels;  // bottom-of-column panel → status label
#ifndef NDEBUG
    QTimer      *m_cursorInfoTimer  = nullptr;
#endif
    int          m_windowNumber     = 0;
    bool         m_cursorInside     = false;

    static constexpr int kResizeMargin = 6; // px from edge that triggers resize
    Qt::Edges resizeEdgesAt(const QPoint &localPos) const;
    static void installEdgeResizeFilter();

    QAction *m_actionBack            = nullptr;
    QAction *m_actionForward         = nullptr;
    QAction *m_actionUp              = nullptr;
    QAction *m_actionHome            = nullptr;
    QAction *m_actionRefresh         = nullptr;
    QAction *m_actionNewTab          = nullptr;
    QAction *m_actionCloseTab        = nullptr;
    QAction *m_actionNextTab         = nullptr;
    QAction *m_actionPrevTab         = nullptr;
    QAction *m_actionCopyToOther     = nullptr;
    QAction *m_actionMoveToOther     = nullptr;
    QAction *m_actionNewFolder       = nullptr;
    QAction *m_actionUndo            = nullptr;
    QAction *m_actionToggleSidebar   = nullptr;
    QAction *m_actionToggleTerminal  = nullptr;
    QAction *m_actionFocusTerminal   = nullptr;
    QAction *m_actionCommandPalette  = nullptr;
    // View ▸ Show Operations: the only way back to the Operations window once
    // it is running in the background. Disabled while nothing is listed.
    QAction *m_actionShowOperations  = nullptr;
    // Toolbar button with the running count, visible only while jobs run.
    QAction *m_actionOperationsIndicator = nullptr;
    QToolButton *m_operationsButton  = nullptr;

    // Owned per-window: these actions belong to this window and die with it.
    CommandRegistry *m_commandRegistry = nullptr;
    CommandPalette  *m_commandPalette  = nullptr;
    SettingsDialog  *m_settingsDialog  = nullptr;
    ViewModeSlider  *m_viewModeSlider  = nullptr;
};
