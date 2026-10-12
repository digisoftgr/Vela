#include "DropZoneOverlay.h"
#include "Panel.h"

#include <QPainter>

DropZoneOverlay::DropZoneOverlay(Panel *panel, QWidget *parent)
    : QWidget(parent)
    , m_panel(panel)
{
    setAttribute(Qt::WA_TranslucentBackground); // composites correctly over parent content
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

DropZoneOverlay::DropZone DropZoneOverlay::zoneAtPoint(const QPoint &pos) const
{
    const int w = width();
    const int h = height();
    const int stripW = w / 4;
    const int stripH = h / 4;

    if (pos.x() < stripW)
        return DropZone::Left;
    if (pos.x() >= w - stripW)
        return DropZone::Right;
    if (pos.y() < stripH)
        return DropZone::Top;
    if (pos.y() >= h - stripH)
        return DropZone::Bottom;
    return DropZone::Center;
}

QRect DropZoneOverlay::zoneRect(DropZone zone) const
{
    const int w = width();
    const int h = height();
    const int stripW = w / 4;
    const int stripH = h / 4;

    // Left/Right span full height; Top/Bottom avoid the corners occupied by Left/Right.
    switch (zone) {
    case DropZone::Left:   return QRect(0,              0,          stripW,           h);
    case DropZone::Right:  return QRect(w - stripW,     0,          stripW,           h);
    case DropZone::Top:    return QRect(stripW,          0,          w - 2 * stripW,   stripH);
    case DropZone::Bottom: return QRect(stripW,          h - stripH, w - 2 * stripW,   stripH);
    case DropZone::Center: return QRect(stripW,          stripH,     w - 2 * stripW,   h - 2 * stripH);
    default:               return {};
    }
}

void DropZoneOverlay::setHoveredZone(DropZone zone)
{
    if (zone == m_hoveredZone)
        return;
    m_hoveredZone = zone;
    update();
}

void DropZoneOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const DropZone zones[] = {
        DropZone::Left, DropZone::Right, DropZone::Top,
        DropZone::Bottom, DropZone::Center
    };

    const QColor base(100, 149, 237, 60);
    const QColor hovered(100, 149, 237, 150);
    const QColor border(100, 149, 237, 220);

    for (DropZone zone : zones) {
        const QRect r = zoneRect(zone);
        p.fillRect(r, (zone == m_hoveredZone) ? hovered : base);
        p.setPen(QPen(border, 2));
        p.drawRect(r.adjusted(1, 1, -1, -1));
    }
}
