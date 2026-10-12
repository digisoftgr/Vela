#include "CommandRegistry.h"

#include <QAction>

CommandRegistry::CommandRegistry(QObject *parent)
    : QObject(parent)
{
}

void CommandRegistry::registerAction(QAction *action, const QString &idOverride)
{
    if (!action || action->isSeparator())
        return;

    QString text = action->text().remove(QLatin1Char('&')).trimmed();
    if (text.isEmpty())
        return;

    const QString id = idOverride.isEmpty()
                     ? (action->objectName().isEmpty() ? text : action->objectName())
                     : idOverride;

    // Update in place if already registered.
    const auto it = m_indexById.constFind(id);
    if (it != m_indexById.constEnd()) {
        Command &cmd = m_commands[it.value()];
        cmd.action = action;
        cmd.displayName = text;
        cmd.icon = action->icon();
        cmd.execute = {}; // the QPointer path handles triggering
        Q_EMIT commandsChanged();
        return;
    }

    Command cmd;
    cmd.id = id;
    cmd.displayName = text;
    cmd.icon = action->icon();
    cmd.action = action;
    // No execute functor: Command::trigger() goes through the QPointer, so a
    // destroyed action becomes a no-op rather than a dangling capture.

    m_indexById[id] = m_commands.size();
    m_commands.append(std::move(cmd));
    Q_EMIT commandsChanged();
}

void CommandRegistry::registerCommand(const Command &cmd)
{
    if (!cmd.isValid())
        return;

    const auto it = m_indexById.constFind(cmd.id);
    if (it != m_indexById.constEnd()) {
        m_commands[it.value()] = cmd;
        Q_EMIT commandsChanged();
        return;
    }

    m_indexById[cmd.id] = m_commands.size();
    m_commands.append(cmd);
    Q_EMIT commandsChanged();
}

void CommandRegistry::clear()
{
    m_commands.clear();
    m_indexById.clear();
    Q_EMIT commandsChanged();
}

void CommandRegistry::rebuildIndex() const
{
    m_indexById.clear();
    m_indexById.reserve(m_commands.size());
    for (int i = 0; i < m_commands.size(); ++i)
        m_indexById.insert(m_commands.at(i).id, i);
}

// Belt-and-braces: a registry is owned by its window, so its actions should
// outlive it. Anything that slips through (an action deleted early, a stale
// free command) is dropped here rather than handed to the palette.
void CommandRegistry::pruneDeadActions() const
{
    const int before = m_commands.size();
    m_commands.removeIf([](const Command &c) {
        return c.action.isNull() && !c.execute;
    });
    if (m_commands.size() != before)
        rebuildIndex();
}

const QList<Command> &CommandRegistry::commands() const
{
    pruneDeadActions();
    return m_commands;
}

Command CommandRegistry::findById(const QString &id) const
{
    pruneDeadActions();
    const auto it = m_indexById.constFind(id);
    if (it != m_indexById.constEnd())
        return m_commands.at(it.value());
    return {};
}
