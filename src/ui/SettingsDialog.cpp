#include "SettingsDialog.h"
#include "OpenInListEditor.h"

#include "CommandPalette.h"
#include "core/VelaSettings.h"

#include <KApplicationTrader>
#include <KColorButton>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KService>
#include <KSharedConfig>
#include <KSycoca>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

// ── Default file manager helpers ──────────────────────────────────────────────

static const QString kDirMime   = QStringLiteral("inode/directory");
static const QString kDesktopId = QStringLiteral("vela.desktop");

// Asks KService rather than parsing mimeapps.list by hand. The old parser did
// an exact match on "vela.desktop" and so missed the "vela.desktop;" list form
// that KDE System Settings writes, and it ignored system-wide and desktop-
// specific mimeapps files that also decide the answer.
static bool isVelaDefault()
{
    KSycoca::self()->ensureCacheValid();
    const KService::Ptr svc = KApplicationTrader::preferredService(kDirMime);
    return svc && svc->storageId() == kDesktopId;
}

// KApplicationTrader has a setter but no "unset", so remove Vela from the
// user's mimeapps.list through KConfig's XDG list API — the same format
// setPreferredService writes — instead of rewriting the file line by line.
static void removeFromMimeapps()
{
    KSharedConfig::Ptr cfg = KSharedConfig::openConfig(QStringLiteral("mimeapps.list"),
                                                       KConfig::NoGlobals,
                                                       QStandardPaths::GenericConfigLocation);
    for (const QString &groupName : {QStringLiteral("Default Applications"),
                                     QStringLiteral("Added Associations")}) {
        KConfigGroup group(cfg, groupName);
        QStringList apps = group.readXdgListEntry(kDirMime);
        if (apps.removeAll(kDesktopId) == 0)
            continue;
        if (apps.isEmpty())
            group.deleteEntry(kDirMime);
        else
            group.writeXdgListEntry(kDirMime, apps);
    }
    cfg->sync();
}

static QString installedIconPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/icons/hicolor/256x256/apps/vela.png");
}

static void installDesktopFile()
{
    const QString appsDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QDir().mkpath(appsDir);

    // Copy the embedded icon to the user icon theme so Icon=vela resolves correctly.
    const QString iconPath = installedIconPath();
    QDir().mkpath(QFileInfo(iconPath).absolutePath());
    QFile src(QStringLiteral(":/icons/vela.png"));
    if (src.open(QIODevice::ReadOnly)) {
        QFile dst(iconPath);
        if (dst.open(QIODevice::WriteOnly | QIODevice::Truncate))
            dst.write(src.readAll());
    }

    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Vela\n"
        "GenericName=File Manager\n"
        "Comment=A fast, dual-pane file manager for KDE Plasma\n"
        "Exec=%1 %%u\n"
        "Icon=vela\n"
        "Categories=Qt;KDE;System;FileManager;\n"
        "MimeType=inode/directory;\n"
        "Keywords=file;manager;files;explorer;\n"
        "StartupNotify=true\n"
        "StartupWMClass=vela\n"
        "X-DBUS-ServiceName=org.kde.vela\n"
        "X-DBUS-StartupType=Unique\n"
        "X-Vela-Generated=true\n"
    ).arg(QCoreApplication::applicationFilePath());

    QFile f(appsDir + QLatin1Char('/') + kDesktopId);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        f.write(content.toUtf8());
}

static QString userDesktopFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)
           + QLatin1Char('/') + kDesktopId;
}

