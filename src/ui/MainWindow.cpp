#include "MainWindow.h"

#include "CommandPalette.h"
#include "DropZoneOverlay.h"
#include "Panel.h"
#include "PanelContainer.h"
#include "preview/PreviewPane.h"
#include "MessageBoxUtil.h"
#include "MountWatcher.h"
#include "SettingsDialog.h"
#include "Sidebar.h"
#include "TerminalPanel.h"
#include "ViewModeSlider.h"
#include "core/CommandRegistry.h"
#include "core/FileItemModelBase.h"
#include "core/FileOperationsManager.h"
#include "core/FileSystemModel.h"
#include "core/VelaSettings.h"
#include "OperationsWindow.h"

#include <KActionCollection>
#include <KIO/FileUndoManager>
#include <KColorSchemeManager>
#include <KColorSchemeModel>
#include <KGlobalAccel>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KShortcutsDialog>
#include <KStandardAction>
#include <KToolBar>
#include <KConfigGroup>
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QSize>
#include <QToolButton>
#include <QStatusBar>
#include <QWindow>
#include <QWidgetAction>
#include <QCloseEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <utility>
#include <QSplitter>
#include <QSet>
#include <QTimer>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>

class WireframeOverlay : public QWidget
{
public:
    explicit WireframeOverlay(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(parent->rect());
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        VelaSettings *s = VelaSettings::self();
        if (!s->wireframeEnabled())
            return;
        const qreal t    = s->wireframeThickness();
        const qreal half = t / 2.0;
        QColor color(s->wireframeColor());
        if (!color.isValid())
            color = QColor(QStringLiteral("#399CD0"));
        constexpr qreal r = 6.0;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(color, t);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(half, half, -half, -half),
                                qMax(0.0, r - half), qMax(0.0, r - half));
    }
};

static QList<MainWindow *> s_allWindows;
static bool s_restartingForTheme = false;
static int  s_appliedThemeMode   = -1; // -1 = not yet applied
static QTimer *s_sessionSaveTimer = nullptr; // debounced autosave (see scheduleSessionSave)
// What the pending autosave must write. Upgraded to Full by structural changes,
// reset after each save. Declared here so the file-static timer lambda sees it.
static MainWindow::SaveScope s_pendingSaveScope = MainWindow::SaveScope::GeometryOnly;

void MainWindow::renumberWindows()
{
    for (int i = 0; i < s_allWindows.size(); ++i) {
        s_allWindows[i]->m_windowNumber = i + 1;
        s_allWindows[i]->updateWindowTitle();
    }
}

MainWindow::MainWindow(QWidget *parent, bool tearOff)
    : KXmlGuiWindow(parent)
{
    // Hook the autosave countdown to application focus once, on first window.
    if (s_allWindows.isEmpty()) {
        QObject::connect(qApp, &QApplication::focusChanged,
                         qApp, &MainWindow::onAppFocusChanged);
    }
    s_allWindows.append(this);
    renumberWindows();  // sets m_windowNumber and title
    setMinimumSize(1000, 480);
    resize(1200, 700);

    setupActions();
    setupUI();
    setHelpMenuEnabled(false);
    setupGUI(Default, QStringLiteral("velaui.rc"));
    menuBar()->hide();
    statusBar()->hide();  // KXmlGuiWindow creates a status bar by default; we use our own

    // Hide the KXmlGui toolbar — our custom title bar takes its place.
    if (KToolBar *tb = toolBar(QStringLiteral("mainToolBar")))
        tb->hide();

    // Command registry + palette — created after setupGUI so all KDE standard
    // actions exist. Both are children of this window, and the registry only
    // ever holds this window's actions, so closing a window can't leave another
    // window's palette pointing at freed QActions.
    m_commandRegistry = new CommandRegistry(this);
    m_commandPalette = new CommandPalette(m_commandRegistry, actionCollection(), this);
    connect(m_commandPalette, &CommandPalette::shortcutChanged, this, [this](QAction *action) {
        actionCollection()->writeSettings();
        propagateShortcuts({action});
    });
    populateCommandRegistry();

    // Panels move off a device before it is unmounted (or as soon as it is
    // gone), so they neither keep it busy nor show a stale listing.
    connect(MountWatcher::self(), &MountWatcher::mountpointLeaving,
            this, &MainWindow::relocatePanelsOffMount);

    // Tear-off windows skip saved state — their content is set by the caller after
    // show(), avoiding QPainter errors from painting on an unshown window.
    if (!tearOff) {
        restoreSettings();
        applySettings();
    } else {
        // Sync sidebar-header width and tab bar positions once the layout has settled.
        QTimer::singleShot(0, this, [this]() {
            if (m_sidebarHeader && m_outerSplitter) {
                const int sw = m_outerSplitter->sizes().value(0, 220);
                if (sw > 0)
                    m_sidebarHeader->setFixedWidth(
                        qMax(sw, m_sidebarHeader->minimumSizeHint().width()));
            }
            repositionTabBars();
        });
    }
}

MainWindow::~MainWindow()
{
    // Before ~QWidget deletes the children: a focus change while they go
    // would run updateActions() over a half-destroyed window. ~QObject's own
    // disconnect comes too late for that.
    disconnect(qApp, &QApplication::focusChanged, this, &MainWindow::updateActions);
}

void MainWindow::setupUI()
{
    setWindowFlag(Qt::FramelessWindowHint);

    m_centralContainer = new QWidget(this);
    auto *vLayout = new QVBoxLayout(m_centralContainer);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);

    // Create the primary panel first so we can pull its tab bar into the header.
    m_leftPanel = new Panel(nullptr);

    // ── Unified header: [sidebar-header] | [tabs + window controls] ──────────
    auto *headerRow = new QWidget(m_centralContainer);
    auto *headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    // Left section — sits above the sidebar, contains only the Vela button.
    m_sidebarHeader = new QWidget(headerRow);
    m_sidebarHeader->setObjectName(QStringLiteral("velaTitleBar"));
    m_sidebarHeader->setAutoFillBackground(true);
    m_sidebarHeader->installEventFilter(this);
    auto *leftLayout = new QHBoxLayout(m_sidebarHeader);
    leftLayout->setContentsMargins(4, 2, 4, 2);
    leftLayout->setSpacing(2);

    m_velaBtn = new QToolButton(m_sidebarHeader);
    m_velaBtn->setDefaultAction(actionCollection()->action(QStringLiteral("options_configure")));
    m_velaBtn->setIcon(QIcon(QStringLiteral(":/icons/vela.png")));
    m_velaBtn->setText(QStringLiteral("Vela"));
    m_velaBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_velaBtn->setAutoRaise(true);
    leftLayout->addWidget(m_velaBtn);

#ifdef NDEBUG
    auto *buildLabel = new QLabel(
        QStringLiteral("Release v.") + QCoreApplication::applicationVersion(), m_sidebarHeader);
#else
    auto *buildLabel = new QLabel(
        QStringLiteral("Debug v.") + QCoreApplication::applicationVersion(), m_sidebarHeader);
#endif
    buildLabel->setStyleSheet(QStringLiteral("font-size: 10px;"));
    leftLayout->addWidget(buildLabel);

    // Expanding spacer between Vela button and sidebar toggle.
    // Hidden when sidebar is collapsed so the header shrinks to button widths.
    m_sidebarHeaderSpacer = new QWidget(m_sidebarHeader);
    m_sidebarHeaderSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    leftLayout->addWidget(m_sidebarHeaderSpacer);

    // Sidebar toggle lives at the right edge of the sidebar header, at the division.
    auto *sidebarToggleBtn = new QToolButton(m_sidebarHeader);
    sidebarToggleBtn->setDefaultAction(m_actionToggleSidebar);
    sidebarToggleBtn->setAutoRaise(true);
    leftLayout->addWidget(sidebarToggleBtn);

    // Right section — sits above the panels, contains sidebar toggle, tab bar,
    // then the window control glyphs on the far right.
    m_titleBar = new QWidget(headerRow);
    m_titleBar->setObjectName(QStringLiteral("velaTitleBar"));
    m_titleBar->setAutoFillBackground(true);
    m_titleBar->installEventFilter(this);
    installEdgeResizeFilter(); // shared by all windows; installed once
    auto *rightLayout = new QHBoxLayout(m_titleBar);
    rightLayout->setContentsMargins(0, 0, 4, 0);
    rightLayout->setSpacing(2);

    // Tab-bar area: fills the space between the sidebar header and window controls.
    // Tab bars and '+' buttons for all panels are positioned manually inside it.
    m_tabBarArea = new QWidget(m_titleBar);
    m_tabBarArea->setObjectName(QStringLiteral("tabBarArea"));
    m_tabBarArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_tabBarArea->installEventFilter(this);
    rightLayout->addWidget(m_tabBarArea, 1);

    // Window controls (text glyphs — always render in current text colour).
    auto makeWinBtn = [&](const QString &glyph, const QString &tip) -> QToolButton * {
        auto *btn = new QToolButton(m_titleBar);
        btn->setText(glyph);
        btn->setToolTip(tip);
        btn->setAutoRaise(true);
        btn->setFixedSize(28, 28);
        QFont f = btn->font();
        f.setPointSize(11);
        btn->setFont(f);
        rightLayout->addWidget(btn);
        return btn;
    };
    auto *minBtn = makeWinBtn(QStringLiteral("−"), i18n("Minimize"));
    connect(minBtn, &QToolButton::clicked, this, &QWidget::showMinimized);
    m_maxBtn = makeWinBtn(QStringLiteral("□"), i18n("Maximize"));
    connect(m_maxBtn, &QToolButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
    });
    auto *closeBtn = makeWinBtn(QStringLiteral("✕"), i18n("Close"));
    closeBtn->setStyleSheet(QStringLiteral(
        "QToolButton:hover { background-color: #c42b1c; color: white; border-radius: 4px; }"));
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);

    headerLayout->addWidget(m_sidebarHeader);
    headerLayout->addWidget(m_titleBar, 1);
    vLayout->addWidget(headerRow);

    // ── Main content: sidebar | panels ───────────────────────────────────────
    m_outerSplitter = new QSplitter(Qt::Horizontal, m_centralContainer);
    m_outerSplitter->setChildrenCollapsible(false);

    m_sidebar = new Sidebar(m_outerSplitter);

    auto *panelsContainer = new QWidget(m_outerSplitter);
    auto *panelsLayout = new QVBoxLayout(panelsContainer);
    panelsLayout->setContentsMargins(0, 0, 0, 0);
    panelsLayout->setSpacing(0);

    // Panels above, terminal below — inside the panels column, so the sidebar
    // and the preview column keep the full height.
    m_terminalSplitter = new QSplitter(Qt::Vertical, panelsContainer);
    m_terminalSplitter->setChildrenCollapsible(false);
    m_panelRoot = new PanelContainer(m_leftPanel, m_terminalSplitter);
    m_terminal = new TerminalPanel(m_terminalSplitter);
    m_terminal->setMinimumHeight(80);
    m_terminal->hide();
    m_terminalSplitter->addWidget(m_panelRoot);
    m_terminalSplitter->addWidget(m_terminal);
    m_terminalSplitter->setStretchFactor(0, 1);
    m_terminalSplitter->setStretchFactor(1, 0);
    panelsLayout->addWidget(m_terminalSplitter, 1);
    connect(m_terminalSplitter, &QSplitter::splitterMoved, this, [this]() {
        if (m_terminal->isVisible())
            m_terminalHeight = m_terminalSplitter->sizes().value(1, m_terminalHeight);
        scheduleSessionSave(SaveScope::GeometryOnly);
    });

    // F4 and Ctrl+Shift+F4 must work from inside the terminal too.
    m_terminal->setPassthroughActions({m_actionToggleTerminal, m_actionFocusTerminal});
    connect(m_terminal, &TerminalPanel::navigateRequested, this, [this](const QUrl &url) {
        // Focus stays where it is: typing in the terminal goes on there.
        if (Panel *p = activePanel())
            p->navigateToWithoutFocus(url);
    });
    // A finished command (mkdir, rm, …) shows at once, not after KIO's ~700 ms.
    // Every panel: the command may have touched the other panel's folder too.
    connect(m_terminal, &TerminalPanel::promptReturned, this, [this]() {
        for (Panel *p : m_panelRoot->allPanels())
            p->model()->refreshSoon();
    });
    connect(m_terminal, &TerminalPanel::shellExited, this, [this]() {
        m_actionToggleTerminal->setChecked(false); // `exit` closes the panel
    });
    connect(m_terminal, &TerminalPanel::syncEnabledChanged, this, [this]() {
        scheduleSessionSave();
    });

    m_preview = new PreviewPane(m_outerSplitter);
    m_preview->hide();
    connect(m_preview, &PreviewPane::previewToggleRequested, this, &MainWindow::togglePreview);
    // The folder actions follow focus into and out of the preview's views, so
    // their enabled state does too.
    connect(m_preview, &PreviewPane::liveNavigationChanged, this, &MainWindow::updateActions);
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::updateActions);

    m_outerSplitter->addWidget(m_sidebar);
    m_outerSplitter->addWidget(panelsContainer);
    m_outerSplitter->addWidget(m_preview);
    m_outerSplitter->setStretchFactor(0, 0);
    m_outerSplitter->setStretchFactor(1, 1);
    m_outerSplitter->setStretchFactor(2, 0);

    // Keep left-header width in sync with the sidebar splitter position.
    connect(m_outerSplitter, &QSplitter::splitterMoved, this, [this](int pos, int index) {
        if (index == 1) {
            m_sidebarHeader->setFixedWidth(qMax(pos, m_sidebarHeader->minimumSizeHint().width()));
        }
        repositionTabBars();
        // Sidebar and preview widths live in the geometry keys.
        scheduleSessionSave(SaveScope::GeometryOnly);
    });

    vLayout->addWidget(m_outerSplitter, 1);

    // Bottom bar: [sidebar spacer] [per-column status labels] [ViewModeSlider]
    // The spacer width is kept in sync with the sidebar splitter so labels
    // start aligned with the panels area, not the left window edge.
    auto *bottomBar = new QWidget(m_centralContainer);
    auto *bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(0, 0, 4, 0);
    bottomLayout->setSpacing(0);

    m_statusBarSpacer = new QWidget(bottomBar);
    m_statusBarSpacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    bottomLayout->addWidget(m_statusBarSpacer);

    // Manual-geometry container for per-column labels (mirrors m_tabBarArea approach).
    m_statusBarArea = new QWidget(bottomBar);
    m_statusBarArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_statusBarArea->installEventFilter(this);
    bottomLayout->addWidget(m_statusBarArea, 1);

    m_viewModeSlider = new ViewModeSlider(bottomBar);
    m_viewModeSlider->setTerminalAction(m_actionToggleTerminal);
    bottomLayout->addWidget(m_viewModeSlider);
    // Active panel only — the same rule as Ctrl+wheel, the context menu and
    // the view-mode actions. The slider tracks whichever panel is active.
    connect(m_viewModeSlider, &ViewModeSlider::viewModeChanged, this, [this](int mode) {
        if (Panel *p = activePanel())
            p->setViewMode(static_cast<Panel::ViewMode>(mode));
    });
    connect(m_viewModeSlider, &ViewModeSlider::foldersFirstToggled, this, [this](bool first) {
        if (Panel *p = activePanel())
            p->setFoldersFirst(first);
    });
    vLayout->addWidget(bottomBar);

    setCentralWidget(m_centralContainer);
    m_centralContainer->installEventFilter(this);

    m_wireframeOverlay = new WireframeOverlay(m_centralContainer);
    m_wireframeOverlay->raise();

