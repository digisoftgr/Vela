#pragma once

#include <QList>
#include <QUrl>

class NavigationHistory
{
public:
    // Oldest entries are dropped beyond this; it used to grow without bound.
    static constexpr int kMaxEntries = 50;

    void push(const QUrl &url);
    QUrl back();
    QUrl forward();
    QUrl current() const;
    bool canGoBack() const;
    bool canGoForward() const;

private:
    QList<QUrl> m_history;
    int m_index = -1;
};
