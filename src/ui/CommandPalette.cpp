#include "CommandPalette.h"

#include "core/CommandRegistry.h"

#include <KActionCollection>
#include <KKeySequenceWidget>
#include <KLocalizedString>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVBoxLayout>

// ── CommandPaletteDelegate ────────────────────────────────────────────────────

CommandPaletteDelegate::CommandPaletteDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QRect CommandPaletteDelegate::shortcutButtonRect(const QRect &itemRect)
{
    const int btnW = 22;
    const int btnH = 18;
    return QRect(itemRect.right() - btnW - 4,
                 itemRect.center().y() - btnH / 2,
                 btnW, btnH);
}

void CommandPaletteDelegate::paint(QPainter *painter,
                                   const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    QStyledItemDelegate::paint(painter, option, index);

    const QString shortcutText = index.data(Qt::UserRole + 2).toString();
    const QRect btnRect = shortcutButtonRect(option.rect);

    // Shortcut badge (left of the "+" button)
    if (!shortcutText.isEmpty()) {
        const QRect scRect(option.rect.right() - btnRect.width() - 8 - 120,
                           option.rect.top(), 120, option.rect.height());
        painter->save();
        QColor scColor = option.palette.color(QPalette::WindowText);
        scColor.setAlphaF(0.55);
        painter->setPen(scColor);
        QFont f = painter->font();
        f.setPointSizeF(f.pointSizeF() * 0.85);
        painter->setFont(f);
        painter->drawText(scRect, Qt::AlignVCenter | Qt::AlignRight, shortcutText);
        painter->restore();
    }

    // "+" shortcut-assign button
    QStyleOptionButton btnOpt;
    btnOpt.rect = btnRect;
    btnOpt.text = QStringLiteral("+");
    btnOpt.state = QStyle::State_Enabled;
    if (option.state & QStyle::State_MouseOver)
        btnOpt.state |= QStyle::State_MouseOver;
    QApplication::style()->drawControl(QStyle::CE_PushButton, &btnOpt, painter);
}

QSize CommandPaletteDelegate::sizeHint(const QStyleOptionViewItem &option,
                                       const QModelIndex &index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(qMax(s.height(), 32));
    return s;
}

bool CommandPaletteDelegate::editorEvent(QEvent *event,
                                         QAbstractItemModel * /*model*/,
                                         const QStyleOptionViewItem &option,
                                         const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton
                && shortcutButtonRect(option.rect).contains(me->pos())) {
            Q_EMIT shortcutEditRequested(index);
            return true;
        }
    }
    return false;
}

// ── CommandPalette ────────────────────────────────────────────────────────────

CommandPalette::CommandPalette(CommandRegistry *registry, KActionCollection *shortcuts,
                               QWidget *parent)
    : QFrame(parent, Qt::Widget)
    , m_registry(registry)
    , m_shortcutCollection(shortcuts)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    setLineWidth(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(i18n("Type a command…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);

    m_model = new QStandardItemModel(this);

    m_listView = new QListView(this);
    m_listView->setModel(m_model);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setFrameStyle(QFrame::NoFrame);
    m_listView->setMouseTracking(true);

    m_delegate = new CommandPaletteDelegate(m_listView);
    m_listView->setItemDelegate(m_delegate);

    layout->addWidget(m_searchEdit);
    layout->addWidget(m_listView);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &CommandPalette::filterList);
    connect(m_listView, &QListView::activated, this, &CommandPalette::executeSelected);
    connect(m_delegate, &CommandPaletteDelegate::shortcutEditRequested,
            this, &CommandPalette::openShortcutEditor);

    if (parent)
        parent->installEventFilter(this);

    hide();
}

void CommandPalette::populate()
{
    const QString currentQuery = m_searchEdit->text();
    filterList(currentQuery);
}

void CommandPalette::showPalette()
{
    repositionOverParent();
    show();
    raise();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
    // Show all commands when opening.
    filterList(m_searchEdit->text());
    // Select first item.
    if (m_model->rowCount() > 0)
        m_listView->setCurrentIndex(m_model->index(0, 0));
}

// ── Event handling ────────────────────────────────────────────────────────────

bool CommandPalette::eventFilter(QObject *obj, QEvent *event)
{
    // Arrow/Enter/Escape keys from the search field drive list navigation.
    if (obj == m_searchEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_Down:
            moveSelection(+1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            executeSelected();
            return true;
        case Qt::Key_Escape:
            hide();
            m_searchEdit->clear();
            return true;
        default:
            break;
        }
    }
    // Reposition when the parent window is resized.
    if (obj == parentWidget() && event->type() == QEvent::Resize)
        repositionOverParent();
    return QFrame::eventFilter(obj, event);
}

void CommandPalette::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        hide();
        m_searchEdit->clear();
    } else {
        QFrame::keyPressEvent(event);
    }
}