#ifndef NDEBUG
    // Cursor info overlay — debug builds only
    m_cursorInfoLabel = new QLabel(m_centralContainer);
    m_cursorInfoLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_cursorInfoLabel->setStyleSheet(QStringLiteral(
        "background: rgba(30,30,30,210); color: white;"
        " padding: 3px 7px; border-radius: 4px; font-size: 11px;"));
    m_cursorInfoLabel->hide();

    m_cursorInfoTimer = new QTimer(this);
    m_cursorInfoTimer->setInterval(50);
    connect(m_cursorInfoTimer, &QTimer::timeout, this, &MainWindow::onCursorInfoTick);
    m_cursorInfoTimer->start();
#endif

    // Extract the left panel's tab bar into the unified header.
    extractTabBarIntoHeader(m_leftPanel);

    setActivePanel(m_leftPanel);

    connect(m_sidebar, &Sidebar::placeActivated, this, [this](const QUrl &url, bool newTab) {
        Panel *p = activePanel();
        if (!p) return;
        if (newTab) p->newTab(url); else p->navigateTo(url);
    });

    connect(m_panelRoot, &PanelContainer::panelAdded, this, [this](Panel *panel) {
        connectPanel(panel);
        extractTabBarIntoHeader(panel);
        applyHeaderStyle(m_titleBar);
        renumberPanels();
        QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
        scheduleSessionSave(); // panel split → session layout changed
    });
    connect(m_panelRoot, &PanelContainer::panelRemoved, this, [this](Panel *panel) {
        // If the canonical left-panel is being removed, adopt another one so
        // no dangling pointer remains for saveSettings / drag tearoff.
        if (panel == m_leftPanel) {
            const QList<Panel *> remaining = m_panelRoot->allPanels();
            m_leftPanel = remaining.isEmpty() ? nullptr : remaining.first();
        }
        returnTabBarToPanel(panel);
        renumberPanels();
        QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
        scheduleSessionSave(); // panel closed/merged → session layout changed
    });
    connect(m_panelRoot, &PanelContainer::layoutChanged, this, [this]() {
        repositionTabBars();
    });

    connectPanel(m_leftPanel);
    renumberPanels();
}

