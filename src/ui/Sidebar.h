#pragma once

#include <QUrl>
#include <QWidget>

class KFilePlacesModel;
class KFilePlacesView;
class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QSplitter;

class Sidebar : public QWidget
{
    Q_OBJECT

public:
    explicit Sidebar(QWidget *parent = nullptr);
    ~Sidebar() override;

    // The recent list is application-wide: a visit in any window shows up in
    // every window's sidebar, and one list is saved.
    void recordVisit(const QUrl &url);
    void setCurrentUrl(const QUrl &url);
    void setTrackRecent(bool enabled);

    void saveSettings() const;
    void restoreSettings();

Q_SIGNALS:
    void placeActivated(const QUrl &url, bool openInNewTab);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private Q_SLOTS:
    void onPlacesViewActivated(const QUrl &url);
    void onPlacesContextMenu(const QPoint &pos);
    void onRecentItemDoubleClicked(QListWidgetItem *item);
    void onRecentContextMenu(const QPoint &pos);

private:
    // Unmount `index`, after moving every panel off it.
    void teardown(const QModelIndex &index);
    void refreshRecentList();
    // Incremental update for the common single-URL case (see recordVisit).
    void promoteRecentItem(const QUrl &url);
    // Rebuilds every sidebar's list after a shared-list change other than a visit.
    static void refreshAllRecentLists();
    QListWidgetItem *makeRecentItem(const QUrl &url) const;

    KFilePlacesView  *m_placesView   = nullptr;
    QWidget          *m_recentWidget = nullptr;
    QFrame           *m_recentSep    = nullptr;
    QLabel           *m_recentLabel  = nullptr;
    QListWidget      *m_recentList   = nullptr;
    QSplitter        *m_splitter     = nullptr;
    KFilePlacesModel *m_placesModel  = nullptr;

    // Shared by all windows. Each window used to keep its own list while only
    // the first window's was saved, so visits made in other windows were lost.
    static QList<QUrl>      s_recentUrls;
    static QList<Sidebar *> s_instances;
    static bool             s_recentLoaded; // read from config once per process
    bool m_trackRecent = true;

    static constexpr int MaxRecent = 20;
};
