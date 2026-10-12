#pragma once

#include <QAction>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

struct Command {
    QString id;
    QString displayName;
    QIcon icon;
    // QPointer, not a raw QAction*: actions belong to a MainWindow, and
    // KMainWindow deletes itself on close. A raw pointer here outlived its
    // action and the command palette dereferenced freed memory.
    QPointer<QAction> action;
    std::function<void()> execute;       // used when action is null

    bool isValid() const { return !displayName.isEmpty(); }

    void trigger() const
    {
        if (action)
            action->trigger();
        else if (execute)
            execute();
    }
};

// Per-window registry of invokable commands.
//
// This is deliberately *not* a singleton. It used to be one, cleared and
// repopulated by every MainWindow constructor, which meant (a) the command
// palette in window 1 triggered window 2's actions, and (b) closing any window
// left the shared registry holding dangling QActions.
class CommandRegistry : public QObject
{
    Q_OBJECT

public:
    explicit CommandRegistry(QObject *parent = nullptr);

    // Register a QAction — derives id, name, icon from the action.
    void registerAction(QAction *action, const QString &idOverride = {});

    // Register a free command not backed by a QAction.
    void registerCommand(const Command &cmd);

    // Remove all commands (call before re-populating).
    void clear();

    // Commands whose backing action is still alive; entries whose action has
    // been destroyed are pruned on access.
    const QList<Command> &commands() const;
    Command findById(const QString &id) const;

Q_SIGNALS:
    void commandsChanged();

private:
    void pruneDeadActions() const;
    void rebuildIndex() const;

    mutable QList<Command> m_commands;
    mutable QHash<QString, int> m_indexById;
};
