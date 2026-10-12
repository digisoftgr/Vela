#pragma once

#include "core/VelaSettings.h"

#include <QList>
#include <QWidget>

class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Settings editor for one "Open in …" list: the applications pinned into a
// context menu, in menu order, each either flat or under the "Open In" submenu.
//
// Applications are picked with KOpenWithDialog — the same chooser as
// "Open With → Other Application" — and stored as desktop-entry ids. An id that
// no longer resolves (the app was uninstalled) stays in the list, greyed and
// labelled, rather than vanishing: a default that silently disappears from the
// menu is the confusing case, and the row must remain removable.
class OpenInListEditor : public QWidget
{
    Q_OBJECT

public:
    // mimeType narrows the chooser to apps declaring it ("inode/directory");
    // empty shows every application.
    explicit OpenInListEditor(const QString &mimeType, QWidget *parent = nullptr);

    void setApps(const QList<VelaSettings::OpenInApp> &apps);
    QList<VelaSettings::OpenInApp> apps() const;

private:
    void addRow(const VelaSettings::OpenInApp &app);
    void attachPlacementCombo(QTreeWidgetItem *item);
    void moveCurrent(int delta);
    void updateButtons();

    QString m_mimeType;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
};
