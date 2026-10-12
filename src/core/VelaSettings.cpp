#include "VelaSettings.h"

#include <QDateTime>
#include <algorithm>

#include <KSharedConfig>
#include <KConfigGroup>
#include <QStandardPaths>
#include <QUrl>

VelaSettings::VelaSettings()
    : QObject(nullptr)
{
    load();
}


// Two parallel string lists per kind — the ordered ids, and the subset that
// goes in the submenu — rather than one list with an encoded flag. Both are
// plain QStringList entries, so the config stays hand-editable.
static QList<VelaSettings::OpenInApp> readOpenIn(const KConfigGroup &g, const QString &kind)
{
    const QString appsKey = kind + QStringLiteral("OpenInApps");
    if (!g.hasKey(appsKey))
        return VelaSettings::defaultOpenIn();
    const QStringList ids = g.readEntry(appsKey, QStringList());
    const QStringList sub = g.readEntry(kind + QStringLiteral("OpenInSubmenu"), QStringList());
    QList<VelaSettings::OpenInApp> apps;
    apps.reserve(ids.size());
    for (const QString &id : ids)
        apps.append({id, sub.contains(id)});
    return apps;
}

static void writeOpenIn(KConfigGroup &g, const QString &kind,
                        const QList<VelaSettings::OpenInApp> &apps)
{
    QStringList ids, sub;
    for (const auto &app : apps) {
        ids << app.desktopId;
        if (app.inSubmenu)
            sub << app.desktopId;
    }
    g.writeEntry(kind + QStringLiteral("OpenInApps"), ids);
    g.writeEntry(kind + QStringLiteral("OpenInSubmenu"), sub);
}

QList<VelaSettings::OpenInApp> VelaSettings::defaultOpenIn()
{
    return {{QStringLiteral("code.desktop"), false}};
}

VelaSettings *VelaSettings::self()
{
    static VelaSettings instance;
    return &instance;
}

void VelaSettings::load()
{
    auto cfg = KSharedConfig::openConfig(QStringLiteral("velarc"));

    {
        KConfigGroup g = cfg->group(QStringLiteral("General"));
        m_defaultViewMode           = g.readEntry("DefaultViewMode",           0);
        m_singleClick               = g.readEntry("SingleClick",               false);
        m_showHiddenDefault         = g.readEntry("ShowHiddenDefault",         false);
        m_trackRecent               = g.readEntry("TrackRecent",               true);
        m_confirmDelete             = g.readEntry("ConfirmDelete",             true);
        m_dateFormat                = g.readEntry("DateFormat",                0);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Theme"));
        m_themeMode  = g.readEntry("ThemeMode",  0);
        m_fontFamily = g.readEntry("FontFamily", QString());
        m_fontSize   = g.readEntry("FontSize",   0);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Appearance"));
        m_alternatingRowColors = g.readEntry("AlternatingRowColors", true);
        m_toolbarIconSize      = g.readEntry("ToolbarIconSize",       1);
        m_showToolbarText      = g.readEntry("ShowToolbarText",       true);
        m_wireframeEnabled     = g.readEntry("WireframeEnabled",      true);
        m_wireframeThickness   = g.readEntry("WireframeThickness",    1);
        m_wireframeColor       = g.readEntry("WireframeColor",        QStringLiteral("#399CD0"));
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Tabs"));
        m_restoreTabsOnStartup = g.readEntry("RestoreTabsOnStartup", true);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Advanced"));
        m_terminalCommand     = g.readEntry("TerminalCommand", QStringLiteral("konsole"));
        m_folderOpenIn        = readOpenIn(g, QStringLiteral("Folder"));
        m_fileOpenIn          = readOpenIn(g, QStringLiteral("File"));
        m_jobDismissDelay     = g.readEntry("JobDismissDelay", 0);
        m_suppressPlasmaJobNotifications = g.readEntry("SuppressPlasmaJobNotifications", false);
        m_thumbnailMaxSizeMiB = g.readEntry("ThumbnailMaxSizeMiB", 10);
    }
}

