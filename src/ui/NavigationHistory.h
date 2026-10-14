#pragma once

#include <QList>
#include <QUrl>

#include <optional>

// Where the view was in a directory when it was left: what was selected and
// how far it was scrolled.
struct ViewPosition {
    // In view order, topmost first. Only the current entry of a tab may hold
    // more than one item (a background tab keeps its whole selection); every
    // other entry is trimmed to the topmost one — a Ctrl+A in a huge directory
    // must not be kept fifty times over.
    QList<QUrl> selection;
    QUrl        current;
    int         scrollX = 0;
    int         scrollY = 0;
    int         viewMode = 0; // Panel::ViewMode; pixel offsets mean nothing in another mode
};

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

    // The position stored on the current entry.
    const std::optional<ViewPosition> &currentPosition() const;
    void setCurrentPosition(std::optional<ViewPosition> position);
    std::optional<ViewPosition> takeCurrentPosition();
    // Reduces the current entry's position to what a non-current entry keeps.
    void trimCurrentPosition();

    // Topmost selected item only; nothing at all when nothing was selected.
    static void trim(std::optional<ViewPosition> &position);

private:
    struct Entry {
        QUrl url;
        std::optional<ViewPosition> position;
    };
    QList<Entry> m_history;
    int m_index = -1;
};
