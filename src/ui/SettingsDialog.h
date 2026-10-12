#pragma once

#include <QDialog>

class CommandPalette;
class OpenInListEditor;
class KColorButton;
class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;
class QSpinBox;
class QStackedWidget;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(CommandPalette *palette, QWidget *parent);

    void loadFromSettings();
    void showCommandsPage();

Q_SIGNALS:
    void settingsApplied();

private Q_SLOTS:
    void onApply();
    void onRestoreDefaults();

private:
    void saveToSettings();

    QWidget *buildVelaPage();
    QWidget *buildGeneralPage();
    QWidget *buildAppearancePage();
    QWidget *buildTabsPage();
    QWidget *buildAdvancedPage();
    QWidget *buildOpenInPage();
    QWidget *buildCommandsPage();

    CommandPalette *m_commandPalette = nullptr;
    QListWidget    *m_pageList  = nullptr;
    QStackedWidget *m_pageStack = nullptr;

    // General
    QComboBox *m_defaultViewMode   = nullptr;
    QCheckBox *m_singleClick       = nullptr;
    QCheckBox *m_showHiddenDefault = nullptr;
    QCheckBox *m_trackRecent       = nullptr;
    QCheckBox *m_confirmDelete     = nullptr;
    QComboBox *m_dateFormat        = nullptr;

    // Theme & Font
    QComboBox    *m_themeMode  = nullptr;
    QCheckBox    *m_useSystemFont = nullptr;
    QFontComboBox *m_fontFamily = nullptr;
    QSpinBox     *m_fontSize   = nullptr;

    // Appearance
    QCheckBox *m_alternatingRowColors = nullptr;
    QComboBox *m_toolbarIconSize      = nullptr;
    QCheckBox    *m_wireframeEnabled        = nullptr;
    QSpinBox     *m_wireframeThickness      = nullptr;
    KColorButton *m_wireframeColor          = nullptr;

    // Tabs
    QCheckBox *m_restoreTabsOnStartup = nullptr;

    // Advanced
    QLineEdit *m_terminalCommand      = nullptr;
    OpenInListEditor *m_folderOpenIn  = nullptr;
    OpenInListEditor *m_fileOpenIn    = nullptr;
    QSpinBox  *m_jobDismissDelay      = nullptr;
    QCheckBox *m_suppressPlasmaJobs   = nullptr;
    QSpinBox  *m_thumbnailMaxSize     = nullptr;
    QCheckBox *m_defaultFileManager   = nullptr;
    // What the system reported when the dialog was loaded. Apply only touches
    // the system integration when the checkbox differs from it.
    bool m_velaWasDefault = false;
};
