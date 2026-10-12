#pragma once

#include <QWidget>

class Panel;
class QTimer;
class QToolButton;

class ViewModeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit ViewModeSlider(QWidget *parent = nullptr);

    void setActivePanel(Panel *panel);

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
    void showModePopup();

    QToolButton *m_folderSizeBtn   = nullptr;  // leftmost: folder size analysis
    QToolButton *m_foldersFirstBtn = nullptr;
    QToolButton *m_modeButton      = nullptr;
    QTimer      *m_hoverTimer      = nullptr;

    Panel *m_activePanel = nullptr;
    QMetaObject::Connection m_viewModeConn;
    QMetaObject::Connection m_folderSizeConn;
};