void MainWindow::setupActions()
{
    m_actionBack = new QAction(QIcon::fromTheme(QStringLiteral("go-previous")), i18n("Back"), this);
    actionCollection()->addAction(QStringLiteral("go_back"), m_actionBack);
    actionCollection()->setDefaultShortcut(m_actionBack, Qt::ALT | Qt::Key_Left);
    connect(m_actionBack, &QAction::triggered, this, &MainWindow::goBack);

    m_actionForward = new QAction(QIcon::fromTheme(QStringLiteral("go-next")), i18n("Forward"), this);
    actionCollection()->addAction(QStringLiteral("go_forward"), m_actionForward);
    actionCollection()->setDefaultShortcut(m_actionForward, Qt::ALT | Qt::Key_Right);
    connect(m_actionForward, &QAction::triggered, this, &MainWindow::goForward);

    m_actionUp = new QAction(QIcon::fromTheme(QStringLiteral("go-up")), i18n("Up"), this);
    actionCollection()->addAction(QStringLiteral("go_up"), m_actionUp);
    actionCollection()->setDefaultShortcut(m_actionUp, Qt::ALT | Qt::Key_Up);
    connect(m_actionUp, &QAction::triggered, this, &MainWindow::goUp);

    m_actionHome = new QAction(QIcon::fromTheme(QStringLiteral("go-home")), i18n("Home"), this);
    actionCollection()->addAction(QStringLiteral("go_home"), m_actionHome);
    actionCollection()->setDefaultShortcut(m_actionHome, Qt::ALT | Qt::Key_Home);
    connect(m_actionHome, &QAction::triggered, this, &MainWindow::goHome);

    m_actionRefresh = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")), i18n("Refresh"), this);
    actionCollection()->addAction(QStringLiteral("refresh"), m_actionRefresh);
    actionCollection()->setDefaultShortcut(m_actionRefresh, Qt::Key_F5);
    connect(m_actionRefresh, &QAction::triggered, this, &MainWindow::refresh);

    m_actionNewTab = new QAction(QIcon::fromTheme(QStringLiteral("tab-new")), i18n("New Tab"), this);
    actionCollection()->addAction(QStringLiteral("new_tab"), m_actionNewTab);
    actionCollection()->setDefaultShortcut(m_actionNewTab, Qt::CTRL | Qt::Key_T);
    connect(m_actionNewTab, &QAction::triggered, this, [this]() {
        if (Panel *p = activePanel())
            p->newTab(p->currentUrl());
    });

    m_actionCloseTab = new QAction(i18n("Close Tab"), this);
    actionCollection()->addAction(QStringLiteral("close_tab"), m_actionCloseTab);
    actionCollection()->setDefaultShortcut(m_actionCloseTab, Qt::CTRL | Qt::Key_W);
    connect(m_actionCloseTab, &QAction::triggered, this, [this]() {
        if (Panel *p = activePanel())
            p->closeTab(p->currentTabIndex());
    });

    m_actionNextTab = new QAction(i18n("Next Tab"), this);
    actionCollection()->addAction(QStringLiteral("next_tab"), m_actionNextTab);
    actionCollection()->setDefaultShortcut(m_actionNextTab, Qt::CTRL | Qt::Key_Tab);
    connect(m_actionNextTab, &QAction::triggered, this, [this]() {
        // tabCount() is briefly 0 while the last tab is being dragged out.
        if (Panel *p = activePanel(); p && p->tabCount() > 0) {
            const int next = (p->currentTabIndex() + 1) % p->tabCount();
            p->switchToTab(next);
        }
    });

    m_actionPrevTab = new QAction(i18n("Previous Tab"), this);
    actionCollection()->addAction(QStringLiteral("prev_tab"), m_actionPrevTab);
    actionCollection()->setDefaultShortcut(m_actionPrevTab, Qt::CTRL | Qt::SHIFT | Qt::Key_Tab);
    connect(m_actionPrevTab, &QAction::triggered, this, [this]() {
        if (Panel *p = activePanel(); p && p->tabCount() > 0) {
            const int prev = (p->currentTabIndex() + p->tabCount() - 1) % p->tabCount();
            p->switchToTab(prev);
        }
    });

    // Ctrl+1-9: jump to tab N. (The view-mode actions below have no default
    // shortcuts, so nothing else claims these keys.)
    for (int i = 1; i <= 9; ++i) {
        auto *action = new QAction(i18n("Go to Tab %1", i), this);
        actionCollection()->addAction(QStringLiteral("go_to_tab_%1").arg(i), action);
        actionCollection()->setDefaultShortcut(action,
                                               QKeySequence(Qt::CTRL | (Qt::Key_0 + i)));
        connect(action, &QAction::triggered, this, [this, i]() {
            if (Panel *p = activePanel())
                p->switchToTab(i - 1);
        });
    }

    // ── File operations ───────────────────────────────────────────────────────

    m_actionCopyToOther = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                      i18n("Copy to Other Panel"), this);
    actionCollection()->addAction(QStringLiteral("copy_to_other"), m_actionCopyToOther);
    // No default shortcut — F5 is reserved for Refresh; user can assign via shortcut editor.
    connect(m_actionCopyToOther, &QAction::triggered, this, [this]() {
        Panel *src = activePanel();
        Panel *dst = otherPanel();
        if (!src || !dst || !dst->isVisible())
            return;
        const QList<QUrl> urls = src->selectedUrls();
        if (!urls.isEmpty())
            FileOperationsManager::instance()->copy(urls, dst->currentUrl(), this);
    });

    m_actionMoveToOther = new QAction(QIcon::fromTheme(QStringLiteral("go-jump")),
                                      i18n("Move to Other Panel"), this);
    actionCollection()->addAction(QStringLiteral("move_to_other"), m_actionMoveToOther);
    actionCollection()->setDefaultShortcut(m_actionMoveToOther, Qt::Key_F6);
    connect(m_actionMoveToOther, &QAction::triggered, this, [this]() {
        Panel *src = activePanel();
        Panel *dst = otherPanel();
        if (!src || !dst || !dst->isVisible())
            return;
        const QList<QUrl> urls = src->selectedUrls();
        if (!urls.isEmpty())
            FileOperationsManager::instance()->move(urls, dst->currentUrl(), this);
    });

    // Edit ▸ Undo: reverses the last recorded file operation (copy, move,
    // rename, trash, new folder, new file). KIO::FileUndoManager is process-wide,
    // so every window's action tracks the same stack.
    m_actionUndo = KStandardAction::undo(this, &MainWindow::undoLastOperation, actionCollection());
    KIO::FileUndoManager *undoManager = KIO::FileUndoManager::self();
    m_actionUndo->setEnabled(undoManager->isUndoAvailable());
    m_actionUndo->setText(undoManager->undoText());
    connect(undoManager, &KIO::FileUndoManager::undoAvailable,
            m_actionUndo, &QAction::setEnabled);
    connect(undoManager, &KIO::FileUndoManager::undoTextChanged,
            m_actionUndo, &QAction::setText);

    m_actionNewFolder = new QAction(QIcon::fromTheme(QStringLiteral("folder-new")),
                                    i18n("New Folder"), this);
    actionCollection()->addAction(QStringLiteral("new_folder"), m_actionNewFolder);
    actionCollection()->setDefaultShortcut(m_actionNewFolder, Qt::Key_F7);
    connect(m_actionNewFolder, &QAction::triggered, this, [this]() {
        if (Panel *p = actionPanel())
            p->createNewFolder();
    });

    // Dolphin's keys: F4 shows/hides, Ctrl+Shift+F4 focuses (showing it first).
    m_actionToggleTerminal = new QAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")),
                                         i18n("Show Terminal"), this);
    m_actionToggleTerminal->setCheckable(true);
    actionCollection()->addAction(QStringLiteral("toggle_terminal"), m_actionToggleTerminal);
    actionCollection()->setDefaultShortcut(m_actionToggleTerminal, Qt::Key_F4);
    connect(m_actionToggleTerminal, &QAction::toggled, this, [this](bool visible) {
        setTerminalVisible(visible);
    });

    m_actionFocusTerminal = new QAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")),
                                        i18n("Focus Terminal"), this);
    actionCollection()->addAction(QStringLiteral("focus_terminal"), m_actionFocusTerminal);
    actionCollection()->setDefaultShortcut(m_actionFocusTerminal, Qt::CTRL | Qt::SHIFT | Qt::Key_F4);
    connect(m_actionFocusTerminal, &QAction::triggered, this, [this]() {
        if (!m_terminal->isVisible())
            m_actionToggleTerminal->setChecked(true); // shows and focuses
        else
            m_terminal->focusTerminal();
    });

    m_actionToggleSidebar = new QAction(QIcon::fromTheme(QStringLiteral("view-sidetree")),
                                        i18n("Show Sidebar"), this);
    m_actionToggleSidebar->setCheckable(true);
    m_actionToggleSidebar->setChecked(true);
    actionCollection()->addAction(QStringLiteral("toggle_sidebar"), m_actionToggleSidebar);
    actionCollection()->setDefaultShortcut(m_actionToggleSidebar, Qt::Key_F9);
    connect(m_actionToggleSidebar, &QAction::toggled, this, [this](bool visible) {
        m_sidebar->setVisible(visible);
        if (m_sidebarHeaderSpacer)
            m_sidebarHeaderSpacer->setVisible(visible);
        if (m_sidebarHeader) {
            if (visible) {
                // Read splitter position after layout has processed the show().
                QTimer::singleShot(0, this, [this]() {
                    const int sw = m_outerSplitter ? m_outerSplitter->sizes().value(0, 220) : 220;
                    m_sidebarHeader->setFixedWidth(qMax(sw, m_sidebarHeader->minimumSizeHint().width()));
                    repositionTabBars();
                });
            } else {
                // Release fixed width so the header shrinks to the two buttons.
                m_sidebarHeader->setMinimumWidth(0);
                m_sidebarHeader->setMaximumWidth(QWIDGETSIZE_MAX);
                QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
            }
        }
    });

    m_actionCommandPalette = new QAction(QIcon::fromTheme(QStringLiteral("search")),
                                         i18n("Command Palette"), this);
    actionCollection()->addAction(QStringLiteral("command_palette"), m_actionCommandPalette);
    actionCollection()->setDefaultShortcut(m_actionCommandPalette,
                                           Qt::CTRL | Qt::SHIFT | Qt::Key_P);
    connect(m_actionCommandPalette, &QAction::triggered, this, [this]() {
        showSettingsDialog();
        m_settingsDialog->showCommandsPage();
    });

    m_actionShowOperations = new QAction(QIcon::fromTheme(QStringLiteral("view-process-system")),
                                         i18n("Show Operations"), this);
    actionCollection()->addAction(QStringLiteral("show_operations"), m_actionShowOperations);
    connect(m_actionShowOperations, &QAction::triggered, this, []() {
        if (OperationsWindow *ops = OperationsWindow::instance())
            ops->present();
    });

    // A widget action, not a plain one: the count is the whole point, and a
    // plain action would lose its text whenever the toolbar is set to icons only.
    m_operationsButton = new QToolButton(this);
    m_operationsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_operationsButton->setAutoRaise(true);
    m_operationsButton->setIcon(m_actionShowOperations->icon());
    m_operationsButton->setToolTip(i18n("File operations are running. Click to show them."));
    connect(m_operationsButton, &QToolButton::clicked, m_actionShowOperations, &QAction::trigger);
    auto *indicator = new QWidgetAction(this);
    indicator->setDefaultWidget(m_operationsButton);
    indicator->setText(i18n("Running Operations"));
    m_actionOperationsIndicator = indicator;
    actionCollection()->addAction(QStringLiteral("operations_indicator"), indicator);
    // A status display, not a command: nothing to bind a shortcut to.
    actionCollection()->setShortcutsConfigurable(indicator, false);

    if (OperationsWindow *ops = OperationsWindow::instance())
        connect(ops, &OperationsWindow::contentChanged, this, &MainWindow::updateOperationsActions);
    updateOperationsActions();

    // ── Backspace as alternate "go up" ───────────────────────────────────────
    // Give Go Up two default shortcuts so both appear in the shortcut editor.
    actionCollection()->setDefaultShortcuts(m_actionUp, {
        QKeySequence(Qt::ALT | Qt::Key_Up),
        QKeySequence(Qt::Key_Backspace),
    });

    // ── Panel-level actions exposed via KActionCollection ────────────────────
    // These were formerly QShortcuts inside Panel; moving them here makes them
    // appear in the shortcut editor and support multiple key bindings.

    auto *actionNewFile = new QAction(
        QIcon::fromTheme(QStringLiteral("document-new")), i18n("New File"), this);
    actionCollection()->addAction(QStringLiteral("new_file"), actionNewFile);
    actionCollection()->setDefaultShortcut(actionNewFile,
                                           Qt::CTRL | Qt::ALT | Qt::Key_N);
    connect(actionNewFile, &QAction::triggered, this, [this]() {
        if (Panel *p = actionPanel()) p->createNewFile();
    });

    auto *actionToggleHidden = new QAction(
        QIcon::fromTheme(QStringLiteral("show-hidden")), i18n("Show Hidden Files"), this);
    actionToggleHidden->setCheckable(true);
    actionCollection()->addAction(QStringLiteral("toggle_hidden_files"), actionToggleHidden);
    actionCollection()->setDefaultShortcuts(actionToggleHidden, {
        QKeySequence(Qt::CTRL | Qt::Key_H),
        QKeySequence(Qt::CTRL | Qt::Key_Period),
    });
    connect(actionToggleHidden, &QAction::triggered, this, [this]() {
        if (Panel *p = actionPanel()) p->toggleHidden();
    });

    auto *actionFocusAddressBar = new QAction(
        QIcon::fromTheme(QStringLiteral("go-jump")), i18n("Focus Address Bar"), this);
    actionCollection()->addAction(QStringLiteral("focus_address_bar"), actionFocusAddressBar);
    actionCollection()->setDefaultShortcut(actionFocusAddressBar,
                                           Qt::CTRL | Qt::Key_L);
    connect(actionFocusAddressBar, &QAction::triggered, this, [this]() {
        if (Panel *p = activePanel()) p->focusAddressBar();
    });

    auto *actionNewFolderAuto = new QAction(
        QIcon::fromTheme(QStringLiteral("folder-new")), i18n("New Folder (Auto-named)"), this);
    actionCollection()->addAction(QStringLiteral("new_folder_auto"), actionNewFolderAuto);
    actionCollection()->setDefaultShortcut(actionNewFolderAuto,
                                           Qt::CTRL | Qt::Key_N);
    connect(actionNewFolderAuto, &QAction::triggered, this, [this]() {
        if (Panel *p = actionPanel()) p->createNewFolderAutoNamed();
    });

    auto *actionSearch = new QAction(
        QIcon::fromTheme(QStringLiteral("search")), i18n("Search"), this);
    actionCollection()->addAction(QStringLiteral("show_search"), actionSearch);
    actionCollection()->setDefaultShortcut(actionSearch, Qt::CTRL | Qt::Key_F);
    connect(actionSearch, &QAction::triggered, this, [this]() {
        if (Panel *p = actionPanel()) p->showSubfolderSearch();
    });

    // ── View mode actions ────────────────────────────────────────────────────

    // No default shortcuts (Ctrl+1-9 belong to tab switching). The user can
    // assign keys via Settings > Configure Keyboard Shortcuts.
    const struct { Panel::ViewMode mode; const char *id; const char *label; } viewModes[] = {
        { Panel::Details,        "view_details",           QT_TR_NOOP("Details View")          },
        { Panel::MediumIcons,    "view_medium_icons",      QT_TR_NOOP("Medium Icons View")      },
        { Panel::LargeIcons,     "view_large_icons",       QT_TR_NOOP("Large Icons View")       },
        { Panel::ExtraLargeIcons,"view_extra_large_icons", QT_TR_NOOP("Extra Large Icons View") },
    };
    for (const auto &vm : viewModes) {
        auto *act = new QAction(i18n(vm.label), this);
        actionCollection()->addAction(QLatin1String(vm.id), act);
        const Panel::ViewMode mode = vm.mode;
        connect(act, &QAction::triggered, this, [this, mode]() {
            if (Panel *p = actionPanel())
                p->setViewMode(mode);
        });
    }

    // ── KDE standard actions ──────────────────────────────────────────────────

    // Settings > Configure Keyboard Shortcuts
    KStandardAction::keyBindings(this, &MainWindow::showShortcutsDialog, actionCollection());

    // Settings > Preferences (Ctrl+,) — dialog implemented in Prompt 13
    KStandardAction::preferences(this, &MainWindow::showSettingsDialog, actionCollection());

    // ── Global shortcut — raise/focus window ──────────────────────────────────

    auto *focusWindowAction = new QAction(
        QIcon::fromTheme(QStringLiteral("window-restore")),
        i18n("Focus/Show Vela Window"), this);
    focusWindowAction->setObjectName(QStringLiteral("focus_window_global"));
    actionCollection()->addAction(QStringLiteral("focus_window_global"), focusWindowAction);
    // No default key sequence — user assigns via global shortcut settings.
    KGlobalAccel::self()->setShortcut(focusWindowAction, {});
    KGlobalAccel::self()->setDefaultShortcut(focusWindowAction, {});
    connect(focusWindowAction, &QAction::triggered, this, [this]() {
        show();
        raise();
        activateWindow();
    });

    updateActions();
}

void MainWindow::updateWindowTitle()
{
    const QString title = (m_windowNumber == 1)
        ? i18n("Vela")
        : i18n("Vela — %1", m_windowNumber);
    setWindowTitle(title);
    setObjectName(QStringLiteral("MainWindow_%1").arg(m_windowNumber));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!s_restartingForTheme && m_preview && !m_preview->confirmDiscard()) {
        event->ignore();
        return;
    }
    if (s_restartingForTheme) {
        // Theme restart: session was already saved before closes began; just track removal.
        s_allWindows.removeOne(this);
    } else if (s_allWindows.size() == 1) {
        // Last window: save its state so next startup restores it.
        saveSession();
        s_allWindows.removeOne(this);
    } else {
        // Secondary window closed manually: save only the remaining windows.
        s_allWindows.removeOne(this);
        saveSession();
    }
    renumberWindows();
    KXmlGuiWindow::closeEvent(event);
}

void MainWindow::enterEvent(QEnterEvent *event)
{
    m_cursorInside = true;
    KXmlGuiWindow::enterEvent(event);
}