// Only a file Vela wrote itself: the marker, or (written by older versions,
// without it) the Comment line only that generator used.
static bool userDesktopFileIsOurs()
{
    QFile f(userDesktopFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray content = f.readAll();
    return content.contains("X-Vela-Generated=true")
        || content.contains("Comment=A fast, dual-pane file manager for KDE Plasma");
}

static void uninstallDesktopFile()
{
    if (!userDesktopFileIsOurs())
        return; // the user's own override, not ours to delete
    QFile::remove(userDesktopFilePath());
    QFile::remove(installedIconPath());
}

// A vela.desktop installed by the package (anywhere but the user's own
// applications directory).
static bool packagedDesktopFileExists()
{
    const QString user = userDesktopFilePath();
    const QStringList all = QStandardPaths::locateAll(QStandardPaths::ApplicationsLocation, kDesktopId);
    for (const QString &path : all) {
        if (QFileInfo(path).canonicalFilePath() != QFileInfo(user).canonicalFilePath())
            return true;
    }
    return false;
}

// Non-KDE consumers (GTK apps, xdg-open without KDE) read mimeinfo.cache.
// Nothing here waits on it, so it runs detached instead of blocking the GUI.
static void refreshDesktopDatabaseDetached()
{
    const QString appsDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QProcess::startDetached(QStringLiteral("update-desktop-database"), {appsDir});
}

// Returns false if Vela could not be made the default (its service did not
// register). Called only when the setting actually changed.
static bool setVelaAsDefault(bool enable)
{
    if (enable) {
        // The packaged entry when there is one. Writing a user copy always
        // shadowed it, pinned Exec= to whatever binary was running at the
        // time, and broke the association once that path moved or the
        // package was updated. A stale copy from earlier versions goes too.
        // Only a build with nothing installed (a dev tree) gets a user copy.
        if (packagedDesktopFileExists())
            uninstallDesktopFile();
        else
            installDesktopFile();
        // In-process sycoca update, so the just-written .desktop file resolves
        // to a KService. Synchronous, but it only runs when the checkbox was
        // actually toggled, not on every Apply.
        KSycoca::self()->ensureCacheValid();
        const KService::Ptr svc = KService::serviceByStorageId(kDesktopId);
        if (!svc)
            return false;
        KApplicationTrader::setPreferredService(kDirMime, svc);
    } else {
        removeFromMimeapps();
        uninstallDesktopFile();
    }
    KSycoca::self()->ensureCacheValid(); // pick up the mimeapps.list change
    refreshDesktopDatabaseDetached();
    return true;
}

SettingsDialog::SettingsDialog(CommandPalette *palette, QWidget *parent)
    : QDialog(parent)
    , m_commandPalette(palette)
{
    setWindowTitle(i18n("Vela Settings"));
    setMinimumSize(640, 460);
    resize(740, 560);

    auto *root = new QVBoxLayout(this);

    auto *row = new QHBoxLayout;
    root->addLayout(row, 1);

    // Left: page list
    m_pageList = new QListWidget(this);
    m_pageList->setFixedWidth(150);
    m_pageList->setSpacing(2);
    row->addWidget(m_pageList);

    // Right: page content
    m_pageStack = new QStackedWidget(this);
    row->addWidget(m_pageStack, 1);

    const struct {
        const char *label;
        QIcon icon;
        QWidget *(SettingsDialog::*build)();
    } pages[] = {
        { QT_TR_NOOP("Vela"),       QApplication::windowIcon(),                          &SettingsDialog::buildVelaPage       },
        { QT_TR_NOOP("General"),    QIcon::fromTheme(QStringLiteral("preferences-system")),        &SettingsDialog::buildGeneralPage    },
        { QT_TR_NOOP("Appearance"), QIcon::fromTheme(QStringLiteral("preferences-desktop-theme")), &SettingsDialog::buildAppearancePage },
        { QT_TR_NOOP("Tabs"),       QIcon::fromTheme(QStringLiteral("tab-new")),                   &SettingsDialog::buildTabsPage       },
        { QT_TR_NOOP("Advanced"),   QIcon::fromTheme(QStringLiteral("preferences-other")),         &SettingsDialog::buildAdvancedPage   },
        { QT_TR_NOOP("Open In"),    QIcon::fromTheme(QStringLiteral("document-open")),             &SettingsDialog::buildOpenInPage     },
        { QT_TR_NOOP("Commands"),   QIcon::fromTheme(QStringLiteral("system-run")),                &SettingsDialog::buildCommandsPage   },
    };
    for (const auto &p : pages) {
        m_pageList->addItem(new QListWidgetItem(p.icon, i18n(p.label)));
        m_pageStack->addWidget((this->*(p.build))());
    }
    m_pageList->setCurrentRow(0);
    connect(m_pageList, &QListWidget::currentRowChanged,
            m_pageStack, &QStackedWidget::setCurrentIndex);

    // Buttons
    auto *btns = new QDialogButtonBox(this);
    btns->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                             | QDialogButtonBox::Apply);
    auto *defaultsBtn = btns->addButton(i18n("Defaults"), QDialogButtonBox::ResetRole);

    connect(btns, &QDialogButtonBox::accepted, this, [this]() {
        onApply();
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(btns->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &SettingsDialog::onApply);
    connect(defaultsBtn, &QPushButton::clicked,
            this, &SettingsDialog::onRestoreDefaults);

    root->addWidget(btns);

    loadFromSettings();
}

// ── Load / Save ───────────────────────────────────────────────────────────────

void SettingsDialog::loadFromSettings()
{
    VelaSettings *s = VelaSettings::self();

    m_defaultViewMode->setCurrentIndex(s->defaultViewMode());
    m_singleClick->setChecked(s->singleClick());
    m_showHiddenDefault->setChecked(s->showHiddenDefault());
    m_trackRecent->setChecked(s->trackRecent());
    m_confirmDelete->setChecked(s->confirmDelete());
    m_dateFormat->setCurrentIndex(s->dateFormat());

    m_themeMode->setCurrentIndex(s->themeMode());
    const bool followSystemFont = s->fontFamily().isEmpty() || s->fontSize() <= 0;
    const QFont sysFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    m_useSystemFont->setChecked(followSystemFont);
    m_fontFamily->setCurrentFont(followSystemFont ? sysFont : QFont(s->fontFamily()));
    m_fontSize->setValue(followSystemFont ? sysFont.pointSize() : s->fontSize());
    m_fontFamily->setEnabled(!followSystemFont);
    m_fontSize->setEnabled(!followSystemFont);

    m_alternatingRowColors->setChecked(s->alternatingRowColors());
    m_toolbarIconSize->setCurrentIndex(s->toolbarIconSize());
    m_wireframeEnabled->setChecked(s->wireframeEnabled());
    m_wireframeThickness->setValue(s->wireframeThickness());
    m_wireframeThickness->setEnabled(s->wireframeEnabled());
    m_wireframeColor->setColor(QColor(s->wireframeColor()));
    m_wireframeColor->setEnabled(s->wireframeEnabled());

    m_restoreTabsOnStartup->setChecked(s->restoreTabsOnStartup());

    m_terminalCommand->setText(s->terminalCommand());
    m_folderOpenIn->setApps(s->folderOpenIn());
    m_fileOpenIn->setApps(s->fileOpenIn());
    m_jobDismissDelay->setValue(s->jobDismissDelay());
    m_suppressPlasmaJobs->setChecked(s->suppressPlasmaJobNotifications());
    m_thumbnailMaxSize->setValue(s->thumbnailMaxSizeMiB());
    m_velaWasDefault = isVelaDefault();
    m_defaultFileManager->setChecked(m_velaWasDefault);
}

void SettingsDialog::saveToSettings()
{
    VelaSettings *s = VelaSettings::self();

    s->setDefaultViewMode(m_defaultViewMode->currentIndex());
    s->setSingleClick(m_singleClick->isChecked());
    s->setShowHiddenDefault(m_showHiddenDefault->isChecked());
    s->setTrackRecent(m_trackRecent->isChecked());
    s->setConfirmDelete(m_confirmDelete->isChecked());
    s->setDateFormat(m_dateFormat->currentIndex());

    s->setThemeMode(m_themeMode->currentIndex());
    if (m_useSystemFont->isChecked()) {
        s->setFontFamily(QString()); // empty + 0 = follow the system font
        s->setFontSize(0);
    } else {
        s->setFontFamily(m_fontFamily->currentFont().family());
        s->setFontSize(m_fontSize->value());
    }

    s->setAlternatingRowColors(m_alternatingRowColors->isChecked());
    s->setToolbarIconSize(m_toolbarIconSize->currentIndex());
    s->setWireframeEnabled(m_wireframeEnabled->isChecked());
    s->setWireframeThickness(m_wireframeThickness->value());
    s->setWireframeColor(m_wireframeColor->color().name());

    s->setRestoreTabsOnStartup(m_restoreTabsOnStartup->isChecked());

    s->setTerminalCommand(m_terminalCommand->text());
    s->setFolderOpenIn(m_folderOpenIn->apps());
    s->setFileOpenIn(m_fileOpenIn->apps());
    s->setJobDismissDelay(m_jobDismissDelay->value());
    s->setSuppressPlasmaJobNotifications(m_suppressPlasmaJobs->isChecked());
    s->setThumbnailMaxSizeMiB(m_thumbnailMaxSize->value());

    // Only when the user actually toggled it. Previously every Apply/OK
    // rewrote mimeapps.list, deleted the .desktop file and icon, and ran two
    // blocking processes — to change, say, the date format.
    const bool wantDefault = m_defaultFileManager->isChecked();
    if (wantDefault != m_velaWasDefault) {
        if (setVelaAsDefault(wantDefault)) {
            m_velaWasDefault = wantDefault;
        } else {
            m_defaultFileManager->setChecked(m_velaWasDefault);
            QMessageBox::warning(this, i18n("Default File Manager"),
                                 i18n("Vela could not be registered as the default file manager."));
        }
    }

    s->save();
}

void SettingsDialog::onApply()
{
    saveToSettings();
    Q_EMIT settingsApplied();
}

void SettingsDialog::onRestoreDefaults()
{
    m_defaultViewMode->setCurrentIndex(0);
    m_singleClick->setChecked(false);
    m_showHiddenDefault->setChecked(false);
    m_trackRecent->setChecked(true);
    m_confirmDelete->setChecked(true);
    m_dateFormat->setCurrentIndex(0);

    m_themeMode->setCurrentIndex(0);
    const QFont sysFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    m_useSystemFont->setChecked(true);
    m_fontFamily->setCurrentFont(sysFont);
    m_fontSize->setValue(sysFont.pointSize());

    m_alternatingRowColors->setChecked(true);
    m_toolbarIconSize->setCurrentIndex(1);
    m_wireframeEnabled->setChecked(true);
    m_wireframeThickness->setValue(1);
    m_wireframeThickness->setEnabled(true);
    m_wireframeColor->setColor(QColor(QStringLiteral("#399CD0")));
    m_wireframeColor->setEnabled(true);

    m_restoreTabsOnStartup->setChecked(true);

    m_terminalCommand->setText(QStringLiteral("konsole"));
    m_folderOpenIn->setApps(VelaSettings::defaultOpenIn());
    m_fileOpenIn->setApps(VelaSettings::defaultOpenIn());
    m_jobDismissDelay->setValue(0);
    m_suppressPlasmaJobs->setChecked(false);
    m_thumbnailMaxSize->setValue(10);
    m_defaultFileManager->setChecked(false);
}

// ── Page builders ─────────────────────────────────────────────────────────────

QWidget *SettingsDialog::buildVelaPage()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    // App header
    auto *header = new QHBoxLayout;
    auto *iconLabel = new QLabel(page);
    iconLabel->setPixmap(QApplication::windowIcon().pixmap(48, 48));
    header->addWidget(iconLabel);
    auto *nameLabel = new QLabel(QStringLiteral("<b style=\"font-size:16pt\">Vela</b><br/>"
                                                "<small>File Manager</small>"), page);
    header->addWidget(nameLabel, 1);
    outer->addLayout(header);

    auto *sep = new QFrame(page);
    sep->setFrameShape(QFrame::HLine);
    outer->addWidget(sep);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_defaultFileManager = new QCheckBox(i18n("Set Vela as the default file manager"), page);
    form->addRow(i18n("System integration:"), m_defaultFileManager);

    outer->addLayout(form);
    outer->addStretch();
    return page;
}

QWidget *SettingsDialog::buildGeneralPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_defaultViewMode = new QComboBox(page);
    // Order must match Panel::ViewMode — the index is written straight to
    // DefaultViewMode. (It previously listed three entries for five modes, so
    // "Large Icons" actually selected Medium and the two largest modes were
    // unreachable from here.)
    m_defaultViewMode->addItems({i18n("Details"), i18n("Medium Icons"),
                                 i18n("Large Icons"), i18n("Extra Large Icons")});
    form->addRow(i18n("Default view mode:"), m_defaultViewMode);

    m_singleClick = new QCheckBox(i18n("Single-click to open files"), page);
    form->addRow(QString(), m_singleClick);

    m_showHiddenDefault = new QCheckBox(i18n("Show hidden files by default"), page);
    form->addRow(QString(), m_showHiddenDefault);

    m_trackRecent = new QCheckBox(i18n("Track recently visited folders in sidebar"), page);
    form->addRow(QString(), m_trackRecent);

    m_confirmDelete = new QCheckBox(i18n("Confirm before deleting"), page);
    form->addRow(QString(), m_confirmDelete);


    m_dateFormat = new QComboBox(page);
    m_dateFormat->addItems({i18n("DD/MM/YYYY"), i18n("MM/DD/YYYY")});
    form->addRow(i18n("Date format:"), m_dateFormat);

    return page;
}

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_themeMode = new QComboBox(page);
    m_themeMode->addItems({i18n("System default"), i18n("Light"), i18n("Dark")});
    form->addRow(i18n("Color theme:"), m_themeMode);

    m_useSystemFont = new QCheckBox(i18n("Use system font"), page);
    form->addRow(QString(), m_useSystemFont);

    m_fontFamily = new QFontComboBox(page);
    form->addRow(i18n("Font:"), m_fontFamily);

    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(6, 48);
    m_fontSize->setSuffix(i18n(" pt"));
    form->addRow(i18n("Font size:"), m_fontSize);

    connect(m_useSystemFont, &QCheckBox::toggled, this, [this](bool on) {
        m_fontFamily->setEnabled(!on);
        m_fontSize->setEnabled(!on);
    });

    m_alternatingRowColors = new QCheckBox(i18n("Alternating row colors in details view"), page);
    form->addRow(QString(), m_alternatingRowColors);

    m_toolbarIconSize = new QComboBox(page);
    m_toolbarIconSize->addItems({i18n("Small (16 px)"), i18n("Medium (22 px)"), i18n("Large (32 px)")});
    form->addRow(i18n("Toolbar icon size:"), m_toolbarIconSize);

    m_wireframeEnabled = new QCheckBox(i18n("Show window wireframe border"), page);
    form->addRow(QString(), m_wireframeEnabled);

    auto *wireframeContainer = new QWidget(page);
    auto *wireframeRow = new QHBoxLayout(wireframeContainer);
    wireframeRow->setContentsMargins(0, 0, 0, 0);
    m_wireframeThickness = new QSpinBox(wireframeContainer);
    m_wireframeThickness->setRange(1, 40);
    m_wireframeThickness->setSuffix(i18n(" px"));
    connect(m_wireframeEnabled, &QCheckBox::toggled, m_wireframeThickness, &QSpinBox::setEnabled);
    wireframeRow->addWidget(m_wireframeThickness);
    wireframeRow->addStretch();
    form->addRow(i18n("Wireframe thickness:"), wireframeContainer);

    m_wireframeColor = new KColorButton(page);
    connect(m_wireframeEnabled, &QCheckBox::toggled, m_wireframeColor, &KColorButton::setEnabled);
    form->addRow(i18n("Wireframe color:"), m_wireframeColor);


    return page;
}

