#include "ui/MainWindow.h"
#include "ui/OperationsWindow.h"
#include "core/FileManagerDBus.h"
#include "core/JobTracker.h"
#include "core/VelaSettings.h"
#include "core/WorkerPools.h"
#include "config.h"

#include <KAboutData>
#include <KColorSchemeManager>
#include <KColorSchemeModel>
#include <KLocalizedString>
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QSet>
#include <QTimer>

// DBus service name — must match vela.desktop X-DBUS-ServiceName
static const char *kServiceName = "org.kde.vela";
static const char *kObjectPath  = "/MainWindow";
static const char *kInterface   = "org.kde.vela.MainWindow";

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    WorkerPools::installShutdownGuard();

    KLocalizedString::setApplicationDomain("vela");

    KAboutData aboutData(QStringLiteral("vela"),
                         i18n("Vela"),
                         QStringLiteral(VELA_VERSION),
                         i18n("A file manager for KDE Plasma"),
                         KAboutLicense::GPL_V3);
    aboutData.setOrganizationDomain("vela.kde");
    aboutData.setDesktopFileName(QStringLiteral("vela"));
    KAboutData::setApplicationData(aboutData);
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/vela.png")));

    // Parse positional arguments (folders to open, or files to reveal). The
    // desktop entry launches `vela %u`, so this is how file-manager association
    // and `vela /some/path` actually reach a window.
    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);
    parser.addPositionalArgument(QStringLiteral("paths"),
                                 i18n("Folders to open, or files to reveal"),
                                 QStringLiteral("[paths...]"));
    // Used by the org.freedesktop.FileManager1 D-Bus service file: the bus
    // starts us to service a reveal request, so we must own the name and wait
    // for the call rather than opening a window at the user's home directory.
    const QCommandLineOption daemonOption(
        QStringLiteral("daemon"),
        i18n("Start without opening a window (D-Bus activation)."));
    parser.addOption(daemonOption);
    parser.process(app);
    aboutData.processCommandLine(&parser);

    const bool daemonMode = parser.isSet(daemonOption);

    // Unexpanded desktop-entry field codes. `Exec=vela %u` launched with no file
    // makes some launchers pass the literal "%u"; it must not be treated as a path
    // (that produced "/home/qwerty/%u" and clobbered session restore).
    static const QSet<QString> kFieldCodes = {
        QStringLiteral("%f"), QStringLiteral("%F"), QStringLiteral("%u"), QStringLiteral("%U"),
        QStringLiteral("%d"), QStringLiteral("%D"), QStringLiteral("%n"), QStringLiteral("%N"),
        QStringLiteral("%i"), QStringLiteral("%c"), QStringLiteral("%k"), QStringLiteral("%v"),
        QStringLiteral("%m"),
    };

    // Resolve arguments to absolute form now, while we still hold this process's
    // working directory — a second instance forwards them to the running one.
    QStringList targets;
    for (const QString &a : parser.positionalArguments()) {
        if (a.isEmpty() || kFieldCodes.contains(a))
            continue;
        if (a.contains(QLatin1String("://")))
            targets << a;
        else
            targets << QDir::current().absoluteFilePath(a);
    }

    // Single-instance: if another Vela is already running, hand off and exit.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        // Object first, name second — as for FileManager1 below. Claiming the
        // name before the object existed left a gap (while the first window was
        // being built) in which a second launch called a missing object, failed
        // silently and exited 0. Calls are only dispatched once the event loop runs, by
        // which point the first window exists.
        auto *instanceDBus = new VelaInstanceDBus(&app);
        bus.registerObject(QLatin1String(kObjectPath), instanceDBus,
                           QDBusConnection::ExportScriptableSlots);

        if (!bus.registerService(QLatin1String(kServiceName))) {
            // Service already registered — another instance is running.
            QDBusInterface iface(QLatin1String(kServiceName),
                                 QLatin1String(kObjectPath),
                                 QLatin1String(kInterface),
                                 bus);
            const QDBusMessage reply = targets.isEmpty()
                ? iface.call(QStringLiteral("raise"))
                : iface.call(QStringLiteral("openPaths"), targets);
            if (reply.type() == QDBusMessage::ErrorMessage) {
                qWarning() << "Vela: handing off to the running instance failed:"
                           << reply.errorMessage();
                return 1;
            }
            return 0;
        }
    }

    // Apply the saved color scheme before the first window is created so the
    // window manager sees the correct decoration color on initial mapping.
    {
        const int mode = VelaSettings::self()->themeMode();
        if (mode != 0) {
            KColorSchemeManager *mgr = KColorSchemeManager::instance();
            const QString keyword = (mode == 1) ? QStringLiteral("Light")
                                                : QStringLiteral("Dark");
            const QAbstractItemModel *model = mgr->model();
            for (int r = 0; r < model->rowCount(); ++r) {
                const QModelIndex idx = model->index(r, 0);
                if (idx.data(Qt::DisplayRole).toString().contains(keyword, Qt::CaseInsensitive)) {
                    mgr->activateSchemeId(idx.data(KColorSchemeModel::IdRole).toString());
                    break;
                }
            }
        }
    }

    // Before any window or job exists: every progress-reporting job from here on
    // goes through JobTracker, which feeds the Operations window and keeps the
    // process alive until running jobs finish, even with every window closed.
    JobTracker::instance()->install();
    OperationsWindow operationsWindow;

    // In daemon mode the bus started us purely to service a pending call; the
    // window is created by whichever D-Bus method arrives next.
    if (!daemonMode) {
        auto *window = new MainWindow();
        window->show();

        // Load any launch targets into this fresh window's active panel.
        if (!targets.isEmpty())
            window->openTargets(targets, /*reuseActive=*/true);
    }

    // D-Bus control objects are parented to the application, not to a window,
    // so they survive the first window closing (which previously silently broke
    // single-instance handoff and reveal-from-other-apps). The single-instance
    // object was registered above, before its name was claimed.
    if (bus.isConnected()) {
        // Register org.freedesktop.FileManager1 so Chrome, VS Code, and other
        // apps that use the standard D-Bus protocol find Vela directly.
        //
        // The object is registered *before* the name is claimed so there is no
        // window in which we own the name but can't answer calls on it.
        auto *fmDbus = new FileManagerDBus(&app);
        bus.registerObject(QStringLiteral("/org/freedesktop/FileManager1"), fmDbus,
                           QDBusConnection::ExportScriptableSlots);

        // ReplaceExistingService + AllowReplacement, not a plain registerService.
        // Dolphin ships an org.freedesktop.FileManager1 service file, so any
        // "open containing folder" issued while Vela was closed activates Dolphin,
        // which then holds the name for the rest of the session — after which a
        // plain registration here fails forever and reveal silently stops working.
        // Requesting replacement takes the name back on every Vela start.
        QDBusConnectionInterface *iface = bus.interface();
        bool claimed = false;
        if (iface) {
            const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
                iface->registerService(QStringLiteral("org.freedesktop.FileManager1"),
                                       QDBusConnectionInterface::ReplaceExistingService,
                                       QDBusConnectionInterface::AllowReplacement);
            claimed = reply.isValid()
                      && reply.value() == QDBusConnectionInterface::ServiceRegistered;
        }
        if (!claimed) {
            qWarning() << "Vela: could not take org.freedesktop.FileManager1;"
                       << "reveal-from-other-apps may open in another file manager.";
        }
    }

    // Daemon mode with nothing to do would otherwise sit invisible forever.
    // Give the activating call a moment to arrive, then exit if no window opened.
    if (daemonMode) {
        QTimer::singleShot(30'000, &app, [&app]() {
            // quit() ignores the tracker's event-loop locks; a job started from a
            // window that has since closed (its Operations row possibly hidden)
            // would be killed mid-write.
            if (JobTracker::instance()->runningJobCount() > 0)
                return;
            const auto windows = app.topLevelWidgets();
            for (QWidget *w : windows) {
                if (w->isVisible())
                    return;
            }
            app.quit();
        });
    }

    return app.exec();
}