void MainWindow::leaveEvent(QEvent *event)
{
    m_cursorInside = false;
#ifndef NDEBUG
    m_cursorInfoLabel->hide();
#endif
    KXmlGuiWindow::leaveEvent(event);
}

Qt::Edges MainWindow::resizeEdgesAt(const QPoint &localPos) const
{
    // Use window-local coordinates (not global) so the check is reliable on Wayland:
    // frameGeometry() can lag behind after move() while the compositor is catching up,
    // causing global-coordinate comparisons to misfire on newly positioned windows.
    const int m = kResizeMargin;
    Qt::Edges edges;
    if (localPos.x() <= m)             edges |= Qt::LeftEdge;
    if (localPos.x() >= width()  - m)  edges |= Qt::RightEdge;
    if (localPos.y() <= m)             edges |= Qt::TopEdge;
    if (localPos.y() >= height() - m)  edges |= Qt::BottomEdge;
    return edges;
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_centralContainer && event->type() == QEvent::Resize) {
        if (m_wireframeOverlay) {
            m_wireframeOverlay->setGeometry(static_cast<QWidget *>(obj)->rect());
            m_wireframeOverlay->raise();
        }
        return false;
    }

    // Reposition tab bars whenever the tab-bar area is resized.
    if (obj == m_tabBarArea && event->type() == QEvent::Resize) {
        repositionTabBars();
        return false;
    }
    if (obj == m_statusBarArea && event->type() == QEvent::Resize) {
        repositionStatusBars();
        return false;
    }

    // Clicks on the tab bar that land on no tab → treat as window drag/maximize.
    if (auto *tb = qobject_cast<QTabBar *>(obj); tb && tb->parent() == m_tabBarArea) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && tb->tabAt(me->pos()) < 0) {
                isMaximized() ? showNormal() : showMaximized();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && windowHandle()
                && tb->tabAt(me->pos()) < 0
                && !resizeEdgesAt(tb->mapTo(this, me->pos()))) {
                windowHandle()->startSystemMove();
                return true;
            }
        }
    }

    // Header sections: drag to move, double-click to toggle maximize.
    // m_tabBarArea empty space is also a valid drag target (tabs/buttons consume their own events).
    if (obj == m_titleBar || obj == m_sidebarHeader || obj == m_tabBarArea) {
        auto *headerWidget = static_cast<QWidget *>(obj);
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                isMaximized() ? showNormal() : showMaximized();
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && windowHandle()) {
                if (!resizeEdgesAt(headerWidget->mapTo(this, me->pos()))) {
                    windowHandle()->startSystemMove();
                    return true;
                }
            }
        }
    }

    // Edge resize is handled by the shared application filter below, which
    // routes each mouse event to handleEdgeEvent() of the window it belongs to.
    return KXmlGuiWindow::eventFilter(obj, event);
}

bool MainWindow::handleEdgeEvent(QWidget *watched, QMouseEvent *me)
{
    if (isMaximized())
        return false;

    if (me->type() == QEvent::MouseButtonPress) {
        if (me->button() == Qt::LeftButton && windowHandle()) {
            const Qt::Edges edges = resizeEdgesAt(watched->mapTo(this, me->pos()));
            if (edges) {
                windowHandle()->startSystemResize(edges);
                return true;
            }
        }
        return false;
    }

    // Resize cursor: skip during tab drag (poll timer owns cursor then).
    if (me->type() == QEvent::MouseMove && !m_dragPollTimer) {
        const Qt::Edges edges = resizeEdgesAt(watched->mapTo(this, me->pos()));
        Qt::CursorShape shape = Qt::ArrowCursor;
        if ((edges & Qt::LeftEdge) && (edges & Qt::TopEdge))         shape = Qt::SizeFDiagCursor;
        else if ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge))shape = Qt::SizeFDiagCursor;
        else if ((edges & Qt::RightEdge) && (edges & Qt::TopEdge))   shape = Qt::SizeBDiagCursor;
        else if ((edges & Qt::LeftEdge) && (edges & Qt::BottomEdge)) shape = Qt::SizeBDiagCursor;
        else if (edges & (Qt::LeftEdge | Qt::RightEdge))              shape = Qt::SizeHorCursor;
        else if (edges & (Qt::TopEdge | Qt::BottomEdge))              shape = Qt::SizeVerCursor;
        // Only on change: this runs on every mouse move over the window.
        if (cursor().shape() != shape)
            setCursor(shape);
    }
    return false;
}

namespace {
// One application-wide filter for every window. Each MainWindow used to
// install itself on qApp, so with N windows every event in the application
// passed through N full eventFilter() calls. This one looks only at mouse
// moves/presses and hands them to the window that owns the widget.
class EdgeResizeFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress)
            return false;
        if (!obj->isWidgetType())
            return false;
        auto *widget = static_cast<QWidget *>(obj);
        if (auto *window = qobject_cast<MainWindow *>(widget->window()))
            return window->handleEdgeEvent(widget, static_cast<QMouseEvent *>(event));
        return false;
    }
};
} // namespace

void MainWindow::installEdgeResizeFilter()
{
    static EdgeResizeFilter *filter = [] {
        auto *f = new EdgeResizeFilter(qApp);
        qApp->installEventFilter(f);
        return f;
    }();
    Q_UNUSED(filter)
}

void MainWindow::raise()
{
    show();
    activateWindow();
    KXmlGuiWindow::raise();
}

void MainWindow::navigateTo(const QUrl &url)
{
    raise();
    if (Panel *p = activePanel())
        p->navigateTo(url);
}

// Resolve a CLI/D-Bus argument (a path or URL, possibly a file) into the folder
// to open and, when the argument names a file, the file to reveal inside it.
static std::pair<QUrl, QUrl> resolveTarget(const QString &arg)
{
    QUrl url;
    if (arg.startsWith(QLatin1Char('/')))
        url = QUrl::fromLocalFile(arg);
    else if (arg.contains(QLatin1String("://")))
        url = QUrl(arg);
    else
        url = QUrl::fromLocalFile(QDir::current().absoluteFilePath(arg));

    if (url.isLocalFile()) {
        const QFileInfo fi(url.toLocalFile());
        if (fi.isFile())
            return {QUrl::fromLocalFile(fi.absolutePath()), url};
    }
    return {url, QUrl()};
}

void MainWindow::openTargets(const QStringList &paths, bool reuseActive)
{
    bool reuse = reuseActive;
    for (const QString &arg : paths) {
        if (arg.isEmpty())
            continue;
        const auto [folder, reveal] = resolveTarget(arg);
        if (!folder.isValid())
            continue;
        if (reuse) {
            if (Panel *p = activePanel()) {
                if (reveal.isValid())
                    p->navigateAndReveal(folder, reveal);
                else
                    p->navigateTo(folder);
            }
            reuse = false;
        } else {
            openInNewWindow(folder, reveal);
        }
    }
}

void MainWindow::openTargetsInNewWindows(const QStringList &paths)
{
    // D-Bus reveal / second-instance handoff: every target gets its own window.
    for (const QString &arg : paths) {
        if (arg.isEmpty())
            continue;
        const auto [folder, reveal] = resolveTarget(arg);
        if (folder.isValid())
            openInNewWindow(folder, reveal);
    }
}

void MainWindow::raiseExisting()
{
    // Bring a live window forward; create one if somehow none remain.
    MainWindow *w = s_allWindows.isEmpty() ? new MainWindow() : s_allWindows.last();
    w->show();
    w->raise();
    w->activateWindow();
    if (w->windowHandle())
        w->windowHandle()->requestActivate();
}

QWidget *MainWindow::anyLiveWindow()
{
    return s_allWindows.isEmpty() ? nullptr : s_allWindows.last();
}

void MainWindow::openInNewWindow(const QUrl &folder, const QUrl &fileToReveal)
{
    FolderTarget target;
    target.folder = folder;
    if (fileToReveal.isValid())
        target.reveal = {fileToReveal};
    openTabsInNewWindow({target});
}

void MainWindow::openTabsInNewWindow(const QList<FolderTarget> &targets)
{
    if (targets.isEmpty())
        return;

    // tearOff=true skips restoreSettings(). A plain `new MainWindow()` runs
    // session restore, so "open containing folder" resurrected every saved tab
    // (and even extra windows) and then merely re-pointed the active one at the
    // target. This window must contain exactly the requested tabs.
    auto *w = new MainWindow(nullptr, /*tearOff=*/true);
    w->m_sidebar->restoreSettings();
    w->applySettings();

    if (Panel *p = w->activePanel()) {
        p->clearAllTabs();
        const int mode = VelaSettings::self()->defaultViewMode();
        // Background tabs keep their reveal until first shown; the first tab is
        // the one shown, and applies its reveal on its first listing.
        for (const FolderTarget &t : targets) {
            TabContext ctx(t.folder, mode);
            ctx.reveal = t.reveal;
            p->insertTab(ctx, /*activate=*/false);
        }
        p->switchToTab(0);
        p->applyColumnSettings();
    }

    w->show();
    w->raise();
    w->activateWindow();
    if (w->windowHandle())
        w->windowHandle()->requestActivate();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    KXmlGuiWindow::resizeEvent(event);
    applyWindowMask();
    QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
    // Geometry only — a resize can't change tabs, so don't rewrite the tree.
    scheduleSessionSave(SaveScope::GeometryOnly);
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    KXmlGuiWindow::moveEvent(event);
    scheduleSessionSave(SaveScope::GeometryOnly);
}

void MainWindow::changeEvent(QEvent *event)
{
    // Coming back to this window is the moment its panels may be showing a
    // directory something else deleted while we were away — including deletions
    // the directory watch cannot see (a moved-away parent, a remote location).
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_panelRoot) {
        for (Panel *p : m_panelRoot->allPanels())
            p->revalidateDirectory();
    }
    if (event->type() == QEvent::WindowStateChange && m_maxBtn) {
        m_maxBtn->setIcon(QIcon::fromTheme(isMaximized()
            ? QStringLiteral("window-restore")
            : QStringLiteral("window-maximize")));
    }
    // KDE color scheme and font changes arrive as StyleChange / FontChange.
    // Qt automatically re-polishes widgets, but we need to repaint our panels.
    KXmlGuiWindow::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange
        || event->type() == QEvent::FontChange) {
        // Cached file icons were resolved against the previous theme.
        FileItemModelBase::clearIconCache();
        if (m_titleBar) {
            applyHeaderStyle(m_sidebarHeader);
            applyHeaderStyle(m_titleBar);
        }
        if (m_panelRoot) m_panelRoot->update();
        if (m_sidebar)   m_sidebar->update();
    }
}

void MainWindow::goBack()
{
    if (Panel *p = actionPanel())
        p->goBack();
}

void MainWindow::goForward()
{
    if (Panel *p = actionPanel())
        p->goForward();
}

void MainWindow::goUp()
{
    if (Panel *p = actionPanel())
        p->goUp();
}

void MainWindow::goHome()
{
    if (Panel *p = actionPanel())
        p->navigateTo(QUrl::fromLocalFile(QDir::homePath()));
}

void MainWindow::refresh()
{
    if (Panel *p = actionPanel())
        p->goRefresh();
}

void MainWindow::onPanelActivated()
{
    auto *panel = qobject_cast<Panel *>(sender());
    if (panel)
        setActivePanel(panel);
}

void MainWindow::updateActions()
{
    Panel *p = actionPanel();
    m_actionBack->setEnabled(p && p->canGoBack());
    m_actionForward->setEnabled(p && p->canGoForward());
    m_actionUp->setEnabled(p && p->currentUrl().isValid());
    m_actionHome->setEnabled(true);
    m_actionRefresh->setEnabled(p && p->currentUrl().isValid());

    const bool hasOther = otherPanel() != nullptr;
    if (m_actionCopyToOther)
        m_actionCopyToOther->setEnabled(hasOther);
    if (m_actionMoveToOther)
        m_actionMoveToOther->setEnabled(hasOther);
}

Panel *MainWindow::activePanel() const
{
    return m_activePanel;
}

Panel *MainWindow::actionPanel() const
{
    if (m_preview) {
        if (Panel *p = m_preview->focusedPanel())
            return p;
    }
    return m_activePanel;
}