QWidget *SettingsDialog::buildTabsPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_restoreTabsOnStartup = new QCheckBox(i18n("Restore previous session on startup"), page);
    form->addRow(QString(), m_restoreTabsOnStartup);

    return page;
}

QWidget *SettingsDialog::buildAdvancedPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_terminalCommand = new QLineEdit(page);
    m_terminalCommand->setPlaceholderText(QStringLiteral("konsole"));
    form->addRow(i18n("Terminal emulator command:"), m_terminalCommand);

    m_jobDismissDelay = new QSpinBox(page);
    m_jobDismissDelay->setRange(0, 30);
    m_jobDismissDelay->setSuffix(i18n(" s"));
    m_jobDismissDelay->setToolTip(
        i18n("How long a finished operation stays listed in the Operations window.\n"
             "Failed operations stay until dismissed."));
    form->addRow(i18n("Job auto-dismiss delay:"), m_jobDismissDelay);

    m_suppressPlasmaJobs = new QCheckBox(i18n("Hide file operations from Plasma notifications"), page);
    m_suppressPlasmaJobs->setToolTip(
        i18n("Progress is then shown only in Vela's Operations window.\n"
             "The system is still kept awake while operations run.\n"
             "Applies to operations started after the change."));
    form->addRow(QString(), m_suppressPlasmaJobs);

    m_thumbnailMaxSize = new QSpinBox(page);
    m_thumbnailMaxSize->setRange(1, 1024);
    m_thumbnailMaxSize->setSuffix(i18n(" MiB"));
    m_thumbnailMaxSize->setToolTip(
        i18n("Files larger than this keep their generic MIME icon in the icon views.\n"
             "Decoding a very large image to draw a small thumbnail costs a full read "
             "of the file, so the limit exists to keep browsing responsive.\n"
             "Raise it if your photos are routinely skipped."));
    form->addRow(i18n("Thumbnail size limit:"), m_thumbnailMaxSize);

    return page;
}

