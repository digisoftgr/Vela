#pragma once

#include <QObject>
#include <QString>
#include <Qt>

class VelaSettings : public QObject
{
    Q_OBJECT
public:
    static VelaSettings *self();
    void load();
    void save();

    // ── General ───────────────────────────────────────────────────────────────
    int  defaultViewMode()  const { return m_defaultViewMode; }
    bool singleClick()      const { return m_singleClick; }
    bool showHiddenDefault()const { return m_showHiddenDefault; }
    bool trackRecent()      const { return m_trackRecent; }
    bool confirmDelete()    const { return m_confirmDelete; }
    // 0 = DD/MM/YYYY, 1 = MM/DD/YYYY
    int  dateFormat()       const { return m_dateFormat; }

    void setDefaultViewMode(int v)    { m_defaultViewMode = v; }
    void setSingleClick(bool v)       { m_singleClick = v; }
    void setShowHiddenDefault(bool v) { m_showHiddenDefault = v; }
    void setTrackRecent(bool v)       { m_trackRecent = v; }
    void setConfirmDelete(bool v)     { m_confirmDelete = v; }
    void setDateFormat(int v)         { m_dateFormat = v; }

    // ── Theme & Font ──────────────────────────────────────────────────────────
    // themeMode: 0=System, 1=Light, 2=Dark
    int     themeMode()    const { return m_themeMode; }
    QString fontFamily()   const { return m_fontFamily; }
    int     fontSize()     const { return m_fontSize; }

    void setThemeMode(int v)              { m_themeMode  = v; }
    void setFontFamily(const QString &v)  { m_fontFamily = v; }
    void setFontSize(int v)               { m_fontSize   = v; }

    // ── Appearance ────────────────────────────────────────────────────────────
    bool alternatingRowColors() const { return m_alternatingRowColors; }
    int  toolbarIconSize()      const { return m_toolbarIconSize; } // 0=16 1=22 2=32
    bool showToolbarText()      const { return m_showToolbarText; }
    bool    wireframeEnabled()      const { return m_wireframeEnabled; }
    int     wireframeThickness()    const { return m_wireframeThickness; }
    QString wireframeColor()        const { return m_wireframeColor; }

    void setAlternatingRowColors(bool v) { m_alternatingRowColors = v; }
    void setToolbarIconSize(int v)       { m_toolbarIconSize = v; }
    void setShowToolbarText(bool v)      { m_showToolbarText = v; }
    void setWireframeEnabled(bool v)          { m_wireframeEnabled = v; }
    void setWireframeThickness(int v)         { m_wireframeThickness = v; }
    void setWireframeColor(const QString &v)  { m_wireframeColor = v; }

    // ── Tabs ──────────────────────────────────────────────────────────────────
    bool restoreTabsOnStartup() const { return m_restoreTabsOnStartup; }

    void setRestoreTabsOnStartup(bool v) { m_restoreTabsOnStartup = v; }

    // ── Advanced ──────────────────────────────────────────────────────────────
    QString terminalCommand()const { return m_terminalCommand; }
    int     jobDismissDelay() const { return m_jobDismissDelay; }
    // Keep file operations out of Plasma's notification area; the Operations
    // window is then the only progress UI. Applies to jobs started afterwards.
    bool    suppressPlasmaJobNotifications() const { return m_suppressPlasmaJobNotifications; }
    // Largest file, in MiB, for which an icon-view thumbnail is generated.
    int     thumbnailMaxSizeMiB() const { return m_thumbnailMaxSizeMiB; }

    void setTerminalCommand(const QString &v) { m_terminalCommand = v; }

    // ── "Open in …" ───────────────────────────────────────────────────────────
    // Applications pinned into the context menus, independent of the system
    // "Open With" list — which only shows apps whose desktop file claims the
    // MIME type, an association that is easy to lose. Each is a desktop-entry
    // storage id ("code.desktop"), resolved through KService at menu-build time
    // so the name, icon and Exec line always come from the installed app.
    struct OpenInApp {
        QString desktopId;
        bool inSubmenu = false; // collected under "Open In ▸" instead of flat
    };
    QList<OpenInApp> folderOpenIn() const { return m_folderOpenIn; }
    QList<OpenInApp> fileOpenIn()   const { return m_fileOpenIn; }
    void setFolderOpenIn(const QList<OpenInApp> &v) { m_folderOpenIn = v; }
    void setFileOpenIn(const QList<OpenInApp> &v)   { m_fileOpenIn = v; }
    static QList<OpenInApp> defaultOpenIn();
    void setJobDismissDelay(int v)            { m_jobDismissDelay = v; }
    void setSuppressPlasmaJobNotifications(bool v) { m_suppressPlasmaJobNotifications = v; }
    void setThumbnailMaxSizeMiB(int v)        { m_thumbnailMaxSizeMiB = v; }

    // ── Preview ───────────────────────────────────────────────────────────────
    // Word wrap in the preview pane's text editor. Toggled from the pane
    // itself, so it is written straight through like dirSortPref rather than
    // going through load()/save().
    static bool previewWordWrap();
    static void setPreviewWordWrap(bool on);

    // ── Per-directory sort preferences ────────────────────────────────────────
    // key: directory URL string, value: (column, order). Default: (Name, Ascending).
    static QPair<int, Qt::SortOrder> dirSortPref(const QString &urlString);
    static void setDirSortPref(const QString &urlString, int column, Qt::SortOrder order);

private:
    VelaSettings();

    // Upper bound on remembered per-directory sort preferences (see setDirSortPref).
    static constexpr int kMaxDirSortPrefs = 500;

    int     m_themeMode                 = 0;  // 0=System 1=Light 2=Dark
    QString m_fontFamily                = {};
    int     m_fontSize                  = 0;  // 0 = follow system
    int     m_defaultViewMode           = 0;
    bool    m_singleClick               = false;
    bool    m_showHiddenDefault         = false;
    bool    m_trackRecent               = true;
    bool    m_confirmDelete             = true;
    int     m_dateFormat                = 0;
    bool    m_alternatingRowColors      = true;
    int     m_toolbarIconSize           = 1;
    bool    m_showToolbarText           = true;
    bool    m_wireframeEnabled          = true;
    int     m_wireframeThickness        = 1;
    QString m_wireframeColor            = QStringLiteral("#399CD0");
    bool    m_restoreTabsOnStartup      = true;
    QString m_terminalCommand           = QStringLiteral("konsole");
    QList<OpenInApp> m_folderOpenIn     = defaultOpenIn();
    QList<OpenInApp> m_fileOpenIn       = defaultOpenIn();
    int     m_jobDismissDelay           = 0;
    bool    m_suppressPlasmaJobNotifications = false;
    int     m_thumbnailMaxSizeMiB       = 10;
};
