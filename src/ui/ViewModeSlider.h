#pragma once

#include <QPointer>
#include <QWidget>

class Panel;
class QAction;
class QTimer;
class QToolButton;

class ViewModeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit ViewModeSlider(QWidget *parent = nullptr);

    void setActivePanel(Panel *panel);
    // The window's checkable Show Terminal action (F4). Its button sits left
    // of Folder Sizes and mirrors the action's checked state.
    void setTerminalAction(QAction *action);

Q_SIGNALS:
    void viewModeChanged(int mode);
    void foldersFirstToggled(bool first);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private Q_SLOTS:
    void onHoverTimeout();

private:
    void updateIcons();
    void updateModeButton(int mode);
    void updateFoldersFirstButton(bool first);
    void updateFolderSizeButton(bool active);
    void updateTerminalButton(bool shown);
    void showModePopup();

    QToolButton *m_terminalBtn     = nullptr;  // leftmost: embedded terminal
    QToolButton *m_folderSizeBtn   = nullptr;  // folder size analysis
    QToolButton *m_foldersFirstBtn = nullptr;
    QToolButton *m_modeButton      = nullptr;
    QTimer      *m_hoverTimer      = nullptr;

    QPointer<QAction> m_terminalAction;
    Panel *m_activePanel = nullptr;
    QMetaObject::Connection m_viewModeConn;
    QMetaObject::Connection m_folderSizeConn;
};
