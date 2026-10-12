#include "NavigationHistory.h"

void NavigationHistory::push(const QUrl &url)
{
    if (m_index >= 0 && m_history.at(m_index) == url)
        return;
    if (m_index < m_history.size() - 1)
        m_history.resize(m_index + 1);
    m_history.append(url);
    if (m_history.size() > kMaxEntries)
        m_history.remove(0, m_history.size() - kMaxEntries);
    m_index = m_history.size() - 1;
}

QUrl NavigationHistory::back()
{
    if (canGoBack())
        --m_index;
    return current();
}

QUrl NavigationHistory::forward()
{
    if (canGoForward())
        ++m_index;
    return current();
}

QUrl NavigationHistory::current() const
{
    if (m_index >= 0 && m_index < m_history.size())
        return m_history.at(m_index);
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
