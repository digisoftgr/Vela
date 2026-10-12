#include "OpenInListEditor.h"

#include <KLocalizedString>
#include <KOpenWithDialog>
#include <KService>

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum { IdRole = Qt::UserRole + 1, SubmenuRole };

// Resolves a stored id to what the row shows. A missing app still gets a row —
// see the class comment — but one that says so.
struct Resolved {
    QString label;
    QIcon icon;
    bool installed = false;
};

Resolved resolve(const QString &desktopId)
{
    Resolved r;
    if (const KService::Ptr service = KService::serviceByStorageId(desktopId)) {
        r.label = service->name();
        r.icon = QIcon::fromTheme(service->icon(),
                                  QIcon::fromTheme(QStringLiteral("application-x-executable")));
        r.installed = true;
    } else {
        r.label = i18n("%1 (not installed)", desktopId);
        r.icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));
    }
    return r;
}

} // namespace

OpenInListEditor::OpenInListEditor(const QString &mimeType, QWidget *parent)
    : QWidget(parent)
    , m_mimeType(mimeType)
{
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({i18n("Application"), i18n("Shown as")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setMinimumHeight(120);

    m_add    = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")),    i18n("Add…"), this);
    m_remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove"), this);
    m_up     = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")),       i18n("Move Up"), this);
    m_down   = new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")),     i18n("Move Down"), this);

    auto *buttons = new QVBoxLayout;
    buttons->addWidget(m_add);
    buttons->addWidget(m_remove);
    buttons->addSpacing(8);
    buttons->addWidget(m_up);
    buttons->addWidget(m_down);
    buttons->addStretch();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree, 1);
    layout->addLayout(buttons);

    connect(m_add, &QPushButton::clicked, this, [this]() {
        // The system chooser: the tree of installed applications, filtered to
        // the MIME type when one is given, with a free command line at the
        // bottom for anything not packaged with a desktop file.
        KOpenWithDialog dialog(m_mimeType, QString(), this);
        dialog.setWindowTitle(i18n("Choose Application"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        const KService::Ptr service = dialog.service();
        if (!service)
            return;
        // No duplicates: the same app twice in one menu is never intended.
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            if (m_tree->topLevelItem(i)->data(0, IdRole).toString() == service->storageId()) {
                m_tree->setCurrentItem(m_tree->topLevelItem(i));
                return;
            }
        }
        addRow({service->storageId(), false});
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
    });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        delete m_tree->currentItem();
        updateButtons();
    });
    connect(m_up,   &QPushButton::clicked, this, [this]() { moveCurrent(-1); });
    connect(m_down, &QPushButton::clicked, this, [this]() { moveCurrent(+1); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this]() { updateButtons(); });

    updateButtons();
}

void OpenInListEditor::setApps(const QList<VelaSettings::OpenInApp> &apps)
{
    m_tree->clear();
    for (const auto &app : apps)
        addRow(app);
    updateButtons();
}

QList<VelaSettings::OpenInApp> OpenInListEditor::apps() const
{
    QList<VelaSettings::OpenInApp> result;
    result.reserve(m_tree->topLevelItemCount());
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        result.append({item->data(0, IdRole).toString(), item->data(0, SubmenuRole).toBool()});
    }
    return result;
}

void OpenInListEditor::addRow(const VelaSettings::OpenInApp &app)
{
    const Resolved r = resolve(app.desktopId);
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, r.label);
    item->setIcon(0, r.icon);
    item->setToolTip(0, app.desktopId);
    item->setData(0, IdRole, app.desktopId);
    item->setData(0, SubmenuRole, app.inSubmenu);
    if (!r.installed) {
        // Greyed by colour, not by Qt::ItemIsEnabled: a disabled item cannot be
        // selected, and this row has to stay selectable so it can be removed.
        item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
    }
    attachPlacementCombo(item);
}

void OpenInListEditor::attachPlacementCombo(QTreeWidgetItem *item)
{
    // The combo is an item widget, which QTreeWidget discards when the item is
    // taken out of the tree — so it is (re)built from the item's data every time
    // the row lands in the tree, and the data is the single source of truth.
    auto *combo = new QComboBox(m_tree);
    combo->addItem(i18n("Entry in the menu"));
    combo->addItem(i18n("Inside “Open In” submenu"));
    combo->setCurrentIndex(item->data(0, SubmenuRole).toBool() ? 1 : 0);
    connect(combo, &QComboBox::currentIndexChanged, this, [item](int index) {
        item->setData(0, SubmenuRole, index == 1);
    });
    m_tree->setItemWidget(item, 1, combo);
}

void OpenInListEditor::moveCurrent(int delta)
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const int row = m_tree->indexOfTopLevelItem(item);
    const int target = row + delta;
    if (target < 0 || target >= m_tree->topLevelItemCount())
        return;
    m_tree->takeTopLevelItem(row);
    m_tree->insertTopLevelItem(target, item);
    attachPlacementCombo(item); // lost with the take, see above
    m_tree->setCurrentItem(item);
    updateButtons();
}

void OpenInListEditor::updateButtons()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    const int row = item ? m_tree->indexOfTopLevelItem(item) : -1;
    m_remove->setEnabled(row >= 0);
    m_up->setEnabled(row > 0);
    m_down->setEnabled(row >= 0 && row < m_tree->topLevelItemCount() - 1);
}