void CommandPalette::showEvent(QShowEvent *event)
{
    repositionOverParent();
    QFrame::showEvent(event);
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void CommandPalette::filterList(const QString &query)
{
    m_model->clear();
    if (!m_registry)
        return;

    const QList<Command> &all = m_registry->commands();

    // Score and collect matching commands.
    struct Scored {
        int score;
        const Command *cmd;
    };
    QList<Scored> scored;
    scored.reserve(all.size());

    for (const Command &cmd : all) {
        const int s = fuzzyScore(query, cmd.displayName);
        if (s > 0)
            scored.append({s, &cmd});
    }

    // Sort: highest score first; equal scores alphabetically.
    std::stable_sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.cmd->displayName < b.cmd->displayName;
    });

    for (const auto &entry : scored) {
        const Command *cmd = entry.cmd;

        auto *item = new QStandardItem(cmd->icon, cmd->displayName);
        item->setData(cmd->id, CommandIdRole);

        // Shortcut text from the backing action (primary shortcut).
        QString sc;
        if (cmd->action && !cmd->action->shortcuts().isEmpty())
            sc = cmd->action->shortcuts().constFirst().toString(QKeySequence::NativeText);
        item->setData(sc, ShortcutTextRole);

        // Disabled commands shown but greyed out.
        if (cmd->action && !cmd->action->isEnabled())
            item->setEnabled(false);

        m_model->appendRow(item);
    }

    if (m_model->rowCount() > 0)
        m_listView->setCurrentIndex(m_model->index(0, 0));
}

void CommandPalette::executeSelected()
{
    const QModelIndex idx = m_listView->currentIndex();
    if (!idx.isValid() || !m_registry)
        return;

    const QString id = idx.data(CommandIdRole).toString();
    const Command cmd = m_registry->findById(id);

    hide();
    m_searchEdit->clear();

    if (cmd.isValid())
        cmd.trigger();
}

