#pragma once

#include <QFrame>
#include <QStyledItemDelegate>

class QAction;
class QKeyEvent;
class QLineEdit;
class QListView;
class QStandardItemModel;

// ── Delegate ──────────────────────────────────────────────────────────────────

class CommandPaletteDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit CommandPaletteDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

Q_SIGNALS:
    void shortcutEditRequested(const QModelIndex &index);

private:
    static QRect shortcutButtonRect(const QRect &itemRect);
};

// ── Palette ───────────────────────────────────────────────────────────────────

class CommandRegistry;
class KActionCollection;

class CommandPalette : public QFrame
{
    Q_OBJECT

public:
    // `registry` is the owning window's command registry — not a global. It must
    // outlive the palette (both are owned by the same MainWindow).
    // `shortcuts` is the collection new key sequences are checked against for
    // conflicts; it must outlive the palette as well.
    explicit CommandPalette(CommandRegistry *registry, KActionCollection *shortcuts,
                            QWidget *parent = nullptr);

    // Call after all actions are registered to (re)populate the list.
    void populate();

    void showPalette();

Q_SIGNALS:
    void shortcutChanged(QAction *action);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private Q_SLOTS:
    void filterList(const QString &query);
    void executeSelected();
    void openShortcutEditor(const QModelIndex &index);

private:
    void repositionOverParent();
    void moveSelection(int delta);

    static int fuzzyScore(const QString &query, const QString &text);

    // Item data roles
    enum Role {
        CommandIdRole = Qt::UserRole + 1,
        ShortcutTextRole,
    };

    CommandRegistry *m_registry = nullptr;
    KActionCollection *m_shortcutCollection = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListView *m_listView = nullptr;
    QStandardItemModel *m_model = nullptr;
    CommandPaletteDelegate *m_delegate = nullptr;
};
