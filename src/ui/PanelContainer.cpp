#include "PanelContainer.h"
#include "Panel.h"

#include <QHBoxLayout>
#include <QSplitter>
#include <QVBoxLayout>

PanelContainer::PanelContainer(Panel *panel, QWidget *parent)
    : QWidget(parent)
    , m_panel(panel)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (m_panel) {
        m_panel->setParent(this);
        layout->addWidget(m_panel);
    }
}

Panel *PanelContainer::leafPanel() const
{
    return m_panel;
}

QList<Panel *> PanelContainer::allPanels() const
{
    if (isLeaf())
        return {m_panel};

    QList<Panel *> result;
    if (m_first)
        result += m_first->allPanels();
    if (m_second)
        result += m_second->allPanels();
    return result;
}

// Which panels touch the top (or bottom) edge of this container.
//
// Side-by-side children both touch both edges, so a horizontal splitter
// contributes all of its children. A vertical splitter stacks its children, so
// only the first touches the top and only the last touches the bottom.
//
// Splitter child *index* order is authoritative — m_first/m_second do not track
// visual order, because splitting toward a left/top edge inserts the new panel
// at index 0.
QList<Panel *> PanelContainer::edgePanels(bool topEdge) const
{
    if (isLeaf())
        return m_panel ? QList<Panel *>{m_panel} : QList<Panel *>{};

    if (!m_splitter || m_splitter->count() == 0)
        return {};

    QList<Panel *> result;
    if (m_splitter->orientation() == Qt::Horizontal) {
        for (int i = 0; i < m_splitter->count(); ++i) {
            if (auto *child = qobject_cast<PanelContainer *>(m_splitter->widget(i)))
                result += child->edgePanels(topEdge);
        }
    } else {
        const int idx = topEdge ? 0 : m_splitter->count() - 1;
        if (auto *child = qobject_cast<PanelContainer *>(m_splitter->widget(idx)))
            result += child->edgePanels(topEdge);
    }
    return result;
}

bool PanelContainer::splitPanel(Panel *targetPanel, Panel *newPanel, Qt::Edge edge)
{
    if (isLeaf()) {
        if (m_panel == targetPanel) {
            splitWith(newPanel, edge);
            return true;
        }
        return false;
    }

    if (m_first && m_first->splitPanel(targetPanel, newPanel, edge))
        return true;
    if (m_second && m_second->splitPanel(targetPanel, newPanel, edge))
        return true;
    return false;
}

void PanelContainer::splitWith(Panel *newPanel, Qt::Edge edge)
{
    Q_ASSERT(isLeaf());

    // Determine splitter orientation
    const Qt::Orientation orientation =
        (edge == Qt::LeftEdge || edge == Qt::RightEdge)
        ? Qt::Horizontal
        : Qt::Vertical;

    // Take the current panel out
    Panel *existingPanel = m_panel;
    m_panel = nullptr;

    // Clear current layout
    QLayout *oldLayout = layout();
    QLayoutItem *item;
    while ((item = oldLayout->takeAt(0)) != nullptr) {
        delete item;
    }

    // Become a split node
    m_splitter = new QSplitter(orientation, this);
    m_splitter->setChildrenCollapsible(false);

    m_first  = new PanelContainer(existingPanel, m_splitter);
    m_second = new PanelContainer(newPanel, m_splitter);

    // Connect child signals up
    connect(m_first,  &PanelContainer::panelAdded,     this, &PanelContainer::panelAdded);
    connect(m_first,  &PanelContainer::panelRemoved,   this, &PanelContainer::panelRemoved);
    connect(m_first,  &PanelContainer::layoutChanged,  this, &PanelContainer::layoutChanged);
    connect(m_second, &PanelContainer::panelAdded,     this, &PanelContainer::panelAdded);
    connect(m_second, &PanelContainer::panelRemoved,   this, &PanelContainer::panelRemoved);
    connect(m_second, &PanelContainer::layoutChanged,  this, &PanelContainer::layoutChanged);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() { Q_EMIT layoutChanged(); });

    // new panel goes on the edge side
    if (edge == Qt::LeftEdge || edge == Qt::TopEdge) {
        m_splitter->insertWidget(0, m_second); // newPanel on left/top
        m_splitter->insertWidget(1, m_first);  // existing on right/bottom
    } else {
        m_splitter->insertWidget(0, m_first);  // existing on left/top
        m_splitter->insertWidget(1, m_second); // newPanel on right/bottom
    }

    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);

    oldLayout->addWidget(m_splitter);

    Q_EMIT panelAdded(newPanel);
}

