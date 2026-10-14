#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

class FilterBar : public QWidget
{
    Q_OBJECT
public:
    explicit FilterBar(QWidget *parent = nullptr);

    QLineEdit *lineEdit() const { return m_lineEdit; }
    void clearFilter();

    bool subdirSearch() const;
    void setSubdirSearch(bool on);
    // Subfolder search only: descend into other filesystems (mounted drives,
    // network shares, /proc, /sys). Off by default, like `find -xdev`.
    bool crossFilesystems() const;
    void setCrossFilesystems(bool on);

    // Short note shown to the right of the field (empty hides it).
    void setHint(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void filterChanged(const QString &pattern);
    void dismissed();
    void subdirSearchToggled(bool on);
    void crossFilesystemsToggled(bool on);
    // A key meant for the file view (arrows, page keys, Enter on a non-empty
    // pattern): the selection moves while typing stays here.
    void keyForView(QKeyEvent *event);

private:
    QLineEdit   *m_lineEdit   = nullptr;
    QLabel      *m_countLabel = nullptr;
    QToolButton *m_subdirBtn  = nullptr;
    QToolButton *m_crossFsBtn = nullptr;
    QToolButton *m_closeBtn   = nullptr;
};
