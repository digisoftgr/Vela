#pragma once

#include <QList>
#include <QUrl>
#include <QWidget>

class QLineEdit;
class QToolButton;

// The address bar: a strip of clickable directory buttons that turns into an
// editable path field on demand.
//
// Two states share one widget rather than living in a QStackedWidget, because
// the edit field has to occupy the whole bar — including the empty space to the
// right of the last crumb, which is the click target that summons it.
//
// Layout is done by hand in relayout() instead of with a QHBoxLayout: the
// overflow rule runs backwards. Crumbs are fitted from the *last* one towards
// the first, so a path too long for the bar drops its leading segments (which
// stay reachable through the overflow menu) rather than eliding its tail, which
// is the part that says where you actually are.
class BreadcrumbBar : public QWidget
{
    Q_OBJECT

public:
    explicit BreadcrumbBar(QWidget *parent = nullptr);

    void setUrl(const QUrl &url);
    QUrl url() const { return m_url; }

    // True while the path field is up. Callers use this to keep shortcuts that
    // belong to the file view (Ctrl+C/X/V) out of the text field.
    bool isEditing() const;

    void enterEditMode(); // shows the field with the whole path selected
    void leaveEditMode(); // discards any edit and returns to the crumbs

Q_SIGNALS:
    void urlActivated(const QUrl &url);
    void urlActivatedInNewTab(const QUrl &url);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    QSize sizeHint() const override;

private:
    struct Crumb {
        QString label;   // what the button shows
        QString full;    // untruncated, for the tooltip
        QUrl url;        // where clicking it goes
        QToolButton *button = nullptr;
    };

    void rebuildCrumbs();
    void relayout();
    void showOverflowMenu();
    QString pathText() const;

    QList<Crumb> m_crumbs;
    QToolButton *m_overflowBtn = nullptr;
    QLineEdit *m_lineEdit = nullptr;
    QUrl m_url;

    // Gap between crumbs, in which the separator chevron is painted.
    static constexpr int kSeparatorWidth = 14;
    // Clickable dead space kept to the right of the last crumb, so entering
    // edit mode never requires hitting a 2px sliver.
    static constexpr int kTrailingClickWidth = 28;
};
