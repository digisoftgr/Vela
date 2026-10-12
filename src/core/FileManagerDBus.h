#pragma once

#include <QObject>
#include <QStringList>

// Implements org.freedesktop.FileManager1 — the standard D-Bus protocol used by
// Chrome, VS Code, and other apps to ask the running file manager to reveal files.
//
// App-lifetime: parented to the application (not a window) and routes to a freshly
// created window, so it keeps working after any individual window closes.
class FileManagerDBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")

public:
    explicit FileManagerDBus(QObject *parent = nullptr);

public Q_SLOTS:
    Q_SCRIPTABLE void ShowFolders(const QStringList &uris, const QString &startupId);
    Q_SCRIPTABLE void ShowItems(const QStringList &uris, const QString &startupId);
    Q_SCRIPTABLE void ShowItemProperties(const QStringList &uris, const QString &startupId);
};

// Implements org.kde.vela.MainWindow — the single-instance control object a second
// `vela` launch calls to hand off to the already-running process.
//
// Also app-lifetime, for the same reason: it must survive the first window closing
// (previously it was the first MainWindow itself, so closing that window silently
// broke both single-instance handoff and reveal-from-other-apps).
class VelaInstanceDBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.vela.MainWindow")

public:
    explicit VelaInstanceDBus(QObject *parent = nullptr);

public Q_SLOTS:
    Q_SCRIPTABLE void raise();
    Q_SCRIPTABLE void openPaths(const QStringList &paths);
};
