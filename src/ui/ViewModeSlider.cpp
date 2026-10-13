#include "ViewModeSlider.h"

#include "Panel.h"

#include <KLocalizedString>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QToolButton>

static const struct {
    Panel::ViewMode mode;
    const char     *iconName;
    const char     *label;
} kModes[] = {
    { Panel::Details,         "view-list-details", QT_TR_NOOP("Details")            },
    { Panel::MediumIcons,     "view-list-icons",   QT_TR_NOOP("Medium Icons")       },
    { Panel::LargeIcons,      "view-list-icons",   QT_TR_NOOP("Large Icons")        },
    { Panel::ExtraLargeIcons, "view-list-icons",   QT_TR_NOOP("Extra Large Icons")  },
};
static constexpr int kModeCount = static_cast<int>(std::size(kModes));

static const char *kFolderSizeStyleActive =
    "QToolButton { background-color: rgba(220,150,0,70);"
    " border: 1px solid rgba(200,130,0,130); border-radius: 3px; }";

// Same shape as the folder-size highlight, in the selection-blue family: on is
// a normal state here, not a warning.
static const char *kTerminalStyleActive =
    "QToolButton { background-color: rgba(61,174,233,60);"
    " border: 1px solid rgba(61,174,233,140); border-radius: 3px; }";

static const char *kFolderSizeTooltipOff =
    "Analyse folder sizes\n\n"
    "When enabled, Vela scans every sub-folder in the current directory "
    "to calculate its total size. Disable when not needed: "
    "repeatedly reading directory metadata forces the disk heads to "
    "seek across the platter on HDDs, increasing latency and wear. "
    "On SSDs the impact is lower but still wastes I/O budget.";

static const char *kFolderSizeTooltipOn =
    "!!!  Folder size analysis is ACTIVE — disable when not needed.\n\n"
    "Scanning directory sizes forces disk metadata reads for every "
    "sub-folder. On HDDs this causes repeated head seeks that "
    "degrade performance and increase mechanical wear over time. "
    "Click to stop and clear calculated sizes.";

ViewModeSlider::ViewModeSlider(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(4);

    // ── Terminal toggle (wired to its action by setTerminalAction) ────────────
    m_terminalBtn = new QToolButton(this);
    m_terminalBtn->setAutoRaise(true);
    m_terminalBtn->setCheckable(true);
    m_terminalBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_terminalBtn->setIcon(QIcon::fromTheme(QStringLiteral("utilities-terminal")));
    m_terminalBtn->setText(i18n("Terminal"));
    m_terminalBtn->setEnabled(false);
    layout->addWidget(m_terminalBtn);

    // ── Folder size analysis toggle ───────────────────────────────────────────
    m_folderSizeBtn = new QToolButton(this);
    m_folderSizeBtn->setAutoRaise(true);
    m_folderSizeBtn->setCheckable(true);
    m_folderSizeBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_folderSizeBtn->setIcon(QIcon::fromTheme(QStringLiteral("folder-info")));
    m_folderSizeBtn->setText(i18n("Folder Sizes"));
    m_folderSizeBtn->setToolTip(QLatin1String(kFolderSizeTooltipOff));
    layout->addWidget(m_folderSizeBtn);

    // ── Folders-first toggle ──────────────────────────────────────────────────
    m_foldersFirstBtn = new QToolButton(this);
    m_foldersFirstBtn->setAutoRaise(true);
    m_foldersFirstBtn->setCheckable(true);
    m_foldersFirstBtn->setChecked(true);
    m_foldersFirstBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_foldersFirstBtn->setText(i18n("Group Folders"));
    layout->addWidget(m_foldersFirstBtn);

    // ── Mode button ───────────────────────────────────────────────────────────
    m_modeButton = new QToolButton(this);
    m_modeButton->setAutoRaise(true);
    m_modeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_modeButton->setToolTip(i18n("View mode — hover to change"));
    m_modeButton->installEventFilter(this);
    layout->addWidget(m_modeButton);

    // ── Hover timer (for mode popup) ──────────────────────────────────────────
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(150);
    connect(m_hoverTimer, &QTimer::timeout, this, &ViewModeSlider::onHoverTimeout);

    // ── Connections ───────────────────────────────────────────────────────────
    connect(m_folderSizeBtn, &QToolButton::toggled, this, [this](bool checked) {
        updateFolderSizeButton(checked);
        if (m_activePanel)
            m_activePanel->setFolderSizeCalcEnabled(checked);
    });

    connect(m_foldersFirstBtn, &QToolButton::toggled, this, [this](bool checked) {
        updateFoldersFirstButton(checked);
        // Applied by MainWindow through this signal — the same route as
        // viewModeChanged. Calling the panel here as well sorted twice.
        Q_EMIT foldersFirstToggled(checked);
    });

    updateIcons();
    updateModeButton(Panel::Details);
    updateFoldersFirstButton(true);
    updateFolderSizeButton(false);
}

