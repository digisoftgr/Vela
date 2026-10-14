#include "NavigationHistory.h"

#include <utility>

void NavigationHistory::push(const QUrl &url)
{
    if (m_index >= 0 && m_history.at(m_index).url == url)
        return;
    trimCurrentPosition();
    if (m_index < m_history.size() - 1)
        m_history.resize(m_index + 1);
    m_history.append({url, std::nullopt});
    if (m_history.size() > kMaxEntries)
        m_history.remove(0, m_history.size() - kMaxEntries);
    m_index = m_history.size() - 1;
}

QUrl NavigationHistory::back()
{
    if (canGoBack()) {
        trimCurrentPosition();
        --m_index;
    }
    return current();
}

QUrl NavigationHistory::forward()
{
    if (canGoForward()) {
        trimCurrentPosition();
        ++m_index;
    }
    return current();
}

QUrl NavigationHistory::current() const
{
    if (m_index >= 0 && m_index < m_history.size())
        return m_history.at(m_index).url;
    return {};
}

bool NavigationHistory::canGoBack() const
{
    return m_index > 0;
}

bool NavigationHistory::canGoForward() const
{
    return m_index < m_history.size() - 1;
}

const std::optional<ViewPosition> &NavigationHistory::currentPosition() const
{
    static const std::optional<ViewPosition> none;
    if (m_index >= 0 && m_index < m_history.size())
        return m_history.at(m_index).position;
    return none;
}

void NavigationHistory::setCurrentPosition(std::optional<ViewPosition> position)
{
    if (m_index >= 0 && m_index < m_history.size())
        m_history[m_index].position = std::move(position);
}

std::optional<ViewPosition> NavigationHistory::takeCurrentPosition()
{
    if (m_index >= 0 && m_index < m_history.size())
        return std::exchange(m_history[m_index].position, std::nullopt);
    return std::nullopt;
}

void NavigationHistory::trimCurrentPosition()
{
    if (m_index >= 0 && m_index < m_history.size())
        trim(m_history[m_index].position);
}

void NavigationHistory::trim(std::optional<ViewPosition> &position)
{
    if (!position)
        return;
    if (position->selection.isEmpty()) {
        position.reset();
        return;
    }
    position->selection.resize(1);
    position->current = position->selection.first();
}
