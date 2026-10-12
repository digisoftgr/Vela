#pragma once

#include <KConfigGroup>
#include <QWidget>
#include <Qt>

class Panel;
class QSplitter;

/**
 * PanelContainer is either:
 *   - A leaf node: directly wraps a single Panel
 *   - A split node: contains a QSplitter with two PanelContainer children
 */
class PanelContainer : public QWidget
{
    Q_OBJECT

public:
    explicit PanelContainer(Panel *panel, QWidget *parent = nullptr);

    // Returns the leaf Panel if this is a leaf, nullptr otherwise.
    Panel *leafPanel() const;

    // Collect all leaf panels recursively.
    QList<Panel *> allPanels() const;

    // Panels whose top edge touches the top of this container, in left-to-right
    // visual order — i.e. exactly the panels whose tab bar belongs in the window
    // header. Correspondingly for the bottom edge and the global status bar.
    //
    // Derived from the split tree, not from pixel coordinates. Note this cannot
    // be expressed as a list of "columns": in V( H(a,b), c ) the top half has two
    // side-by-side panels while the bottom spans the full width, so there is no
    // consistent column grid. Edge reachability is the property that actually
    // matters and it is well defined for any tree.
    QList<Panel *> topEdgePanels() const    { return edgePanels(true); }
    QList<Panel *> bottomEdgePanels() const { return edgePanels(false); }

    // Split this container (must be a leaf), placing newPanel at the given edge.
    // This container becomes a split node; the original panel stays on the opposite side.
    void splitWith(Panel *newPanel, Qt::Edge edge);

    // Find the leaf container holding targetPanel and split it with newPanel.
    // Returns true if targetPanel was found and split.
    bool splitPanel(Panel *targetPanel, Panel *newPanel, Qt::Edge edge);

    // Remove a panel. If the sibling remains alone, collapse the splitter.
    // Returns true if successfully removed.
    bool removePanel(Panel *panel);

    // Serialise the full split tree + panel tab data into group.
    void saveLayout(KConfigGroup group) const;

    // Deserialise the tree from group into this container (must be a leaf on entry).
    // Creates additional Panel instances as needed and returns them so the caller
    // can call connectPanel() / extractTabBarIntoHeader() on each.
    QList<Panel *> restoreLayout(const KConfigGroup &group);

Q_SIGNALS:
    void panelAdded(Panel *panel);
    void panelRemoved(Panel *panel);
    // Emitted when an internal splitter moves (any depth).
    void layoutChanged();

private:
    // Leaf state
    Panel *m_panel = nullptr;

    // Split state
    QSplitter *m_splitter = nullptr;
    PanelContainer *m_first = nullptr;
    PanelContainer *m_second = nullptr;

    bool isLeaf() const { return m_panel != nullptr; }

    // topEdge=true walks the top edge, false the bottom. See topEdgePanels().
    QList<Panel *> edgePanels(bool topEdge) const;

    // Reparent a child container's contents into this one (collapse step).
    void absorb(PanelContainer *child);
};