QWidget *SettingsDialog::buildOpenInPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *intro = new QLabel(i18n(
        "Applications pinned into the context menus, regardless of what the system "
        "\"Open With\" list offers. Each can appear directly in the menu or inside an "
        "\"Open In\" submenu."), page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    layout->addSpacing(8);
    layout->addWidget(new QLabel(i18n("<b>Folders</b> — right-click on a folder, or on the "
                                      "background of the open folder"), page));
    m_folderOpenIn = new OpenInListEditor(QStringLiteral("inode/directory"), page);
    layout->addWidget(m_folderOpenIn, 1);

    layout->addSpacing(8);
    layout->addWidget(new QLabel(i18n("<b>Files</b> — right-click on one or more files"), page));
    // No MIME type: a file list spans every type, so the chooser shows every app.
    m_fileOpenIn = new OpenInListEditor(QString(), page);
    layout->addWidget(m_fileOpenIn, 1);

    return page;
}

QWidget *SettingsDialog::buildCommandsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    if (m_commandPalette) {
        m_commandPalette->setParent(page);
        m_commandPalette->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_commandPalette->show();
        layout->addWidget(m_commandPalette);
    }
    return page;
}

void SettingsDialog::showCommandsPage()
{
    show();
    raise();
    activateWindow();
    for (int i = 0; i < m_pageList->count(); ++i) {
        if (m_pageList->item(i)->text() == i18n("Commands")) {
            m_pageList->setCurrentRow(i);
            break;
        }
    }
}
