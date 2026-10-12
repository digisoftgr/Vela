#include "Sidebar.h"

#include "MessageBoxUtil.h"
#include "MountWatcher.h"

#include <KFilePlacesModel>
#include <KFilePlacesView>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KConfigGroup>

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSplitter>
#include <QVBoxLayout>

// Subclass (not proxy) so KFilePlacesView's internal qobject_cast succeeds.
// Overrides DecorationRole: fetches the 32px colored art then scales it down.
class PlacesModel : public KFilePlacesModel
{
public:
    static constexpr int kSourceSize  = 32;
    static constexpr int kDisplaySize = 22;

    explicit PlacesModel(QObject *parent = nullptr)
        : KFilePlacesModel(parent) {}

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (role == Qt::DecorationRole) {
            const QVariant v = KFilePlacesModel::data(index, role);
            if (v.canConvert<QIcon>()) {
                const QIcon icon = v.value<QIcon>();
                // Rendering and smooth-scaling ran on every paint of every row.
                // Cache by the icon's identity (theme name, or cacheKey for an
                // unnamed one, e.g. a device icon); a changed icon gets a new key.
                const QString key = icon.name().isEmpty()
                                        ? QString::number(icon.cacheKey())
                                        : icon.name();
                auto it = m_scaled.constFind(key);
                if (it != m_scaled.constEnd())
                    return QVariant::fromValue(it.value());
                const QPixmap src = icon.pixmap(kSourceSize, kSourceSize);
                if (!src.isNull()) {
                    QIcon out;
                    out.addPixmap(src.scaled(kDisplaySize, kDisplaySize,
                                             Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
                    m_scaled.insert(key, out);
                    return QVariant::fromValue(out);
                }
            }
        }
        return KFilePlacesModel::data(index, role);
    }

    // A theme change renders different art under the same names.
    void clearIconCache() { m_scaled.clear(); }

private:
    mutable QHash<QString, QIcon> m_scaled;
};

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(150);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // ── Places / Bookmarks / Devices ─────────────────────────────────────────
    m_placesModel = new PlacesModel(this);

    m_placesView = new KFilePlacesView(this);
    m_placesView->setModel(m_placesModel);
    m_placesView->setFrameStyle(QFrame::NoFrame);
    m_placesView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_placesView->setAcceptDrops(true);
    m_placesView->setDragDropMode(QAbstractItemView::DropOnly);
    m_placesView->setAutoResizeItemsEnabled(false);
    m_placesView->setIconSize(QSize(PlacesModel::kDisplaySize, PlacesModel::kDisplaySize));

    connect(m_placesView, &KFilePlacesView::placeActivated,
            this, &Sidebar::onPlacesViewActivated);
    // The view's own eject/unmount button goes through the same path as the
    // context menu, so panels are moved off the device first either way.
    m_placesView->setTeardownFunction([this](const QModelIndex &index) { teardown(index); });
    // Mount and unmount failures ("device is busy", wrong password, …) arrive
    // only through this signal. KFilePlacesView does not display it, so nothing
    // was shown at all.
    connect(m_placesModel, &KFilePlacesModel::errorMessage, this, [this](const QString &message) {
        QMessageBox box(QMessageBox::Warning, i18n("Device Error"), message,
                        QMessageBox::Ok, window());
        prepareMessageBox(box);
        box.exec();
    });
    connect(m_placesView, &QWidget::customContextMenuRequested,
            this, &Sidebar::onPlacesContextMenu);
    m_placesView->viewport()->installEventFilter(this);

    // ── Recent section ───────────────────────────────────────────────────────
    m_recentWidget = new QWidget(this);
    auto *recentLayout = new QVBoxLayout(m_recentWidget);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->setSpacing(0);

    m_recentSep = new QFrame(m_recentWidget);
    m_recentSep->setFrameShape(QFrame::HLine);
    m_recentSep->setFrameShadow(QFrame::Sunken);

    m_recentLabel = new QLabel(i18n("Recent"), m_recentWidget);
    m_recentLabel->setContentsMargins(8, 6, 8, 2);
    {
        QFont f = m_recentLabel->font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 0.85);
        m_recentLabel->setFont(f);
    }

    m_recentList = new QListWidget(m_recentWidget);
    m_recentList->setFrameStyle(QFrame::NoFrame);
    m_recentList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recentList->setIconSize(QSize(16, 16));

    connect(m_recentList, &QListWidget::itemDoubleClicked,
            this, &Sidebar::onRecentItemDoubleClicked);
    connect(m_recentList, &QWidget::customContextMenuRequested,
            this, &Sidebar::onRecentContextMenu);
    m_recentList->viewport()->installEventFilter(this);

    recentLayout->addWidget(m_recentSep);
    recentLayout->addWidget(m_recentLabel);
    recentLayout->addWidget(m_recentList, 1);

    // ── Vertical splitter: places on top, recent on bottom ───────────────────
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_placesView);
    m_splitter->addWidget(m_recentWidget);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);

    outerLayout->addWidget(m_splitter, 1);

    // A window opened later starts from the list the others already share.
    s_instances.append(this);
    refreshRecentList();
}