void CommandPalette::openShortcutEditor(const QModelIndex &index)
{
    if (!m_registry)
        return;
    const QString id = index.data(CommandIdRole).toString();
    const Command cmd = m_registry->findById(id);
    if (!cmd.action)
        return; // only QAction-backed commands can have shortcuts changed

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Assign Shortcut — %1", cmd.displayName));
    auto *dlgLayout = new QVBoxLayout(&dlg);

    auto *label = new QLabel(
        i18n("Assign shortcuts for <b>%1</b>:", cmd.displayName), &dlg);
    label->setWordWrap(true);

    auto *primaryLabel  = new QLabel(i18n("Primary:"),   &dlg);
    auto *alternateLabel = new QLabel(i18n("Alternate:"), &dlg);

    auto *primaryWidget = new KKeySequenceWidget(&dlg);
    primaryWidget->setKeySequence(
        cmd.action->shortcuts().value(0), KKeySequenceWidget::NoValidate);

    auto *alternateWidget = new KKeySequenceWidget(&dlg);
    alternateWidget->setKeySequence(
        cmd.action->shortcuts().value(1), KKeySequenceWidget::NoValidate);

    // Checked against every action in the window (the collection's actions are
    // the same in every window), plus standard and global shortcuts — the
    // widgets' defaults. An empty list here meant no check at all. Taking a
    // key from another action is offered by the widget and applied on OK.
    const QList<KActionCollection *> collections =
        m_shortcutCollection ? QList<KActionCollection *>{m_shortcutCollection}
                             : QList<KActionCollection *>{};
    primaryWidget->setCheckActionCollections(collections);
    alternateWidget->setCheckActionCollections(collections);
    // Actions losing a key to this one; they changed too, and every window
    // has to hear about it.
    QList<QAction *> stolenFrom;
    const auto noteSteal = [&stolenFrom](const QKeySequence &, QAction *action) {
        if (action && !stolenFrom.contains(action))
            stolenFrom.append(action);
    };
    connect(primaryWidget, &KKeySequenceWidget::stealShortcut, &dlg, noteSteal);
    connect(alternateWidget, &KKeySequenceWidget::stealShortcut, &dlg, noteSteal);

    auto *grid = new QGridLayout;
    grid->addWidget(primaryLabel,   0, 0);
    grid->addWidget(primaryWidget,  0, 1);
    grid->addWidget(alternateLabel, 1, 0);
    grid->addWidget(alternateWidget,1, 1);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);

    dlgLayout->addWidget(label);
    dlgLayout->addLayout(grid);
    dlgLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Clears the key from any action the user agreed to take it from.
    primaryWidget->applyStealShortcut();
    alternateWidget->applyStealShortcut();

    QList<QKeySequence> shortcuts;
    const QKeySequence primary   = primaryWidget->keySequence();
    const QKeySequence alternate = alternateWidget->keySequence();
    if (!primary.isEmpty())   shortcuts << primary;
    // The two widgets don't check against each other.
    if (!alternate.isEmpty() && alternate != primary) shortcuts << alternate;
    cmd.action->setShortcuts(shortcuts);

    // Refresh the shortcut badge in the list.
    auto *item = m_model->itemFromIndex(index);
    if (item) {
        item->setData(
            shortcuts.isEmpty() ? QString()
                                : shortcuts.constFirst().toString(QKeySequence::NativeText),
            ShortcutTextRole);
    }

    for (QAction *other : std::as_const(stolenFrom)) {
        if (other != cmd.action)
            Q_EMIT shortcutChanged(other);
    }
    Q_EMIT shortcutChanged(cmd.action);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void CommandPalette::repositionOverParent()
{
    QWidget *par = parentWidget();
    if (!par)
        return;
    // When managed by a parent layout (e.g. embedded in SettingsDialog),
    // don't manually resize/move — the layout owns our geometry.
    if (QLayout *pl = par->layout()) {
        if (pl->indexOf(this) >= 0)
            return;
    }
    const QRect pr = par->rect();
    const int w = qMin(600, qMax(300, pr.width() - 80));
    const int h = qMin(420, qMax(200, pr.height() - 160));
    resize(w, h);
    move((pr.width() - w) / 2, pr.height() / 5);
}

void CommandPalette::moveSelection(int delta)
{
    const int rows = m_model->rowCount();
    if (rows == 0)
        return;
    const int current = m_listView->currentIndex().row();
    const int next = qBound(0, current + delta, rows - 1);
    m_listView->setCurrentIndex(m_model->index(next, 0));
    m_listView->scrollTo(m_model->index(next, 0));
}

int CommandPalette::fuzzyScore(const QString &query, const QString &text)
{
    if (query.isEmpty())
        return 1; // show everything when no query

    const QString lq = query.toLower();
    const QString lt = text.toLower();

    // Substring match: strong score, earlier position wins.
    const int pos = lt.indexOf(lq);
    if (pos >= 0)
        return 300 - pos;

    // Subsequence match: accumulate score, consecutive hits get bonus.
    int qi = 0;
    int score = 0;
    int consecutive = 0;
    bool prevHit = false;

    for (int ti = 0; ti < lt.size() && qi < lq.size(); ++ti) {
        if (lt[ti] == lq[qi]) {
            consecutive = prevHit ? consecutive + 1 : 1;
            score += consecutive * 2; // consecutive bonus
            prevHit = true;
            ++qi;
        } else {
            consecutive = 0;
            prevHit = false;
        }
    }

    return (qi == lq.size()) ? score : 0; // 0 if not all query chars matched
}
