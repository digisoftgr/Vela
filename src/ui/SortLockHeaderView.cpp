#include "SortLockHeaderView.h"

#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
SortLockHeaderView::SortLockHeaderView(Qt::Orientation orientation, QWidget *parent)
    : QHeaderView(orientation, parent)
{
}

void SortLockHeaderView::setLockedSection(int section)
{
    if (m_lockedSection == section)
        return;
    m_lockedSection = section;
    viewport()->update();
}

void SortLockHeaderView::paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const
{
    QHeaderView::paintSection(painter, rect, logicalIndex);

    if (rect.width() < kIconSize * 3)
        return;

    const bool isActive = (logicalIndex == sortIndicatorSection());
    const bool isLocked = (logicalIndex == m_lockedSection);

    // Looked up once per icon theme, not per section per paint.
    static QString cachedTheme;
    static QIcon lockedIcon, unlockedIcon;
    if (lockedIcon.isNull() || cachedTheme != QIcon::themeName()) {
        cachedTheme  = QIcon::themeName();
        lockedIcon   = QIcon::fromTheme(QStringLiteral("object-locked"));
        unlockedIcon = QIcon::fromTheme(QStringLiteral("object-unlocked"));
    }
    const QIcon &icon = isLocked ? lockedIcon : unlockedIcon;

    if (icon.isNull())
        return;

    painter->save();
    painter->setOpacity(isActive ? 1.0 : 0.35);
    icon.paint(painter, lockIconRect(rect));
    painter->restore();
}

void SortLockHeaderView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int logical = logicalIndexAt(event->pos());
        if (logical >= 0 && logical == sortIndicatorSection()) {
            const QRect sectionR(sectionViewportPosition(logical), 0,
                                 sectionSize(logical), height());
            if (lockIconRect(sectionR).contains(event->pos())) {
                Q_EMIT lockClicked(logical);
                return;
            }
        }
    }
    QHeaderView::mousePressEvent(event);
}

QRect SortLockHeaderView::lockIconRect(const QRect &sectionRect) const
{
    const int arrowSize   = style()->pixelMetric(QStyle::PM_HeaderMarkSize,  nullptr, this);
    const int arrowMargin = style()->pixelMetric(QStyle::PM_HeaderMargin,    nullptr, this);
    const int right = sectionRect.right() - arrowMargin - arrowSize - 3;
    const int top   = sectionRect.top() + (sectionRect.height() - kIconSize) / 2;
    return QRect(right - kIconSize, top, kIconSize, kIconSize);
}