Sidebar::~Sidebar()
{
    s_instances.removeOne(this);
}

QList<QUrl>      Sidebar::s_recentUrls;
QList<Sidebar *> Sidebar::s_instances;
bool             Sidebar::s_recentLoaded = false;

// ── Public ────────────────────────────────────────────────────────────────────

void Sidebar::setTrackRecent(bool enabled)
{
    m_trackRecent = enabled;
    m_recentWidget->setVisible(enabled);
    if (!enabled && !s_recentUrls.isEmpty()) {
        s_recentUrls.clear();
        refreshAllRecentLists();
    }
}

void Sidebar::recordVisit(const QUrl &url)
{
    if (!m_trackRecent || !url.isValid())
        return;
    if (!s_recentUrls.isEmpty() && s_recentUrls.constFirst() == url)
        return;
    s_recentUrls.removeAll(url);
    s_recentUrls.prepend(url);
    while (s_recentUrls.size() > MaxRecent)
        s_recentUrls.removeLast();
    for (Sidebar *s : std::as_const(s_instances))
        s->promoteRecentItem(url);
}

void Sidebar::refreshAllRecentLists()
{
    for (Sidebar *s : std::as_const(s_instances))
        s->refreshRecentList();
}

void Sidebar::setCurrentUrl(const QUrl &url)
{
    m_placesView->setUrl(url);
}

void Sidebar::saveSettings() const
{
    KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("Sidebar"));

    QStringList entries;
    entries.reserve(s_recentUrls.size());
    for (const QUrl &u : s_recentUrls)
        entries << u.toString();
    group.writeEntry("RecentFolders", entries);

    const QList<int> sizes = m_splitter->sizes();
    group.writeEntry("SplitterSizes", QList<int>{sizes[0], sizes[1]});
}

void Sidebar::restoreSettings()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(QStringLiteral("Sidebar"));

    // Read the saved list once per process. Later windows (restored, torn off,
    // or recreated for a theme change) already see the live shared list, which
    // is newer than what was saved.
    if (!s_recentLoaded) {
        s_recentLoaded = true;
        const QStringList entries = group.readEntry("RecentFolders", QStringList());
        for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
            const QUrl url(*it);
            if (url.isValid())
                recordVisit(url);
        }
    }

    const QList<int> sizes = group.readEntry("SplitterSizes", QList<int>{});
    if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 0)
        m_splitter->setSizes(sizes);
}

// ── Event filter ──────────────────────────────────────────────────────────────

bool Sidebar::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_placesView->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        // Ctrl+left-click or middle-click → open in new tab.
        const bool ctrlLeft = (me->button() == Qt::LeftButton)
                              && (me->modifiers() & Qt::ControlModifier);
        const bool middleClick = (me->button() == Qt::MiddleButton);
        if (ctrlLeft || middleClick) {
            const QModelIndex idx = m_placesView->indexAt(me->pos());
            if (idx.isValid()) {
                const QUrl url = m_placesModel->url(idx);
                if (url.isValid()) {
                    Q_EMIT placeActivated(url, /*openInNewTab=*/true);
                    return true;
                }
            }
        }
    }

    if (obj == m_recentList->viewport() && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            QListWidgetItem *item = m_recentList->itemAt(me->pos());
            if (item) {
                const QUrl url = item->data(Qt::UserRole).toUrl();
                if (url.isValid()) {
                    Q_EMIT placeActivated(url, /*openInNewTab=*/true);
                    return true;
                }
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

void Sidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ThemeChange:
        // Scaled place icons are cached; drop them so the new theme's art shows.
        static_cast<PlacesModel *>(m_placesModel)->clearIconCache();
        m_placesView->viewport()->update();
        break;
    default:
        break;
    }
}

// ── Private slots ─────────────────────────────────────────────────────────────

void Sidebar::onPlacesViewActivated(const QUrl &url)
{
    Q_EMIT placeActivated(url, false);
}