Panel *MainWindow::otherPanel() const
{
    if (!m_panelRoot)
        return nullptr;
    for (Panel *p : m_panelRoot->allPanels()) {
        if (p != m_activePanel)
            return p;
    }
    return nullptr;
}

void MainWindow::setActivePanel(Panel *panel)
{
    if (m_activePanel == panel)
        return;
    if (m_activePanel)
        m_activePanel->setActive(false);
    m_activePanel = panel;
    if (m_activePanel)
        m_activePanel->setActive(true);
    updateActions();
    if (m_viewModeSlider)
        m_viewModeSlider->setActivePanel(panel);
    if (m_preview)
        m_preview->setPanel(panel);
    if (m_terminal && panel)
        m_terminal->setExplorerUrl(panel->currentUrl());
}

void MainWindow::togglePreview()
{
    setPreviewVisible(!m_preview->isVisible());
}

void MainWindow::setPreviewVisible(bool visible, int width)
{
    if (!visible) {
        if (!m_preview->isVisible())
            return;
        // Closing drops the document, so unsaved edits get their say. Cancel
        // keeps the pane open.
        if (!m_preview->confirmDiscard())
            return;
        m_preview->hide();
        scheduleSessionSave();
        return;
    }
    if (m_preview->isVisible())
        return;

    const QList<int> sizes = m_outerSplitter->sizes();
    const int sidebarW = sizes.value(0, 0);
    const int panelsW  = sizes.value(1, 0) + sizes.value(2, 0);
    if (width <= 0)
        width = m_preview->rememberedWidth(panelsW / 2);
    // Leave the panels at least as much room as the preview needs itself.
    width = qBound(m_preview->minimumWidth(), width,
                   qMax(panelsW - m_preview->minimumWidth(), m_preview->minimumWidth()));

    m_preview->setPanel(m_activePanel);
    m_preview->show();
    m_outerSplitter->setSizes({sidebarW, panelsW - width, width});
    QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
    scheduleSessionSave();
}

void MainWindow::setTerminalVisible(bool visible, int height)
{
    // The action is the one source of truth, so the bottom-bar button and F4
    // agree whichever way the change came in (restore, `exit`): go through it,
    // and come back here from its toggled().
    if (m_actionToggleTerminal->isChecked() != visible) {
        if (height > 0)
            m_terminalHeight = height;
        m_actionToggleTerminal->setChecked(visible);
        return;
    }

    if (!visible) {
        if (m_terminal->isHidden())
            return;
        m_terminalHeight = m_terminalSplitter->sizes().value(1, m_terminalHeight);
        const bool hadFocus = m_terminal->isAncestorOf(QApplication::focusWidget());
        m_terminal->stopShell(); // by design: hiding ends the shell and its jobs
        m_terminal->hide();
        if (hadFocus && m_activePanel)
            m_activePanel->focusView();
        scheduleSessionSave();
        return;
    }

    if (m_terminal->isHidden()) {
        const int total = m_terminalSplitter->height();
        if (height > 0)
            m_terminalHeight = height;
        int h = m_terminalHeight > 0 ? m_terminalHeight : total / 3;
        // Leave the panels at least as much room as the terminal needs itself.
        h = qBound(m_terminal->minimumHeight(), h,
                   qMax(total - m_terminal->minimumHeight(), m_terminal->minimumHeight()));
        if (m_activePanel)
            m_terminal->setExplorerUrl(m_activePanel->currentUrl());
        m_terminal->show();
        m_terminalSplitter->setSizes({total - h, h});
        scheduleSessionSave();
    }
    m_terminal->startShell();
    m_terminal->focusTerminal();
}

void MainWindow::connectPanel(Panel *panel)
{
    connect(panel, &Panel::activated, this, [this, panel]() {
        setActivePanel(panel);
    });
    connect(panel, &Panel::navigationStateChanged, this, &MainWindow::updateActions);
    // Recent only learns about folders that actually listed: urlChanged fires
    // as a load starts, so a mistyped address used to be recorded too.
    connect(panel, &Panel::directoryVisited, m_sidebar, &Sidebar::recordVisit);
    connect(panel, &Panel::urlChanged, this, [this, panel](const QUrl &url) {
        if (panel == m_activePanel) {
            m_sidebar->setCurrentUrl(url);
            m_terminal->setExplorerUrl(url);
        }
        // Debounced autosave so an unclean exit doesn't lose the session.
        scheduleSessionSave();
    });
    connect(panel, &Panel::previewToggleRequested, this, [this, panel]() {
        setActivePanel(panel);
        togglePreview();
    });
    connect(panel, &Panel::tabDragStarted,  this, &MainWindow::onTabDragStarted);
    connect(panel, &Panel::columnVisibilityChanged, this, [this, panel]() {
        for (Panel *p : m_panelRoot->allPanels()) {
            if (p != panel)
                p->applyColumnSettings();
        }
        for (Panel *p : m_preview->livePanels())
            p->applyColumnSettings();
    });
    connect(panel, &Panel::tabCountChanged, this, [this]() {
        QTimer::singleShot(0, this, [this]() { repositionTabBars(); });
        scheduleSessionSave(); // tab opened/closed → session structure changed
    });
    connect(panel, &Panel::closeRequested,  this, [this, panel]() {
        const QList<Panel *> panels = m_panelRoot->allPanels();
        if (panels.size() <= 1) {
            close();
            return;
        }
        if (m_activePanel == panel) {
            for (Panel *p : panels) {
                if (p != panel) { setActivePanel(p); break; }
            }
        }
        m_panelRoot->removePanel(panel);
        panel->deleteLater();
        updateActions();
    });
}

// ── Drop-target API (called by the drag-coordinator window) ──────────────────

void MainWindow::showDragOverlays()
{
    hideDragOverlays();
    for (Panel *p : m_panelRoot->allPanels()) {
        auto *overlay = new DropZoneOverlay(p, m_panelRoot);
        const QPoint pos = p->mapTo(m_panelRoot, QPoint(0, 0));
        overlay->setGeometry(pos.x(), pos.y(), p->width(), p->height());
        overlay->raise();
        overlay->show();
        m_dragOverlays.append(overlay);
    }
}

void MainWindow::hideDragOverlays()
{
    for (DropZoneOverlay *ov : m_dragOverlays)
        delete ov;
    m_dragOverlays.clear();
    if (m_panelRoot) {
        for (Panel *p : m_panelRoot->allPanels())
            p->repaint();
        m_panelRoot->repaint();
    }
}

void MainWindow::updateDragOverlayHover(const QPoint &globalPos)
{
    for (DropZoneOverlay *ov : m_dragOverlays) {
        const QPoint local = ov->mapFromGlobal(globalPos);
        ov->setHoveredZone(ov->rect().contains(local) ? ov->zoneAtPoint(local)
                                                       : DropZoneOverlay::DropZone::None);
    }
}

DropZoneOverlay *MainWindow::dragOverlayAt(const QPoint &globalPos) const
{
    for (DropZoneOverlay *ov : m_dragOverlays) {
        if (ov->rect().contains(ov->mapFromGlobal(globalPos)))
            return ov;
    }
    return nullptr;
}

void MainWindow::acceptTabDrop(const TabContext &ctx, DropZoneOverlay::DropZone zone, Panel *target)
{
    if (zone == DropZoneOverlay::DropZone::Center) {
        target->insertTab(ctx);
        setActivePanel(target);
        return;
    }

    Qt::Edge edge = Qt::RightEdge;
    switch (zone) {
    case DropZoneOverlay::DropZone::Left:   edge = Qt::LeftEdge;   break;
    case DropZoneOverlay::DropZone::Right:  edge = Qt::RightEdge;  break;
    case DropZoneOverlay::DropZone::Top:    edge = Qt::TopEdge;    break;
    case DropZoneOverlay::DropZone::Bottom: edge = Qt::BottomEdge; break;
    default: break;
    }

    auto *newPanel = new Panel(nullptr);
    newPanel->applyColumnSettings();
    newPanel->insertTab(ctx);
    m_panelRoot->splitPanel(target, newPanel, edge);
    setActivePanel(newPanel);
    // connectPanel is called automatically via panelAdded signal from splitPanel
}

MainWindow *MainWindow::windowUnderCursor()
{
    // Option A: enter/leave events — reliable for same-screen windows, fails during
    // cross-window drag (OS pointer grab suppresses events on the target window).
    for (int i = s_allWindows.size() - 1; i >= 0; --i)
        if (s_allWindows[i]->m_cursorInside)
            return s_allWindows[i];
    return nullptr;

    // THIS DOES NOT WORK — DO NOT ATTEMPT TO GO THIS ROUTE AGAIN: hit-testing
    // QCursor::pos() against each window's geometry(). On Wayland a client
    // does not know where its windows are on screen, so the geometry is
    // meaningless for this. (The helper that did it has been removed.)
}

// ── Drag lifecycle (coordinator side) ────────────────────────────────────────

void MainWindow::onTabDragStarted(Panel *source, int tabIndex)
{
    m_dragSourcePanel = source;
    m_dragTabIndex    = tabIndex;
    m_dragTabCtx = source->takeTab(tabIndex);

    const QUrl dragUrl = m_dragTabCtx.history.current();
    if (!dragUrl.isValid())
        return;

    const QString title = dragUrl.fileName().isEmpty()
                        ? dragUrl.toString()
                        : dragUrl.fileName();

    // Render drag cursor: small move-icon + tab name label (Wayland-safe).
    {
        const int pad    = 6;
        const int iconSz = 14;
        const int gap    = 4;
        QFont font;
        QFontMetrics fm(font);
        const QRect tb    = fm.boundingRect(title);
        const int labelW  = tb.width() + pad * 2;
        const int labelH  = tb.height() + pad * 2;
        const int totalW  = iconSz + gap + labelW;
        const int totalH  = qMax(iconSz, labelH);

        QPixmap pm(totalW, totalH);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing);

        // Blue circle drag icon
        const int iconY = (totalH - iconSz) / 2;
        painter.setBrush(QColor(70, 130, 220, 220));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(0, iconY, iconSz, iconSz);

        // Cross-arrows inside the circle
        const QPointF c(iconSz / 2.0, iconY + iconSz / 2.0);
        const qreal arm = 4.0;
        painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(c + QPointF(-arm, 0), c + QPointF(arm, 0));
        painter.drawLine(c + QPointF(0, -arm), c + QPointF(0, arm));
        painter.drawLine(c + QPointF(arm, 0),  c + QPointF(arm - 2, -2));
        painter.drawLine(c + QPointF(arm, 0),  c + QPointF(arm - 2,  2));
        painter.drawLine(c + QPointF(-arm, 0), c + QPointF(-arm + 2, -2));
        painter.drawLine(c + QPointF(-arm, 0), c + QPointF(-arm + 2,  2));
        painter.drawLine(c + QPointF(0, -arm), c + QPointF(-2, -arm + 2));
        painter.drawLine(c + QPointF(0, -arm), c + QPointF( 2, -arm + 2));
        painter.drawLine(c + QPointF(0,  arm), c + QPointF(-2,  arm - 2));
        painter.drawLine(c + QPointF(0,  arm), c + QPointF( 2,  arm - 2));

        // Tab name label
        const int labelX = iconSz + gap;
        const int labelY = (totalH - labelH) / 2;
        painter.setBrush(QColor(60, 60, 60, 210));
        painter.setPen(QPen(QColor(180, 180, 180, 200), 1));
        painter.drawRoundedRect(labelX, labelY, labelW, labelH, 4, 4);
        painter.setPen(Qt::white);
        painter.setFont(font);
        painter.drawText(labelX + pad, labelY + pad, tb.width(), tb.height(),
                         Qt::AlignLeft | Qt::AlignTop, title);
        painter.end();

        // Hotspot at icon center
        QApplication::setOverrideCursor(QCursor(pm, iconSz / 2, totalH / 2));
    }

    // If the source window is now tabless, hide it immediately so it doesn't
    // block other windows from receiving drop events.
    bool windowNowEmpty = true;
    for (Panel *p : m_panelRoot->allPanels())
        if (p->tabCount() > 0) { windowNowEmpty = false; break; }
    if (windowNowEmpty)
        hide();

    startDragPoll();
}