void ViewModeSlider::setActivePanel(Panel *panel)
{
    disconnect(m_viewModeConn);
    disconnect(m_folderSizeConn);
    m_activePanel = panel;

    if (!panel) {
        updateModeButton(Panel::Details);
        return;
    }

    updateModeButton(static_cast<int>(panel->viewMode()));

    const bool ff = panel->foldersFirst();
    m_foldersFirstBtn->blockSignals(true);
    m_foldersFirstBtn->setChecked(ff);
    m_foldersFirstBtn->blockSignals(false);
    updateFoldersFirstButton(ff);

    const bool fsc = panel->folderSizeCalcEnabled();
    m_folderSizeBtn->blockSignals(true);
    m_folderSizeBtn->setChecked(fsc);
    m_folderSizeBtn->blockSignals(false);
    updateFolderSizeButton(fsc);

    m_viewModeConn = connect(panel, &Panel::viewModeChanged, this,
                             [this](int mode) { updateModeButton(mode); });

    m_folderSizeConn = connect(panel, &Panel::folderSizeCalcChanged, this,
                               [this](bool active) {
                                   m_folderSizeBtn->blockSignals(true);
                                   m_folderSizeBtn->setChecked(active);
                                   m_folderSizeBtn->blockSignals(false);
                                   updateFolderSizeButton(active);
                               });
}

void ViewModeSlider::setTerminalAction(QAction *action)
{
    m_terminalBtn->setEnabled(action != nullptr);
    if (!action)
        return;
    // Not setDefaultAction(): that would show the action's menu text
    // ("Show Terminal") instead of the short label.
    connect(m_terminalBtn, &QToolButton::clicked, action, &QAction::trigger);
    connect(action, &QAction::toggled, this, &ViewModeSlider::updateTerminalButton);
    connect(action, &QAction::changed, this, [this, action]() {
        updateTerminalButton(action->isChecked()); // shortcut may have been rebound
    });
    m_terminalAction = action;
    updateTerminalButton(action->isChecked());
}

void ViewModeSlider::updateTerminalButton(bool shown)
{
    {
        const QSignalBlocker block(m_terminalBtn);
        m_terminalBtn->setChecked(shown);
    }
    m_terminalBtn->setStyleSheet(shown ? QLatin1String(kTerminalStyleActive) : QString{});
    const QString key = m_terminalAction
        ? m_terminalAction->shortcut().toString(QKeySequence::NativeText) : QString();
    const QString tip = shown ? i18n("Hide the terminal. Its shell and anything running in it end.")
                              : i18n("Show a terminal below the panels, following the active folder");
    m_terminalBtn->setToolTip(key.isEmpty() ? tip : i18n("%1 (%2)", tip, key));
}

bool ViewModeSlider::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_modeButton) {
        if (event->type() == QEvent::Enter)
            m_hoverTimer->start();
        else if (event->type() == QEvent::Leave)
            m_hoverTimer->stop();
    }
    return QWidget::eventFilter(watched, event);
}

void ViewModeSlider::onHoverTimeout()
{
    showModePopup();
}

void ViewModeSlider::showModePopup()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    for (int i = 0; i < kModeCount; ++i) {
        const auto &m = kModes[i];
        QAction *action = menu->addAction(
            QIcon::fromTheme(QLatin1String(m.iconName)), tr(m.label));
        const Panel::ViewMode mode = m.mode;
        connect(action, &QAction::triggered, this, [this, mode]() {
            updateModeButton(static_cast<int>(mode));
            Q_EMIT viewModeChanged(static_cast<int>(mode));
        });
    }

    const QPoint pos = m_modeButton->mapToGlobal(
        QPoint(0, -menu->sizeHint().height()));
    menu->exec(pos);
}

void ViewModeSlider::updateModeButton(int mode)
{
    if (mode < 0 || mode >= kModeCount)
        return;
    const auto &m = kModes[mode];
    m_modeButton->setIcon(QIcon::fromTheme(QLatin1String(m.iconName)));
    m_modeButton->setText(tr(m.label));
}

void ViewModeSlider::updateFoldersFirstButton(bool first)
{
    const QString icon = first ? QStringLiteral("view-sort-ascending-name")
                               : QStringLiteral("format-list-unordered");
    m_foldersFirstBtn->setIcon(QIcon::fromTheme(icon));
    m_foldersFirstBtn->setToolTip(
        first ? i18n("Folders grouped first (click to mix)")
              : i18n("Folders mixed with files (click to group)"));
}

void ViewModeSlider::updateFolderSizeButton(bool active)
{
    m_folderSizeBtn->setStyleSheet(active ? QLatin1String(kFolderSizeStyleActive) : QString{});
    m_folderSizeBtn->setToolTip(QLatin1String(active ? kFolderSizeTooltipOn
                                                     : kFolderSizeTooltipOff));
}

void ViewModeSlider::updateIcons()
{
    m_terminalBtn->setIcon(QIcon::fromTheme(QStringLiteral("utilities-terminal")));
    updateFoldersFirstButton(m_foldersFirstBtn->isChecked());
    updateFolderSizeButton(m_folderSizeBtn->isChecked());
    const QString cur = m_modeButton->text();
    for (int i = 0; i < kModeCount; ++i) {
        if (cur == tr(kModes[i].label)) {
            updateModeButton(i);
            break;
        }
    }
}

void ViewModeSlider::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        updateIcons();
    }
    QWidget::changeEvent(event);
}
