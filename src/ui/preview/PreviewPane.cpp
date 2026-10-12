#include "PreviewPane.h"

#include "PreviewContainer.h"
#include "ui/Panel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QHideEvent>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

PreviewPane::PreviewPane(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(180);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *caption = new QLabel(i18n("Preview"), this);
    caption->setAlignment(Qt::AlignCenter);
    caption->setContentsMargins(8, 6, 8, 2);
    {
        QFont cf = caption->font();
        cf.setBold(true);
        cf.setPointSizeF(cf.pointSizeF() + 1);
        caption->setFont(cf);
    }

    // The whole chain of containers scrolls as one column. Resizable, so a
    // short chain fills the height and its deepest page takes what is left.
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_root = new PreviewContainer(0, [this]() { return m_panel.data(); });
    m_scroll->setWidget(m_root);

    layout->addWidget(caption);
    layout->addWidget(m_scroll, 1);

    // The folder link of any container: enter that folder in the followed
    // panel, selecting what the link names. The panel's own selection change
    // then brings the preview along.
    connect(m_root, &PreviewContainer::revealRequested, this,
            [this](const QUrl &folder, const QUrl &item) {
        if (m_panel)
            m_panel->navigateAndReveal(folder, item);
    });
    connect(m_root, &PreviewContainer::previewToggleRequested,
            this, &PreviewPane::previewToggleRequested);
    connect(m_root, &PreviewContainer::liveNavigationChanged,
            this, &PreviewPane::liveNavigationChanged);

    // A held arrow key changes the selection dozens of times a second; the
    // preview only needs the one it lands on.
    m_refreshDebounce = new QTimer(this);
    m_refreshDebounce->setSingleShot(true);
    m_refreshDebounce->setInterval(80);
    connect(m_refreshDebounce, &QTimer::timeout, this, &PreviewPane::refresh);

    m_root->showMessage(i18n("Nothing selected"));

    // Build the classification tables here, on the GUI thread, before a
    // classification worker could be the first to touch them.
    PreviewContainer::primeClassification();
}

PreviewPane::~PreviewPane() = default;

// ── Panel binding ────────────────────────────────────────────────────────────

void PreviewPane::setPanel(Panel *panel)
{
    if (m_panel == panel)
        return;
    disconnectPanel();
    m_panel = panel;
    m_root->setSource(panel);
    if (panel)
        connectPanel(panel);
    m_refreshDebounce->stop();
    refresh();
}

void PreviewPane::connectPanel(Panel *panel)
{
    connect(panel, &Panel::previewItemChanged, m_refreshDebounce,
            qOverload<>(&QTimer::start));
    connect(panel, &QObject::destroyed, this, [this]() {
        m_panel = nullptr;
        m_root->setSource(nullptr);
        resolveUnsavedChanges();
        refresh();
    });
}

void PreviewPane::disconnectPanel()
{
    if (m_panel) {
        disconnect(m_panel, nullptr, m_refreshDebounce, nullptr);
        disconnect(m_panel, nullptr, this, nullptr);
    }
}

// ── Content ──────────────────────────────────────────────────────────────────

void PreviewPane::refresh()
{
    if (!isVisible())
        return;

    if (!m_panel) {
        if (!confirmDiscard())
            return;
        m_root->showMessage(i18n("Nothing selected"));
        return;
    }

    int count = 0;
    const KFileItem item = m_panel->soleSelectedItem(count);

    // Same item still selected: nothing to do. A directory reload echoes a
    // selection change without the selection having moved; this keeps dirty
    // editors and the open nested previews alive across it, and spares an
    // image a second decode.
    const KFileItem shown = m_root->item();
    if (count == 1 && !shown.isNull() && shown.url() == item.url())
        return;

    // Anything below replaces the documents, so unsaved edits get their say
    // first. Cancel leaves them on screen; the panel's new selection is
    // simply not followed until the user decides.
    if (!confirmDiscard())
        return;

    if (count == 0) {
        m_root->showMessage(i18n("Nothing selected"));
    } else if (count > 1) {
        m_root->showMessage(i18np("%1 item selected", "%1 items selected", count));
    } else {
        m_root->showItem(item, m_panel->model() && m_panel->model()->showHidden());
        m_scroll->ensureVisible(0, 0);
    }
}

// ── Live folder views ────────────────────────────────────────────────────────

Panel *PreviewPane::focusedPanel() const
{
    QWidget *focus = QApplication::focusWidget();
    if (!focus || !isAncestorOf(focus))
        return nullptr;
    for (QWidget *w = focus; w && w != this; w = w->parentWidget()) {
        if (auto *panel = qobject_cast<Panel *>(w))
            return panel;
    }
    return nullptr;
}

QList<Panel *> PreviewPane::livePanels() const
{
    return m_root->livePanels();
}

// ── Unsaved edits ────────────────────────────────────────────────────────────

bool PreviewPane::hasUnsavedChanges() const
{
    return m_root->hasUnsavedChanges();
}

bool PreviewPane::confirmDiscard()
{
    return m_root->confirmDiscard();
}

void PreviewPane::resolveUnsavedChanges()
{
    m_root->resolveUnsavedChanges();
}

// ── Geometry ─────────────────────────────────────────────────────────────────

int PreviewPane::rememberedWidth(int fallback) const
{
    return m_rememberedWidth > 0 ? m_rememberedWidth : fallback;
}

void PreviewPane::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (isVisible() && event->size().width() > 0)
        m_rememberedWidth = event->size().width();
}

void PreviewPane::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Drop content while hidden; it is re-read from the panel on show. Dirty
    // documents are the one thing kept: closing the pane went through
    // confirmDiscard(), so if one is still dirty the user chose to keep it.
    if (!hasUnsavedChanges())
        m_root->showMessage(i18n("Nothing selected"));
}

void PreviewPane::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_refreshDebounce->stop();
    refresh();
}
