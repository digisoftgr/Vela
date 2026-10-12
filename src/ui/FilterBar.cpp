#include "FilterBar.h"

#include <KLocalizedString>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

FilterBar::FilterBar(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    auto *label = new QLabel(i18n("Filter:"), this);
    layout->addWidget(label);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setClearButtonEnabled(true);
    m_lineEdit->setPlaceholderText(i18n("Type to filter… (glob patterns ok, e.g. *.cpp)"));
    layout->addWidget(m_lineEdit, 1);

    m_subdirBtn = new QToolButton(this);
    m_subdirBtn->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
    m_subdirBtn->setCheckable(true);
    m_subdirBtn->setChecked(false);
    m_subdirBtn->setAutoRaise(true);
    m_subdirBtn->setToolTip(i18n("Search subdirectories"));
    layout->addWidget(m_subdirBtn);

    m_crossFsBtn = new QToolButton(this);
    m_crossFsBtn->setIcon(QIcon::fromTheme(QStringLiteral("drive-multidisk"),
                                           QIcon::fromTheme(QStringLiteral("drive-harddisk"))));
    m_crossFsBtn->setCheckable(true);
    m_crossFsBtn->setChecked(false);
    m_crossFsBtn->setAutoRaise(true);
    m_crossFsBtn->setToolTip(i18n("Also search other filesystems (mounted drives, network "
                                  "shares, /proc, /sys)"));
    m_crossFsBtn->setVisible(false); // meaningful only for a subfolder search
    layout->addWidget(m_crossFsBtn);

    m_countLabel = new QLabel(this);
    layout->addWidget(m_countLabel);

    m_closeBtn = new QToolButton(this);
    m_closeBtn->setIcon(QIcon::fromTheme(QStringLiteral("dialog-close")));
    m_closeBtn->setAutoRaise(true);
    m_closeBtn->setToolTip(i18n("Clear filter (Escape)"));
    layout->addWidget(m_closeBtn);

    connect(m_lineEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        Q_EMIT filterChanged(text);
    });
    connect(m_lineEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_lineEdit->text().isEmpty())
            Q_EMIT dismissed();
    });
    connect(m_subdirBtn, &QToolButton::toggled, this, [this](bool on) {
        m_crossFsBtn->setVisible(on);
        Q_EMIT subdirSearchToggled(on);
    });
    connect(m_crossFsBtn, &QToolButton::toggled, this, &FilterBar::crossFilesystemsToggled);
    connect(m_closeBtn, &QToolButton::clicked, this, [this]() {
        clearFilter();
        Q_EMIT dismissed();
    });

    m_lineEdit->installEventFilter(this);
}

bool FilterBar::subdirSearch() const
{
    return m_subdirBtn->isChecked();
}

void FilterBar::setSubdirSearch(bool on)
{
    m_subdirBtn->setChecked(on);
    m_crossFsBtn->setVisible(on);
}

bool FilterBar::crossFilesystems() const
{
    return m_crossFsBtn->isChecked();
}

void FilterBar::setCrossFilesystems(bool on)
{
    m_crossFsBtn->setChecked(on);
}

void FilterBar::setHint(const QString &text)
{
    m_countLabel->setText(text);
    m_countLabel->setVisible(!text.isEmpty());
}

bool FilterBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_lineEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            clearFilter();
            Q_EMIT dismissed();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FilterBar::clearFilter()
{
    m_lineEdit->clear();
}