void Sidebar::onPlacesContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_placesView->indexAt(pos);
    if (!idx.isValid())
        return;

    const QUrl url = m_placesModel->url(idx);
    const bool isDevice = m_placesModel->isDevice(idx);

    QMenu menu(this);
    QAction *openAct    = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open"));
    QAction *openTabAct = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-new")),       i18n("Open in New Tab"));
    menu.addSeparator();

    QAction *renameAct = nullptr;
    QAction *removeAct = nullptr;
    if (!isDevice) {
        renameAct = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18n("Rename…"));
        removeAct = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Remove"));
        menu.addSeparator();
    }

    QAction *mountAct   = nullptr;
    QAction *unmountAct = nullptr;
    if (isDevice) {
        if (!m_placesModel->setupNeeded(idx))
            unmountAct = menu.addAction(QIcon::fromTheme(QStringLiteral("media-eject")), i18n("Safely Remove / Unmount"));
        else
            mountAct = menu.addAction(QIcon::fromTheme(QStringLiteral("media-mount")), i18n("Mount"));
    }

    QAction *chosen = menu.exec(m_placesView->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == openAct)            Q_EMIT placeActivated(url, false);
    else if (chosen == openTabAct)    Q_EMIT placeActivated(url, true);
    else if (renameAct && chosen == renameAct) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, i18n("Rename Bookmark"), i18n("New name:"),
                                                   QLineEdit::Normal, m_placesModel->text(idx), &ok);
        if (ok && !name.isEmpty())
            m_placesModel->editPlace(idx, name, url);
    } else if (removeAct && chosen == removeAct) {
        m_placesModel->removePlace(idx);
    } else if (mountAct && chosen == mountAct) {
        m_placesModel->requestSetup(idx);
    } else if (unmountAct && chosen == unmountAct) {
        teardown(idx);
    }
}

void Sidebar::teardown(const QModelIndex &index)
{
    // Panels showing the device let go of it before the request goes out:
    // their listers, watches and previews would otherwise keep it busy, or be
    // left showing a stale listing of the (now empty) mountpoint.
    const QUrl url = m_placesModel->url(index);
    if (url.isLocalFile())
        MountWatcher::self()->prepareTeardown(url.toLocalFile());
    m_placesModel->requestTeardown(index);
}

void Sidebar::onRecentItemDoubleClicked(QListWidgetItem *item)
{
    const QUrl url = item->data(Qt::UserRole).toUrl();
    if (url.isValid())
        Q_EMIT placeActivated(url, false);
}

void Sidebar::onRecentContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_recentList->itemAt(pos);
    QMenu menu(this);

    QAction *openAct    = nullptr;
    QAction *openTabAct = nullptr;
    QAction *removeAct  = nullptr;

    if (item) {
        openAct    = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18n("Open"));
        openTabAct = menu.addAction(QIcon::fromTheme(QStringLiteral("tab-new")),       i18n("Open in New Tab"));
        menu.addSeparator();
        removeAct  = menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")),   i18n("Remove from Recent"));
        menu.addSeparator();
    }
    QAction *clearAct = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), i18n("Clear All Recent"));

    QAction *chosen = menu.exec(m_recentList->mapToGlobal(pos));
    if (!chosen) return;

    if (openAct && chosen == openAct)          Q_EMIT placeActivated(item->data(Qt::UserRole).toUrl(), false);
    else if (openTabAct && chosen == openTabAct) Q_EMIT placeActivated(item->data(Qt::UserRole).toUrl(), true);
    else if (removeAct && chosen == removeAct) {
        s_recentUrls.removeAll(item->data(Qt::UserRole).toUrl());
        refreshAllRecentLists();
    } else if (chosen == clearAct) {
        s_recentUrls.clear();
        refreshAllRecentLists();
    }
}

// Called on every navigation, so it must not rebuild the whole list. The common
// case is "one URL moved to the front"; handle that by moving the existing row
// rather than destroying and recreating 20 widgets and 20 theme-icon lookups.
void Sidebar::promoteRecentItem(const QUrl &url)
{
    // Already-present entry: lift it to the top.
    for (int row = 0; row < m_recentList->count(); ++row) {
        if (m_recentList->item(row)->data(Qt::UserRole).toUrl() == url) {
            if (row != 0)
                m_recentList->insertItem(0, m_recentList->takeItem(row));
            return;
        }
    }

    m_recentList->insertItem(0, makeRecentItem(url));
    while (m_recentList->count() > MaxRecent)
        delete m_recentList->takeItem(m_recentList->count() - 1);
}

QListWidgetItem *Sidebar::makeRecentItem(const QUrl &url) const
{
    // One shared QIcon rather than a theme lookup per row.
    static const QIcon folderIcon = QIcon::fromTheme(QStringLiteral("folder"));
    const QString display = url.fileName().isEmpty() ? url.toString() : url.fileName();
    auto *item = new QListWidgetItem(folderIcon, display);
    item->setData(Qt::UserRole, url);
    item->setToolTip(url.toDisplayString(QUrl::PreferLocalFile));
    return item;
}

void Sidebar::refreshRecentList()
{
    m_recentList->clear();
    for (const QUrl &url : std::as_const(s_recentUrls))
        m_recentList->addItem(makeRecentItem(url));
}