void MainWindow::startDragPoll()
{
    stopDragPoll();
    m_dragPollTimer = new QTimer(this);
    m_dragPollTimer->setInterval(16);
    connect(m_dragPollTimer, &QTimer::timeout, this, &MainWindow::onDragPoll);
    m_dragPollTimer->start();
}

void MainWindow::stopDragPoll()
{
    if (!m_dragPollTimer)
        return;
    m_dragPollTimer->stop();
    m_dragPollTimer->deleteLater();
    m_dragPollTimer = nullptr;
}

void MainWindow::cleanupDrag()
{
    QApplication::restoreOverrideCursor();
    m_hoveredWindow = nullptr;
    stopDragPoll();
    m_dragSourcePanel = nullptr;
    m_dragTabIndex    = -1;
    m_dragTabCtx      = {};
}

void MainWindow::onDragPoll()
{
    const QPoint pos  = QCursor::pos();
    const bool   held = (QApplication::mouseButtons() & Qt::LeftButton) != 0;

    // Only allow dropping back into the source window — other windows are not valid targets.
    MainWindow *backWindow = (windowUnderCursor() == this) ? this : nullptr;

    // Update snap overlays: show in the back-window, hide in any previous one.
    if (backWindow != m_hoveredWindow) {
        if (m_hoveredWindow)
            m_hoveredWindow->hideDragOverlays();
        m_hoveredWindow = backWindow;
        if (m_hoveredWindow)
            m_hoveredWindow->showDragOverlays();
    }
    if (m_hoveredWindow)
        m_hoveredWindow->updateDragOverlayHover(pos);

    if (!held) {
        // Extract drop info before cleanup invalidates overlay pointers.
        QPointer<MainWindow> targetWindow = m_hoveredWindow;
        Panel *dropTarget  = nullptr;
        DropZoneOverlay::DropZone dropZone = DropZoneOverlay::DropZone::None;

        if (targetWindow) {
            if (DropZoneOverlay *hit = targetWindow->dragOverlayAt(pos)) {
                dropZone  = hit->zoneAtPoint(hit->mapFromGlobal(pos));
                dropTarget = hit->targetPanel();
            }
        }

        const TabContext savedCtx = m_dragTabCtx;
        cleanupDrag();   // stops poll, deletes floatingTab, clears m_hoveredWindow

        if (targetWindow)
            targetWindow->hideDragOverlays();

        if (dropTarget && targetWindow) {
            targetWindow->acceptTabDrop(savedCtx, dropZone, dropTarget);
        } else if (savedCtx.history.current().isValid()) {
            // Released over empty space — spawn a new standalone window.
            auto *newWindow = new MainWindow(nullptr, /*tearOff=*/true);
            newWindow->show();
            newWindow->move(pos - QPoint(newWindow->width() / 2, 50));
            newWindow->m_leftPanel->insertTab(savedCtx);
            newWindow->m_leftPanel->applyColumnSettings();
        }

        // Close this source window if it ended up with no tabs anywhere.
        QTimer::singleShot(0, this, [this]() {
            for (Panel *p : m_panelRoot->allPanels())
                if (p->tabCount() > 0)
                    return;
            close();
        });
    }
}


#ifndef NDEBUG
void MainWindow::onCursorInfoTick()
{
    // m_cursorInside is O(1) — check it before the O(n) windowUnderCursor() loop.
    if (!m_cursorInside || m_dragPollTimer || windowUnderCursor() != this) {
        m_cursorInfoLabel->hide();
        return;
    }

    const QPoint globalPos = QCursor::pos();

    // widgetAt() is used only to identify what widget the cursor is over for
    // display purposes. m_cursorInside already decides which window owns the label;
    // we do not gate on w->window() because widgetAt() is unreliable on Wayland
    // and would suppress the label in the correct window.
    QWidget *w = QApplication::widgetAt(globalPos);
    if (!w || w->window() != this)
        w = childAt(mapFromGlobal(globalPos));
    if (!w) {
        m_cursorInfoLabel->hide();
        return;
    }

    // Walk up the widget tree to find the enclosing Panel
    Panel *panel = nullptr;
    for (QWidget *it = w; it; it = it->parentWidget()) {
        if (auto *p = qobject_cast<Panel *>(it)) { panel = p; break; }
    }

    const QString cls    = QString::fromUtf8(w->metaObject()->className());
    const QString winStr = i18n("Window %1", m_windowNumber);
    QString info;
    if (panel) {
        const QUrl url = panel->currentUrl();
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        info = winStr + QStringLiteral(" • ") + cls + QStringLiteral(" • ") + path;
    } else {
        info = winStr + QStringLiteral(" • ") + cls;
    }

    m_cursorInfoLabel->setText(info);
    m_cursorInfoLabel->adjustSize();

    const QPoint local = m_centralContainer->mapFromGlobal(globalPos);
    int x = qMin(local.x() + 18, m_centralContainer->width()  - m_cursorInfoLabel->width()  - 4);
    int y = qMin(local.y() + 18, m_centralContainer->height() - m_cursorInfoLabel->height() - 4);
    m_cursorInfoLabel->move(qMax(x, 0), qMax(y, 0));
    m_cursorInfoLabel->raise();
    m_cursorInfoLabel->show();
}
#endif // NDEBUG

void MainWindow::saveSettings()
{
    saveSession();
}

// Geometry is the only part of a window's state that a resize or move can
// change, and it is cheap. Keeping it separate means dragging a window edge no
// longer rewrites every tab URL in every window.
void MainWindow::saveGeometryToGroup(KConfigGroup &group) const
{
    group.writeEntry("Geometry", saveGeometry());
    // Sidebar and panels only: a hidden preview reports 0 here, and its real
    // width is kept under its own key so it survives being closed.
    const QList<int> sizes = m_outerSplitter->sizes();
    group.writeEntry("OuterSplitterSizes", QList<int>{sizes.value(0), sizes.value(1)});
    group.writeEntry("PreviewWidth", m_preview->rememberedWidth(-1));
    group.writeEntry("TerminalHeight", m_terminal->isVisible()
                                           ? m_terminalSplitter->sizes().value(1, m_terminalHeight)
                                           : m_terminalHeight);
}

void MainWindow::saveToGroup(KConfigGroup &group) const
{
    saveGeometryToGroup(group);
    group.writeEntry("SidebarVisible", m_actionToggleSidebar->isChecked());
    group.writeEntry("PreviewOpen", m_preview->isVisible());
    group.writeEntry("TerminalOpen", m_terminal->isVisible());
    group.writeEntry("TerminalSync", m_terminal->syncEnabled());
    KConfigGroup treeGroup = group.group(QStringLiteral("PanelTree"));
    m_panelRoot->saveLayout(treeGroup);
}

// INTENDED — do not flag: session state (tabs, geometry) lives in velarc
// alongside the settings, not in KSharedConfig::openStateConfig(). The
// autosave rewrite is debounced (5 s, paused while unfocused) and KConfig
// skips unchanged keys, so the cost is small; moving it would need a one-time
// migration for no user-visible gain. Decided and kept deliberately.
void MainWindow::saveSession(SaveScope scope)
{
    if (s_allWindows.isEmpty())
        return;

    KSharedConfigPtr cfg = KSharedConfig::openConfig();
    KConfigGroup session = cfg->group(QStringLiteral("Session"));

    if (scope == SaveScope::GeometryOnly
        && session.readEntry("WindowCount", 0) == s_allWindows.size()) {
        // Fast path: only touch the geometry keys. KConfig skips writes for
        // values that haven't changed, so a no-op resize costs nothing.
        for (int i = 0; i < s_allWindows.size(); ++i) {
            KConfigGroup wg = session.group(QStringLiteral("Window_%1").arg(i));
            s_allWindows[i]->saveGeometryToGroup(wg);
        }
        if (cfg->isDirty())
            cfg->sync();
        return;
    }

    // Full save. deleteGroup() is only correct when the window count shrank —
    // otherwise it throws away entries KConfig would have diffed away for free,
    // guaranteeing a full rewrite every time.
    const int previousCount = session.readEntry("WindowCount", 0);
    if (previousCount > s_allWindows.size()) {
        session.deleteGroup();
        session = cfg->group(QStringLiteral("Session"));
    }

    session.writeEntry("WindowCount", s_allWindows.size());
    for (int i = 0; i < s_allWindows.size(); ++i) {
        KConfigGroup wg = session.group(QStringLiteral("Window_%1").arg(i));
        s_allWindows[i]->saveToGroup(wg);
    }

    // Sidebar state is application-global — save once.
    s_allWindows.first()->m_sidebar->saveSettings();

    // Nothing actually changed (e.g. navigating back to where you started):
    // skip the sync entirely rather than rewriting velarc.
    if (cfg->isDirty())
        cfg->sync();
}

void MainWindow::scheduleSessionSave(SaveScope scope)
{
    if (!s_sessionSaveTimer) {
        s_sessionSaveTimer = new QTimer(qApp);
        s_sessionSaveTimer->setSingleShot(true);
        s_sessionSaveTimer->setInterval(5000);
        QObject::connect(s_sessionSaveTimer, &QTimer::timeout, qApp, []() {
            // Don't overwrite the saved session with an empty one after the last
            // window has already closed (closeEvent saved on the way out).
            if (s_allWindows.isEmpty())
                return;
            const SaveScope pending = s_pendingSaveScope;
            s_pendingSaveScope = SaveScope::GeometryOnly;
            saveSession(pending);
        });
    }

    // A pending full save is never downgraded by a later geometry-only request.
    if (scope == SaveScope::Full)
        s_pendingSaveScope = SaveScope::Full;

    // Only count down while the app has focus. Restarting the 5s window also
    // coalesces rapid traversals into one save once navigation settles.
    if (QApplication::activeWindow())
        s_sessionSaveTimer->start();
}

// The autosave countdown is paused while Vela is unfocused, so background
// windows never write. Losing focus flushes once immediately, so an unclean
// exit while alt-tabbed away still can't lose the session.
void MainWindow::onAppFocusChanged(QWidget *old, QWidget *now)
{
    if (!s_sessionSaveTimer)
        return;

    const bool focused = (now != nullptr);
    if (focused) {
        // Regained focus: resume the countdown if a save is still outstanding.
        if (!s_sessionSaveTimer->isActive() && s_pendingSaveScope == SaveScope::Full)
            s_sessionSaveTimer->start();
        return;
    }

    Q_UNUSED(old)
    if (s_sessionSaveTimer->isActive()) {
        s_sessionSaveTimer->stop();
        const SaveScope pending = s_pendingSaveScope;
        s_pendingSaveScope = SaveScope::GeometryOnly;
        saveSession(pending);
    }
}

void MainWindow::restoreSettings()
{
    KSharedConfigPtr cfg = KSharedConfig::openConfig();
    const KConfigGroup session = cfg->group(QStringLiteral("Session"));

    if (session.readEntry("WindowCount", 0) > 0
            && (VelaSettings::self()->restoreTabsOnStartup() || s_restartingForTheme)) {
        restoreSession();
        return;
    }

    // ── Legacy single-window fallback ────────────────────────────────────────
    KConfigGroup group = cfg->group(QStringLiteral("MainWindow"));
    m_sidebar->restoreSettings();

    const bool sidebarVisible = group.readEntry("SidebarVisible", true);
    m_actionToggleSidebar->setChecked(sidebarVisible);
    m_sidebar->setVisible(sidebarVisible);

    const QList<int> outerSizes = group.readEntry("OuterSplitterSizes", QList<int>());
    if (outerSizes.size() == 2)
        m_outerSplitter->setSizes(outerSizes);
    else
        m_outerSplitter->setSizes({220, m_outerSplitter->width() - 220});

    const bool previewOpen  = group.readEntry("PreviewOpen", false);
    const int  previewWidth = group.readEntry("PreviewWidth", -1);
    m_preview->setRememberedWidth(previewWidth);
    const bool terminalOpen = group.readEntry("TerminalOpen", false);
    m_terminalHeight = group.readEntry("TerminalHeight", -1);
    m_terminal->setSyncEnabled(group.readEntry("TerminalSync", true));
    QTimer::singleShot(0, this, [this, previewOpen, previewWidth, terminalOpen]() {
        if (m_sidebarHeader && m_outerSplitter) {
            const int sw = m_outerSplitter->sizes().value(0, 220);
            m_sidebarHeader->setFixedWidth(qMax(sw, m_sidebarHeader->minimumSizeHint().width()));
        }
        // After the splitter has real geometry, so the width split is computed
        // against the actual panels column.
        if (previewOpen)
            setPreviewVisible(true, previewWidth);
        if (terminalOpen)
            setTerminalVisible(true);
        repositionTabBars();
    });

    m_leftPanel->restoreSettings(QStringLiteral("LeftPanel"));

    if (m_leftPanel->tabCount() == 0) {
        const QString leftPath = group.readEntry("LeftPanelPath", QDir::homePath());
        m_leftPanel->newTab(QUrl::fromLocalFile(leftPath));
    }
}