void PanelContainer::absorb(PanelContainer *child)
{
    // Take everything from child into this
    QLayout *myLayout = layout();
    QLayoutItem *item;
    while ((item = myLayout->takeAt(0)) != nullptr) {
        delete item;
    }

    if (child->isLeaf()) {
        m_panel = child->m_panel;
        if (m_panel)
            m_panel->setParent(this);
        m_splitter = nullptr;
        m_first = nullptr;
        m_second = nullptr;
        myLayout->addWidget(m_panel);
    } else {
        m_splitter = child->m_splitter;
        m_first = child->m_first;
        m_second = child->m_second;
        m_panel = nullptr;

        // The absorbed subtree's signals were routed through `child`, which is
        // about to be deleted. Detach them and re-route to this container.
        disconnect(child, nullptr, this, nullptr);

        if (m_splitter) {
            m_splitter->setParent(this);
            // Re-establish the splitter's own layoutChanged emission: dragging a
            // divider inside the absorbed subtree has to keep repositioning the
            // tab bars, which silently stopped working when this was omitted.
            connect(m_splitter, &QSplitter::splitterMoved,
                    this, [this]() { Q_EMIT layoutChanged(); });
        }

        const auto adopt = [this](PanelContainer *c) {
            if (!c)
                return;
            c->setParent(m_splitter);
            connect(c, &PanelContainer::panelAdded,    this, &PanelContainer::panelAdded);
            connect(c, &PanelContainer::panelRemoved,  this, &PanelContainer::panelRemoved);
            connect(c, &PanelContainer::layoutChanged, this, &PanelContainer::layoutChanged);
        };
        adopt(m_first);
        adopt(m_second);

        myLayout->addWidget(m_splitter);
    }

    // Detach child's internals so they won't be deleted with it
    child->m_panel = nullptr;
    child->m_splitter = nullptr;
    child->m_first = nullptr;
    child->m_second = nullptr;
    child->deleteLater();
}

bool PanelContainer::removePanel(Panel *panel)
{
    if (isLeaf()) {
        // Should not remove the only panel this way; caller handles
        return false;
    }

    // Check if one of our direct children is the leaf with this panel
    auto tryRemoveFromChild = [&](PanelContainer *childToRemove, PanelContainer *sibling) -> bool {
        if (!childToRemove->isLeaf() || childToRemove->leafPanel() != panel)
            return false;

        // Remove it
        childToRemove->m_panel->setParent(nullptr);
        Q_EMIT panelRemoved(panel);

        // Absorb the sibling into ourselves
        absorb(sibling);
        return true;
    };

    if (tryRemoveFromChild(m_first, m_second))
        return true;
    if (tryRemoveFromChild(m_second, m_first))
        return true;

    // Recurse
    if (m_first && m_first->removePanel(panel))
        return true;
    if (m_second && m_second->removePanel(panel))
        return true;

    return false;
}

void PanelContainer::saveLayout(KConfigGroup group) const
{
    if (isLeaf()) {
        group.writeEntry("Type", QStringLiteral("leaf"));
        m_panel->saveSettings(group);
        return;
    }

    group.writeEntry("Type", QStringLiteral("split"));
    group.writeEntry("Orientation",
        m_splitter->orientation() == Qt::Horizontal ? QStringLiteral("H") : QStringLiteral("V"));
    group.writeEntry("SplitterSizes", m_splitter->sizes());
    // Record which splitter index m_first occupies so visual order is preserved on restore.
    group.writeEntry("FirstIndex", m_splitter->indexOf(m_first));
    m_first->saveLayout(group.group(QStringLiteral("First")));
    m_second->saveLayout(group.group(QStringLiteral("Second")));
}

QList<Panel *> PanelContainer::restoreLayout(const KConfigGroup &group)
{
    const QString type = group.readEntry("Type", QStringLiteral("leaf"));

    if (type == QLatin1String("leaf")) {
        m_panel->restoreSettings(const_cast<KConfigGroup &>(group));
        return {};
    }

    const Qt::Orientation orient =
        group.readEntry("Orientation", QStringLiteral("H")) == QLatin1String("H")
        ? Qt::Horizontal : Qt::Vertical;
    const QList<int> sizes    = group.readEntry("SplitterSizes", QList<int>());
    const int        firstIdx = group.readEntry("FirstIndex", 0);

    // Detach the existing panel — it becomes m_first.
    Panel *existingPanel = m_panel;
    m_panel = nullptr;

    QLayout *oldLayout = layout();
    QLayoutItem *item;
    while ((item = oldLayout->takeAt(0)) != nullptr)
        delete item;

    m_splitter = new QSplitter(orient, this);
    m_splitter->setChildrenCollapsible(false);

    auto *newPanel = new Panel();
    newPanel->applyColumnSettings();
    m_first  = new PanelContainer(existingPanel, m_splitter);
    m_second = new PanelContainer(newPanel,      m_splitter);

    connect(m_first,  &PanelContainer::panelAdded,    this, &PanelContainer::panelAdded);
    connect(m_first,  &PanelContainer::panelRemoved,  this, &PanelContainer::panelRemoved);
    connect(m_first,  &PanelContainer::layoutChanged, this, &PanelContainer::layoutChanged);
    connect(m_second, &PanelContainer::panelAdded,    this, &PanelContainer::panelAdded);
    connect(m_second, &PanelContainer::panelRemoved,  this, &PanelContainer::panelRemoved);
    connect(m_second, &PanelContainer::layoutChanged, this, &PanelContainer::layoutChanged);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() { Q_EMIT layoutChanged(); });

    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);

    // Insert preserving the original visual order.
    if (firstIdx == 0) {
        m_splitter->insertWidget(0, m_first);
        m_splitter->insertWidget(1, m_second);
    } else {
        m_splitter->insertWidget(0, m_second);
        m_splitter->insertWidget(1, m_first);
    }

    oldLayout->addWidget(m_splitter);
    Q_EMIT panelAdded(newPanel);

    QList<Panel *> newPanels = {newPanel};
    newPanels += m_first->restoreLayout(group.group(QStringLiteral("First")));
    newPanels += m_second->restoreLayout(group.group(QStringLiteral("Second")));

    if (sizes.size() == 2)
        m_splitter->setSizes(sizes);

    return newPanels;
}
