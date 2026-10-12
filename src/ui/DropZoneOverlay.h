#pragma once

#include <QWidget>

class Panel;

class DropZoneOverlay : public QWidget
{
    Q_OBJECT

public:
    enum class DropZone { None, Center, Left, Right, Top, Bottom };

    explicit DropZoneOverlay(Panel *panel, QWidget *parent = nullptr);

    Panel *targetPanel() const { return m_panel; }

    // Hit-test a local point against the zone grid.
    DropZone zoneAtPoint(const QPoint &localPos) const;

    // Update the highlighted zone and repaint.
    void setHoveredZone(DropZone zone);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRect zoneRect(DropZone zone) const;

    Panel *m_panel = nullptr;
    DropZone m_hoveredZone = DropZone::None;
};