bool MainWindow::restoreSession()  // non-static; Window_0 is restored into `this`
{
    KSharedConfigPtr cfg = KSharedConfig::openConfig();
    const KConfigGroup session = cfg->group(QStringLiteral("Session"));
    const int windowCount = session.readEntry("WindowCount", 0);
    if (windowCount <= 0)
        return false;

    // Window_0 is restored into the already-constructed first window (this).
    const KConfigGroup wg0 = session.group(QStringLiteral("Window_0"));
    restoreFromGroup(wg0);

    // Additional windows are created as tear-offs then restored.
    for (int i = 1; i < windowCount; ++i) {
        const KConfigGroup wgi = session.group(QStringLiteral("Window_%1").arg(i));
        auto *win = new MainWindow(nullptr, /*tearOff=*/true);
        win->restoreFromGroup(wgi);
        win->applySettings();
        win->show();
    }

    m_sidebar->restoreSettings();
    return true;
}

void MainWindow::restoreFromGroup(const KConfigGroup &group)
{
    const QByteArray geom = group.readEntry("Geometry", QByteArray());
    if (!geom.isEmpty())
        restoreGeometry(geom);

    const bool sidebarVisible = group.readEntry("SidebarVisible", true);
    m_actionToggleSidebar->setChecked(sidebarVisible);
    m_sidebar->setVisible(sidebarVisible);

    const QList<int> outerSizes = group.readEntry("OuterSplitterSizes", QList<int>());
    if (outerSizes.size() == 2)
        m_outerSplitter->setSizes(outerSizes);
    else
        m_outerSplitter->setSizes({220, m_outerSplitter->width() - 220});

    const KConfigGroup treeGroup = group.group(QStringLiteral("PanelTree"));
    if (treeGroup.readEntry("Type", QString()) == QLatin1String("leaf") ||
        treeGroup.readEntry("Type", QString()) == QLatin1String("split")) {
        m_panelRoot->restoreLayout(treeGroup);
        // connectPanel and extractTabBarIntoHeader are handled by the panelAdded signal.
    }

    if (m_leftPanel->tabCount() == 0)
        m_leftPanel->newTab(QUrl::fromLocalFile(QDir::homePath()));

    const bool previewOpen  = group.readEntry("PreviewOpen", false);
    const int  previewWidth = group.readEntry("PreviewWidth", -1);
    m_preview->setRememberedWidth(previewWidth);
    const bool terminalOpen = group.readEntry("TerminalOpen", false);
    m_terminalHeight = group.readEntry("TerminalHeight", -1);
    m_terminal->setSyncEnabled(group.readEntry("TerminalSync", true));
    QTimer::singleShot(0, this, [this, previewOpen, previewWidth, terminalOpen]() {
        if (m_sidebarHeader && m_outerSplitter) {
            const int sw = m_outerSplitter->sizes().value(0, 220);
            m_sidebarHeader->setFixedWidth(qMax(sw, m_sidebarHeader->minimumSizeHint().width()));
        }
        // After the splitter has real geometry, so the width split is computed
        // against the actual panels column.
        if (previewOpen)
            setPreviewVisible(true, previewWidth);
        if (terminalOpen)
            setTerminalVisible(true);
        repositionTabBars();
    });
}


void MainWindow::showShortcutsDialog()
{
    auto *dlg = new KShortcutsDialog(KShortcutsEditor::AllActions,
                                     KShortcutsEditor::LetterShortcutsAllowed, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->addCollection(actionCollection());
    connect(dlg, &KShortcutsDialog::saved, this, [this]() {
        propagateShortcuts(actionCollection()->actions());
    });
    dlg->configure(true);
}

// Every window has its own action objects, so a shortcut edited in one window
// only rebound that window's action — the others kept the old key until
// restart. Copy the new bindings across by action name.
void MainWindow::propagateShortcuts(const QList<QAction *> &changed)
{
    for (MainWindow *w : std::as_const(s_allWindows)) {
        if (w == this)
            continue;
        for (QAction *src : changed) {
            if (!src || src->objectName().isEmpty())
                continue;
            if (QAction *dst = w->actionCollection()->action(src->objectName()))
                dst->setShortcuts(src->shortcuts());
        }
        w->m_commandPalette->populate(); // refresh the shortcut badges
    }
}

void MainWindow::showSettingsDialog()
{
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(m_commandPalette, this);
        // Settings are application-global, so every open window has to re-apply
        // them — not just the one that happens to own the dialog.
        connect(m_settingsDialog, &SettingsDialog::settingsApplied,
                this, &MainWindow::applySettingsToAllWindows);
    }
    if (!m_settingsDialog->isVisible())
        m_settingsDialog->loadFromSettings();
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void MainWindow::applySettingsToAllWindows()
{
    // Theme changes tear down and recreate every window, so iterate a copy.
    const QList<MainWindow *> windows = s_allWindows;
    for (MainWindow *w : windows) {
        if (s_allWindows.contains(w))
            w->applySettings();
    }
}

void MainWindow::applySettings()
{
    VelaSettings *s = VelaSettings::self();

    // Theme: System=0, Light=1, Dark=2
    {
        KColorSchemeManager *mgr = KColorSchemeManager::instance();
        const int mode = s->themeMode();
        const bool themeChanged = (s_appliedThemeMode >= 0) && (s_appliedThemeMode != mode);
        s_appliedThemeMode = mode;

        if (mode == 0) {
            mgr->activateSchemeId(QString()); // revert to system/KDE default
        } else {
            const QString keyword = (mode == 1) ? QStringLiteral("Light")
                                                : QStringLiteral("Dark");
            const auto *model = mgr->model();
            QString found;
            for (int r = 0; r < model->rowCount(); ++r) {
                const QModelIndex idx = model->index(r, 0);
                const QString name = idx.data(Qt::DisplayRole).toString();
                if (name.contains(keyword, Qt::CaseInsensitive)) {
                    found = idx.data(KColorSchemeModel::IdRole).toString();
                    break;
                }
            }
            if (!found.isEmpty())
                mgr->activateSchemeId(found);
        }

        if (themeChanged) {
            // Recreate all windows so they inherit the new palette cleanly.
            // Unsaved preview edits are discarded without asking (closeEvent
            // skips confirmDiscard while the flag is set) — deliberate.
            QTimer::singleShot(0, []() {
                saveSession();
                s_restartingForTheme = true;
                const QList<MainWindow *> toClose = s_allWindows;
                for (MainWindow *w : toClose)
                    w->close();
                // The flag stays set through construction so restoreSettings()
                // brings back every window and tab regardless of "Restore tabs
                // on startup": that option is about launching the app, and a
                // theme switch must not wipe the current session.
                auto *w = new MainWindow();
                w->show();
                s_restartingForTheme = false;
            });
            return;
        }
    }

    // Font
    {
        const QString family = s->fontFamily();
        const int size = s->fontSize();
        // Empty family / size 0 means "follow the system". Set the platform
        // font explicitly: doing nothing kept whatever custom font was active
        // until the next launch.
        const QFont wanted = (!family.isEmpty() && size > 0)
                                 ? QFont(family, size)
                                 : QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        if (qApp->font() != wanted)
            qApp->setFont(wanted);
    }

    for (Panel *p : m_panelRoot->allPanels())
        p->applyAppearanceSettings();
    for (Panel *p : m_preview->livePanels())
        p->applyAppearanceSettings();
    m_sidebar->setTrackRecent(s->trackRecent());

    static const int kIconSizes[] = {16, 22, 32};
    const int sz = kIconSizes[qBound(0, s->toolbarIconSize(), 2)];

    // Re-apply header palette so it tracks the current theme.
    applyHeaderStyle(m_sidebarHeader);
    applyHeaderStyle(m_titleBar);

    // Also keep the hidden KXmlGui toolbar in sync (user may re-enable it).
    const auto &tbs = toolBars();
    for (KToolBar *tb : tbs) {
        tb->setIconSize(QSize(sz, sz));
        tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

    if (m_wireframeOverlay)
        m_wireframeOverlay->raise();

    applyWindowMask();
}

void MainWindow::applyWindowMask()
{
    constexpr int r = 6;
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), r, r);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
    // update(), not repaint(): this runs on every resize event, and repaint()
    // painted synchronously each time instead of once per frame.
    if (m_centralContainer)
        m_centralContainer->update();
}

