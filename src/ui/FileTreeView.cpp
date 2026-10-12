#include "FileTreeView.h"

#include "core/FileSystemModel.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionRubberBand>

FileTreeView::FileTreeView(QWidget *parent)
    : QTreeView(parent)
{
}

// ── Hit testing ──────────────────────────────────────────────────────────────

QPoint FileTreeView::contentOffset() const
{
    return QPoint(horizontalOffset(), verticalOffset());
}

// ── Mouse ────────────────────────────────────────────────────────────────────

void FileTreeView::mousePressEvent(QMouseEvent *event)
{
    // A drag that starts on a row moves those files — the press selects the row
    // first, so this holds whether or not it was selected beforehand. A band is
    // only ever drawn from a spot holding no row at all.
    m_bandCandidate = event->button() == Qt::LeftButton
                   && selectionMode() != NoSelection
                   && selectionMode() != SingleSelection
                   && !indexAt(event->pos()).isValid();
    m_pressPos = event->pos();
    QTreeView::mousePressEvent(event);
}

void FileTreeView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_bandActive) {
        m_bandCursor = event->pos();

        // Autoscroll once the cursor reaches the viewport margin. The timer
        // stops itself when nothing moves, so re-arming per event is cheap.
        const QRect area = viewport()->rect();
        const int margin = autoScrollMargin();
        if (m_bandCursor.y() - area.top() < margin || area.bottom() - m_bandCursor.y() < margin
            || m_bandCursor.x() - area.left() < margin || area.right() - m_bandCursor.x() < margin)
            startAutoScroll();

        updateBand(m_bandCursor);
        event->accept();
        return;
    }

    if (m_bandCandidate && (event->buttons() & Qt::LeftButton)) {
        // Held short of the threshold this is still a click, so the event is
        // swallowed rather than handed on: QTreeView would read it as the start
        // of a row-range sweep, which is the behaviour being replaced.
        if ((event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_bandCursor = event->pos();
            startBand();
            updateBand(m_bandCursor);
        }
        event->accept();
        return;
    }

    QTreeView::mouseMoveEvent(event);
}

void FileTreeView::mouseReleaseEvent(QMouseEvent *event)
{
    m_bandCandidate = false;
    if (m_bandActive) {
        // The band already set the selection; handing the release to QTreeView
        // would emit clicked() for whatever row the cursor ended on — which in
        // single-click mode opens it.
        endBand();
        event->accept();
        return;
    }
    QTreeView::mouseReleaseEvent(event);
}

// ── Rubber band ──────────────────────────────────────────────────────────────

void FileTreeView::startBand()
{
    m_bandActive = true;
    m_bandOrigin = m_pressPos + contentOffset();
    setState(DragSelectingState);
}

void FileTreeView::endBand()
{
    stopAutoScroll();
    setState(NoState);
    m_bandActive = false;
    if (m_bandRect.isValid())
        viewport()->update(m_bandRect.adjusted(-1, -1, 1, 1));
    m_bandRect = QRect();
}

void FileTreeView::updateBand(const QPoint &viewportPos)
{
    const QRect area = viewport()->rect();
    const QPoint cursor(qBound(area.left(), viewportPos.x(), area.right()),
                        qBound(area.top(), viewportPos.y(), area.bottom()));

    const QRect oldRect = m_bandRect;
    m_bandRect = QRect(m_bandOrigin - contentOffset(), cursor).normalized();
    viewport()->update(oldRect.united(m_bandRect).adjusted(-1, -1, 1, 1));

    // Rows the band overlaps, the whole row counting as the target: touching
    // any part of it selects it. Worked out in content coordinates from the row
    // height, not by walking the rows on screen: once autoscroll has carried
    // the band's origin out of view, those rows are still inside the band and
    // must stay selected (the walk only saw visible rows, and ClearAndSelect
    // then dropped the rest). The listing is flat with uniform row heights, so
    // row r spans [r*h, (r+1)*h) and the range is two divisions.
    QItemSelection inBand;
    const int rows = model() ? model()->rowCount(rootIndex()) : 0;
    const QModelIndex first = rows > 0 ? model()->index(0, FileSystemModel::Name, rootIndex())
                                       : QModelIndex();
    const int rowH = first.isValid() ? rowHeight(first) : 0;
    if (rowH > 0) {
        const int offY = verticalOffset();
        const int top    = m_bandRect.top() + offY;
        const int bottom = m_bandRect.bottom() + offY;
        const int firstRow = qMax(0, top / rowH);
        const int lastRow  = qMin(rows - 1, bottom / rowH);
        if (bottom >= 0 && firstRow <= lastRow) {
            inBand.select(model()->index(firstRow, 0, rootIndex()),
                          model()->index(lastRow, model()->columnCount(rootIndex()) - 1,
                                         rootIndex()));
        }
    }

    selectionModel()->select(inBand,
                             QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void FileTreeView::scrollContentsBy(int dx, int dy)
{
    QTreeView::scrollContentsBy(dx, dy);
    // The origin is fixed in content space, so scrolling changes which rows the
    // band covers even with the mouse still. This also serves autoscroll.
    if (m_bandActive)
        updateBand(m_bandCursor);
}

void FileTreeView::paintEvent(QPaintEvent *event)
{
    QTreeView::paintEvent(event);
    if (!m_bandActive || !m_bandRect.isValid())
        return;

    QStyleOptionRubberBand opt;
    opt.initFrom(this);
    opt.shape = QRubberBand::Rectangle;
    opt.opaque = false;
    opt.rect = m_bandRect;
    QPainter painter(viewport());
    style()->drawControl(QStyle::CE_RubberBand, &opt, &painter, this);
}