void VelaSettings::save()
{
    auto cfg = KSharedConfig::openConfig(QStringLiteral("velarc"));

    {
        KConfigGroup g = cfg->group(QStringLiteral("General"));
        g.writeEntry("DefaultViewMode",   m_defaultViewMode);
        g.writeEntry("SingleClick",       m_singleClick);
        g.writeEntry("ShowHiddenDefault", m_showHiddenDefault);
        g.writeEntry("TrackRecent",       m_trackRecent);
        g.writeEntry("ConfirmDelete",     m_confirmDelete);
        // Retired: overwriting always asks now. Drop the stale key.
        g.deleteEntry("ConfirmOverwrite");
        g.writeEntry("DateFormat",        m_dateFormat);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Theme"));
        g.writeEntry("ThemeMode",  m_themeMode);
        g.writeEntry("FontFamily", m_fontFamily);
        g.writeEntry("FontSize",   m_fontSize);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Appearance"));
        g.writeEntry("AlternatingRowColors", m_alternatingRowColors);
        g.writeEntry("ToolbarIconSize",      m_toolbarIconSize);
        g.writeEntry("ShowToolbarText",      m_showToolbarText);
        g.writeEntry("WireframeEnabled",     m_wireframeEnabled);
        g.writeEntry("WireframeThickness",   m_wireframeThickness);
        g.writeEntry("WireframeColor",       m_wireframeColor);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Tabs"));
        g.writeEntry("RestoreTabsOnStartup", m_restoreTabsOnStartup);
    }

    {
        KConfigGroup g = cfg->group(QStringLiteral("Advanced"));
        g.writeEntry("TerminalCommand",     m_terminalCommand);
        writeOpenIn(g, QStringLiteral("Folder"), m_folderOpenIn);
        writeOpenIn(g, QStringLiteral("File"),   m_fileOpenIn);
        g.writeEntry("JobDismissDelay",     m_jobDismissDelay);
        g.writeEntry("SuppressPlasmaJobNotifications", m_suppressPlasmaJobNotifications);
        g.writeEntry("ThumbnailMaxSizeMiB", m_thumbnailMaxSizeMiB);
    }

    cfg->sync();
}

bool VelaSettings::previewWordWrap()
{
    return KSharedConfig::openConfig(QStringLiteral("velarc"))
        ->group(QStringLiteral("Preview")).readEntry("WordWrap", false);
}

void VelaSettings::setPreviewWordWrap(bool on)
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("velarc"))
                         ->group(QStringLiteral("Preview"));
    g.writeEntry("WordWrap", on);
    // No sync(), same reasoning as setDirSortPref.
}

QPair<int, Qt::SortOrder> VelaSettings::dirSortPref(const QString &urlString)
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("velarc"))
                         ->group(QStringLiteral("DirectorySortPrefs"));
    const QString val = g.readEntry(urlString, QString());
    if (val.isEmpty()) {
        const QString downloads = QUrl::fromLocalFile(
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
        if (urlString == downloads)
            return {3, Qt::DescendingOrder}; // FileSystemModel::DateModified, Descending
        return {0, Qt::AscendingOrder};
    }
    // "column:order[:setAtSecs]" — the timestamp is optional so entries
    // written before it existed still read.
    const QStringList parts = val.split(u':');
    if (parts.size() != 2 && parts.size() != 3)
        return {0, Qt::AscendingOrder};
    return {parts[0].toInt(), static_cast<Qt::SortOrder>(parts[1].toInt())};
}

void VelaSettings::setDirSortPref(const QString &urlString, int column, Qt::SortOrder order)
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("velarc"))
                         ->group(QStringLiteral("DirectorySortPrefs"));

    const QString sortPart = QStringLiteral("%1:%2").arg(column).arg(static_cast<int>(order));
    const QString current = g.readEntry(urlString, QString());
    if (current == sortPart || current.startsWith(sortPart + QLatin1Char(':')))
        return; // unchanged — don't dirty the config at all

    // The group grows by one entry per directory ever sorted and is never read
    // in bulk, so cap it rather than letting velarc accumulate forever. The
    // least recently set entries are dropped; the default (Name, Ascending)
    // simply applies again. Age comes from the stored timestamp: keyList() is
    // alphabetical, so pruning by position dropped arbitrary directories.
    if (!g.hasKey(urlString) && g.keyList().size() >= kMaxDirSortPrefs) {
        QList<QPair<qint64, QString>> byAge;
        const QStringList keys = g.keyList();
        byAge.reserve(keys.size());
        for (const QString &key : keys) {
            const QStringList parts = g.readEntry(key, QString()).split(u':');
            // Legacy entries have no timestamp: treat them as oldest.
            byAge.append({parts.size() == 3 ? parts[2].toLongLong() : 0, key});
        }
        std::sort(byAge.begin(), byAge.end());
        for (int i = 0; i < byAge.size() - kMaxDirSortPrefs / 2; ++i)
            g.deleteEntry(byAge.at(i).second);
    }

    g.writeEntry(urlString, QString(sortPart + QLatin1Char(':')
                                    + QString::number(QDateTime::currentSecsSinceEpoch())));
    // No sync() here: a sort-order click should not force a full velarc rewrite.
    // KSharedConfig flushes on destruction, and the session autosave syncs too.
}