void MainWindow::applyHeaderStyle(QWidget *header)
{
    if (!header) return;
    QPalette pal = qApp->palette();
    pal.setColor(QPalette::Window, pal.color(QPalette::Button));
    pal.setColor(QPalette::WindowText, pal.color(QPalette::ButtonText));
    header->setPalette(pal);

    // QTabBar has its own stylesheet (min-width), which breaks Qt palette inheritance.
    // Explicitly push the fresh palette so the platform style re-renders with correct colours.
    for (QTabBar *tb : header->findChildren<QTabBar *>()) {
        tb->setPalette(qApp->palette());
        tb->update();
    }

    static const int kIconSizes[] = {16, 22, 32};
    const int sz = kIconSizes[qBound(0, VelaSettings::self()->toolbarIconSize(), 2)];
    for (QToolButton *btn : header->findChildren<QToolButton *>()) {
        if (!btn->defaultAction()) continue;
        btn->setIconSize(QSize(sz, sz));
        btn->setToolButtonStyle(btn == m_velaBtn ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    }
}

// ── Tab-bar-in-header management ──────────────────────────────────────────────

void MainWindow::extractTabBarIntoHeader(Panel *panel)
{
    if (!m_tabBarArea || !panel)
        return;
    QTabBar *tb = panel->tabBar();
    if (!tb)
        return;
    // Guard: already extracted (panelAdded signal and restoreFromGroup both call this).
    if (tb->parent() == m_tabBarArea)
        return;

    // Remove from panel's VBoxLayout (it was added there during Panel construction).
    if (auto *pl = qobject_cast<QVBoxLayout *>(panel->layout()))
        pl->removeWidget(tb);
    tb->setParent(m_tabBarArea);
    tb->show();
    tb->installEventFilter(this);  // for window-drag on empty tab bar space

    // Height is initialised lazily in repositionTabBars once tabs exist.

    if (!m_addTabBtns.contains(panel))
        m_addTabBtns[panel] = createAddTabButton(panel, m_tabBarArea);
}

void MainWindow::returnTabBarToPanel(Panel *panel)
{
    if (!panel)
        return;
    QTabBar *tb = panel->tabBar();
    if (tb && tb->parent() == m_tabBarArea) {
        tb->removeEventFilter(this);
        tb->setParent(panel);
    }
    if (QToolButton *btn = m_addTabBtns.take(panel))
        delete btn;
    if (QWidget *row = m_embeddedTabRows.take(panel))
        delete row;
    if (QLabel *label = m_bottomStatusLabels.take(panel)) {
        disconnect(panel, &Panel::statusChanged, label, &QLabel::setText);
        delete label;
    }
}

void MainWindow::renumberPanels()
{
    if (!m_panelRoot) return;
    const QList<Panel *> panels = m_panelRoot->allPanels();
    const bool multi = panels.size() > 1;
    for (int i = 0; i < panels.size(); ++i) {
        panels[i]->setPanelNumber(i + 1);
        // Allow tearing off the last tab only when sibling panels exist.
        panels[i]->setDragLastTabAllowed(multi);
    }
}

void MainWindow::repositionTabBars()
{
    if (!m_tabBarArea || !m_panelRoot)
        return;

    m_tabBarArea->setUpdatesEnabled(false);

    // Structure comes from the split tree, not from comparing panel x-positions.
    // A panel's tab bar belongs in the header exactly when its top edge touches
    // the top of the panel area.
    const QList<Panel *> headerPanels = m_panelRoot->topEdgePanels();

    // Height is set lazily: during setup, tab bars have no tabs and sizeHint()
    // returns 0. Once tabs exist we can read the real height and lock it in.
    if (m_tabBarArea->height() == 0) {
        for (Panel *panel : m_panelRoot->allPanels()) {
            QTabBar *tb = panel->tabBar();
            if (tb && tb->parent() == m_tabBarArea) {
                const int h = tb->sizeHint().height();
                if (h > 0) {
                    m_tabBarArea->setFixedHeight(h);
                    break;
                }
            }
        }
    }

    syncTabBarOwnership(headerPanels);
    layOutHeaderTabBars(headerPanels);

    m_tabBarArea->setUpdatesEnabled(true);

    repositionStatusBars();
}

QToolButton *MainWindow::createAddTabButton(Panel *panel, QWidget *parent)
{
    auto *addBtn = new QToolButton(parent);
    addBtn->setText(QStringLiteral("+"));
    addBtn->setToolTip(i18n("New Tab"));
    addBtn->setAutoRaise(true);
    QFont f = addBtn->font();
    f.setPointSize(16);
    addBtn->setFont(f);
    connect(addBtn, &QToolButton::clicked, this, [this, panel]() {
        panel->newTab(QUrl::fromLocalFile(QDir::homePath()));
    });
    addBtn->show();
    return addBtn;
}

// Pull a panel's tab bar out of the panel (or its embedded row) and into the
// shared header area.
void MainWindow::moveTabBarToHeader(Panel *panel)
{
    QTabBar *tb = panel->tabBar();
    if (!tb || tb->parent() == m_tabBarArea)
        return;

    if (QWidget *row = m_embeddedTabRows.take(panel)) {
        if (auto *pl = qobject_cast<QVBoxLayout *>(panel->layout()))
            pl->removeWidget(row);
        row->layout()->removeWidget(tb);
        tb->setParent(nullptr); // rescue tb before row (its parent) is deleted
        delete row;             // also deletes the embedded '+' button
    } else if (auto *pl = qobject_cast<QVBoxLayout *>(panel->layout())) {
        pl->removeWidget(tb);
    }

    tb->setParent(m_tabBarArea);
    tb->show();
    tb->installEventFilter(this);
    if (!m_addTabBtns.contains(panel))
        m_addTabBtns[panel] = createAddTabButton(panel, m_tabBarArea);
}

// Give a panel its own tab bar row, directly above its content.
void MainWindow::moveTabBarIntoPanel(Panel *panel)
{
    if (m_embeddedTabRows.contains(panel))
        return;

    QTabBar *tb = panel->tabBar();
    if (!tb)
        return;

    if (tb->parent() == m_tabBarArea) {
        if (QToolButton *btn = m_addTabBtns.take(panel))
            delete btn;
    }

    auto *row = new QWidget(panel);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    tb->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    rowLayout->addWidget(tb, 0);
    rowLayout->addWidget(createAddTabButton(panel, row));
    rowLayout->addStretch(1);

    if (auto *pl = qobject_cast<QVBoxLayout *>(panel->layout()))
        pl->insertWidget(1, row);
    row->show();
    tb->show();
    m_embeddedTabRows[panel] = row;
}

// Panels along the top edge keep their tab bar in the window header. Everything
// below one of them gets an embedded row instead, since a single header strip
// cannot represent two vertically stacked panels.
void MainWindow::syncTabBarOwnership(const QList<Panel *> &headerPanels)
{
    const QSet<Panel *> inHeader(headerPanels.cbegin(), headerPanels.cend());
    for (Panel *panel : m_panelRoot->allPanels()) {
        if (!panel->tabBar())
            continue;
        if (inHeader.contains(panel))
            moveTabBarToHeader(panel);
        else
            moveTabBarIntoPanel(panel);
    }
}

void MainWindow::layOutHeaderTabBars(const QList<Panel *> &headerPanels)
{
    const int areaW = m_tabBarArea->width();
    const int h     = m_tabBarArea->height();

    // Each header tab bar sits directly above its own panel. The x-offset and
    // width come from geometry — those are genuinely pixel questions — but
    // *which* panels get a header slot no longer does.
    for (Panel *panel : headerPanels) {
        QTabBar *tb = panel->tabBar();
        if (!tb || tb->parent() != m_tabBarArea)
            continue;

        const int slotX  = panel->mapTo(m_panelRoot, QPoint(0, 0)).x();
        const int availW = qMax(qMin(panel->width(), areaW - slotX), 0);

        QToolButton *addBtn = m_addTabBtns.value(panel, nullptr);
        const int addBtnW  = addBtn ? addBtn->sizeHint().width() : 0;
        const int maxTabsW = qMax(availW - addBtnW, 0);
        // Place '+' right after the last tab; fall back to the slot edge when
        // the tabs overflow their slot.
        const int contentW = tb->count() > 0 ? qMin(tb->sizeHint().width(), maxTabsW) : 0;

        tb->setGeometry(slotX, 0, maxTabsW, h);
        if (addBtn)
            addBtn->setGeometry(slotX + contentW, 0, addBtnW, h);
    }

    // Pin embedded rows to the same height as the header strip so they don't
    // inflate from the large '+' button font.
    const int rowH = h > 0 ? h
                           : (m_tabBarArea->minimumHeight() > 0 ? m_tabBarArea->minimumHeight() : 0);
    for (auto it = m_embeddedTabRows.cbegin(); it != m_embeddedTabRows.cend(); ++it) {
        Panel   *panel = it.key();
        QWidget *row   = it.value();
        QTabBar *tb    = panel->tabBar();
        if (!row || !tb)
            continue;
        if (rowH > 0)
            row->setFixedHeight(rowH);
        tb->setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

void MainWindow::repositionStatusBars()
{
    if (!m_statusBarArea || !m_panelRoot)
        return;

    // Sync sidebar spacer width so status labels start aligned with the panels area.
    if (m_statusBarSpacer && m_outerSplitter) {
        const bool sidebarVis = m_sidebar && m_sidebar->isVisible();
        m_statusBarSpacer->setVisible(sidebarVis);
        if (sidebarVis)
            m_statusBarSpacer->setFixedWidth(m_outerSplitter->sizes().value(0, 220));
    }

    // Mirror of the tab-bar rule: a panel uses the window's global status bar
    // exactly when its bottom edge touches the bottom of the panel area.
    const QList<Panel *> bottomEdge = m_panelRoot->bottomEdgePanels();
    const QSet<Panel *> bottomPanels(bottomEdge.cbegin(), bottomEdge.cend());

    // Non-bottom panels keep their own embedded status bar; bottom panels use the global bar.
    for (Panel *panel : m_panelRoot->allPanels())
        panel->setStatusBarVisible(!bottomPanels.contains(panel));

    // Remove labels for panels that are no longer bottom panels (or no longer exist).
    for (auto it = m_bottomStatusLabels.begin(); it != m_bottomStatusLabels.end(); ) {
        if (!bottomPanels.contains(it.key())) {
            disconnect(it.key(), &Panel::statusChanged, it.value(), &QLabel::setText);
            delete it.value();
            it = m_bottomStatusLabels.erase(it);
        } else {
            ++it;
        }
    }

    // Create labels for newly promoted bottom panels.
    for (Panel *panel : std::as_const(bottomPanels)) {
        if (!m_bottomStatusLabels.contains(panel)) {
            auto *label = new QLabel(m_statusBarArea);
            label->setContentsMargins(4, 0, 4, 0);
            label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            label->setText(panel->statusText());
            label->show();
            connect(panel, &Panel::statusChanged, label, &QLabel::setText);
            m_bottomStatusLabels.insert(panel, label);
        }
    }

    // Set area height lazily (first time labels have meaningful sizeHint).
    if (m_statusBarArea->height() == 0 && !m_bottomStatusLabels.isEmpty()) {
        if (const int h = m_bottomStatusLabels.first()->sizeHint().height(); h > 0)
            m_statusBarArea->setFixedHeight(h);
    }

    // Position each label directly beneath its own panel.
    const int areaW = m_statusBarArea->width();
    const int areaH = m_statusBarArea->height();
    for (Panel *panel : bottomEdge) {
        QLabel *label = m_bottomStatusLabels.value(panel);
        if (!label)
            continue;
        const int slotX  = panel->mapTo(m_panelRoot, QPoint(0, 0)).x();
        const int labelW = qMax(qMin(panel->width(), areaW - slotX), 0);
        label->setGeometry(slotX, 0, labelW, areaH);
    }
}

void MainWindow::updateOperationsActions()
{
    const OperationsWindow *ops = OperationsWindow::instance();
    const int running = ops ? ops->runningCount() : 0;
    m_actionShowOperations->setEnabled(ops && ops->hasContent());
    m_actionOperationsIndicator->setVisible(running > 0);
    m_operationsButton->setText(i18np("%1 operation", "%1 operations", running));
}

void MainWindow::populateCommandRegistry()
{
    CommandRegistry *reg = m_commandRegistry;
    reg->clear();

    // Register every named action in the collection (navigation, tabs, ops, etc.)
    for (QAction *action : actionCollection()->actions()) {
        if (action == m_actionOperationsIndicator)
            continue; // a status display; Show Operations is the command
        if (!action->isSeparator() && !action->text().isEmpty())
            reg->registerAction(action);
    }

    // Panel-specific toggle commands that delegate to the active panel.
    reg->registerCommand({QStringLiteral("panel_toggle_hidden"),
                          i18n("Toggle Hidden Files"),
                          QIcon::fromTheme(QStringLiteral("hint")),
                          nullptr,
                          [this]() {
                              if (Panel *p = activePanel())
                                  p->model()->setShowHidden(!p->model()->showHidden());
                          }});

    // Inform the palette so it rebuilds its item list.
    m_commandPalette->populate();
}

// ── Undo ──────────────────────────────────────────────────────────────────────

void MainWindow::undoLastOperation()
{
    KIO::FileUndoManager *undoManager = KIO::FileUndoManager::self();
    if (!undoManager->isUndoAvailable())
        return;

    // All KIO exposes about the pending command is its label ("Undo: Move").
    // Undoing touches files, possibly far away from what is on screen, so it
    // is never done on a bare keypress.
    const QString what = KLocalizedString::removeAcceleratorMarker(undoManager->undoText());
    QMessageBox box(QMessageBox::Question, i18n("Undo"),
                    i18n("%1?", what), QMessageBox::NoButton, this);
    box.setInformativeText(i18n("This reverses the last file operation: copied or new "
                                "items are removed, moved or renamed items go back, and "
                                "trashed items are restored."));
    QPushButton *undoBtn = box.addButton(i18n("Undo"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(undoBtn);
    prepareMessageBox(box);
    box.exec();
    if (box.clickedButton() != undoBtn)
        return;

    // The manager is shared by every window; its dialogs belong to this one.
    undoManager->uiInterface()->setParentWidget(this);
    undoManager->undo();
}

// ── Unmount ───────────────────────────────────────────────────────────────────

void MainWindow::relocatePanelsOffMount(const QString &mountpoint)
{
    const QUrl root = QUrl::fromLocalFile(mountpoint);
    // Home, unless home itself lives on the device going away.
    QUrl fallback = QUrl::fromLocalFile(QDir::homePath());
    const QUrl rootDir = root.adjusted(QUrl::StripTrailingSlash);
    if (rootDir == fallback.adjusted(QUrl::StripTrailingSlash) || rootDir.isParentOf(fallback))
        fallback = QUrl::fromLocalFile(QFileInfo(mountpoint).absolutePath());
    for (Panel *p : m_panelRoot->allPanels())
        p->relocateTabsUnder(root, fallback);
    m_terminal->relocateOffMount(mountpoint, fallback.toLocalFile());
}

