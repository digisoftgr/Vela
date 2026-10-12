#pragma once

#include <QTreeView>

// Details view that offers a rubber-band selection. QTreeView has none: a
// left-drag either starts a file drag or extends a row range, so there is no
// way to sweep a rectangle over a listing the way every file manager allows.
//
// Clicking is untouched — press, release and double-click go to QTreeView, so a
// click anywhere on a row selects and opens it exactly as before. Only a drag
// is intercepted, and only when it starts on empty space rather than on a row:
// a drag off a row moves files, as before.
class FileTreeView : public QTreeView
{
    Q_OBJECT
public:
    explicit FileTreeView(QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    QPoint contentOffset() const;
    void startBand();
    void updateBand(const QPoint &viewportPos);
    void endBand();

    bool   m_bandCandidate = false; // press landed where a drag means "band"
    bool   m_bandActive = false;    // past the drag threshold, band is live
    QPoint m_pressPos;              // viewport coordinates
    QPoint m_bandOrigin;            // content coordinates: survives scrolling
    QPoint m_bandCursor;            // viewport coordinates, last seen
    QRect  m_bandRect;              // viewport coordinates
};
