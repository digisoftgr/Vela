#include "MessageBoxUtil.h"

#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QTimer>

namespace {

// QMessageBox is rigid by design: QMessageBoxPrivate::updateSize() ends in a
// setFixedSize() on the dialog, so clearing the layout's size constraint or
// adding a size grip achieves nothing on its own — the minimum and the maximum
// are still pinned to the same value. updateSize() runs on show and again on
// every layout request (expanding "Show Details" is one), so the fixed size has
// to be lifted after each, not once up front.
class SizeUnfixer : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Show && event->type() != QEvent::LayoutRequest)
            return QObject::eventFilter(watched, event);

        // Queued: the filter runs before the dialog's own handler, which is
        // what sets the fixed size in the first place.
        auto *box = qobject_cast<QWidget *>(watched);
        QTimer::singleShot(0, box, [box]() {
            // Only when it is actually pinned. Lifting the limits emits another
            // layout request, and re-entering unconditionally would keep the
            // timer firing for as long as the dialog is open.
            if (box->minimumSize() != box->maximumSize())
                return;
            box->setMinimumSize(0, 0);
            box->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        });
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

void prepareMessageBox(QMessageBox &box, int widenFactor)
{
    if (auto *grid = qobject_cast<QGridLayout *>(box.layout())) {
        // Height 0: this widens, it must not add vertical space.
        grid->addItem(new QSpacerItem(box.sizeHint().width() * widenFactor, 0,
                                      QSizePolicy::Minimum, QSizePolicy::Expanding),
                      grid->rowCount(), 0, 1, grid->columnCount());
    }

    // Without this the message label keeps its full single-line width as a
    // minimum, so the dialog would resize wider but never narrower again.
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label")))
        label->setWordWrap(true);

    box.setSizeGripEnabled(true);
    box.installEventFilter(new SizeUnfixer(&box));
}
