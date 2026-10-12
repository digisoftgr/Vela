#include "OperationsWindow.h"

#include "MessageBoxUtil.h"
#include "core/JobTracker.h"
#include "core/VelaSettings.h"

#include <KColorScheme>
#include <KIO/Global>
#include <KJob>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KSqueezedTextLabel>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// A job has to outlive this before it gets a row. Most pastes, renames and
// the stat/list jobs other components start finish well inside it.
static constexpr int kShowDelayMs = 1500;

static OperationsWindow *s_instance = nullptr;

OperationsWindow *OperationsWindow::instance()
{
    return s_instance;
}

OperationsWindow::OperationsWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    Q_ASSERT(!s_instance);
    s_instance = this;
    setWindowTitle(i18n("Operations — Vela"));
    // Appearing after the delay must not steal focus from whatever the user
    // is typing into; they can raise it themselves. (The compositor may
    // still decide otherwise on Wayland.)
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(560, 320);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);

    m_container = new QWidget;
    m_layout = new QVBoxLayout(m_container);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(6);
    m_layout->addStretch();
    scroll->setWidget(m_container);

    auto *tracker = JobTracker::instance();
    connect(tracker, &JobTracker::jobRegistered, this, &OperationsWindow::onJobRegistered);
    connect(tracker, &JobTracker::jobChanged, this, &OperationsWindow::onJobChanged);
    connect(tracker, &JobTracker::jobFinished, this, &OperationsWindow::onJobFinished);
    connect(tracker, &JobTracker::jobRemoved, this, &OperationsWindow::onJobRemoved);
}

OperationsWindow::~OperationsWindow()
{
    s_instance = nullptr;
}

// ── Tracker events ────────────────────────────────────────────────────────────

void OperationsWindow::onJobRegistered(KJob *job)
{
    m_pending.insert(job);
    QPointer<KJob> guarded(job);
    QTimer::singleShot(kShowDelayMs, this, [this, guarded]() {
        if (!guarded || !m_pending.remove(guarded))
            return;
        if (!JobTracker::instance()->info(guarded))
            return;
        addRow(guarded);
        showWindow();
    });
}

void OperationsWindow::onJobChanged(KJob *job)
{
    const auto it = m_rows.find(job);
    if (it == m_rows.end())
        return;
    if (const TrackedJob *info = JobTracker::instance()->info(job))
        updateRow(*it, *info);
}

void OperationsWindow::onJobFinished(KJob *job)
{
    if (m_pending.remove(job))
        return; // finished inside the show delay: never shown

    const auto it = m_rows.find(job);
    if (it == m_rows.end())
        return;
    const Row row = *it;
    m_rows.erase(it);
    notifyChanged();

    const TrackedJob *info = JobTracker::instance()->info(job);
    const int error = info ? info->error : KJob::NoError;
    QWidget *widget = row.widget;

    row.bar->setRange(0, 100);
    disconnect(row.button, &QToolButton::clicked, this, nullptr); // drop Cancel

    if (error != KJob::NoError && error != KJob::KilledJobError) {
        // Failures stay until dismissed. The error is also the only report of
        // an Ark compress failing: its plugin is gone by the time it happens.
        const QString message = info && !info->errorString.isEmpty()
                                    ? info->errorString
                                    : i18n("The operation failed.");
        row.stats->setText(i18n("Failed: %1", message));
        row.stats->setWordWrap(true);
        QPalette pal = row.stats->palette();
        pal.setColor(QPalette::WindowText,
                     KColorScheme(QPalette::Active, KColorScheme::Window)
                         .foreground(KColorScheme::NegativeText)
                         .color());
        row.stats->setPalette(pal);
        row.button->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear")));
        row.button->setToolTip(i18n("Dismiss"));
        row.button->setEnabled(true);
        connect(row.button, &QToolButton::clicked, this, [this, widget]() {
            removeRowWidget(widget);
        });
        m_finishedRows.insert(widget);
        showWindow();
        return;
    }

    if (!isVisible()) {
        // Running in the background: nobody is looking, nothing to report.
        removeRowWidget(widget);
        return;
    }

    row.bar->setValue(100);
    row.stats->setText(error == KJob::KilledJobError ? i18n("Cancelled") : i18n("Finished"));
    row.button->setEnabled(false);
    m_finishedRows.insert(widget);
    // Bound to the row: dismissing it or closing the window first cancels this.
    QTimer::singleShot(VelaSettings::self()->jobDismissDelay() * 1000, widget, [this, widget]() {
        removeRowWidget(widget);
    });
}

void OperationsWindow::onJobRemoved(KJob *job)
{
    m_pending.remove(job);
    const auto it = m_rows.find(job);
    if (it == m_rows.end())
        return;
    QWidget *widget = it->widget;
    m_rows.erase(it);
    removeRowWidget(widget);
}

// ── Rows ──────────────────────────────────────────────────────────────────────

void OperationsWindow::addRow(KJob *job)
{
    Row row;
    row.widget = new QFrame(m_container);
    static_cast<QFrame *>(row.widget)->setFrameShape(QFrame::StyledPanel);

    auto *layout = new QVBoxLayout(row.widget);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(2);

    auto *top = new QHBoxLayout;
    row.title = new QLabel(row.widget);
    QFont bold = row.title->font();
    bold.setBold(true);
    row.title->setFont(bold);
    top->addWidget(row.title, 1);

    row.button = new QToolButton(row.widget);
    row.button->setAutoRaise(true);
    row.button->setIcon(QIcon::fromTheme(QStringLiteral("dialog-cancel")));
    if (job->capabilities() & KJob::Killable) {
        row.button->setToolTip(i18n("Cancel"));
        QPointer<KJob> guarded(job);
        connect(row.button, &QToolButton::clicked, this, [guarded]() {
            if (guarded)
                guarded->kill(KJob::EmitResult);
        });
    } else {
        row.button->setEnabled(false);
        row.button->setToolTip(i18n("This operation cannot be cancelled"));
    }
    top->addWidget(row.button);
    layout->addLayout(top);

    // Paths are squeezed in the middle, where they are least informative.
    row.field1 = new KSqueezedTextLabel(row.widget);
    row.field1->setTextElideMode(Qt::ElideMiddle);
    layout->addWidget(row.field1);
    row.field2 = new KSqueezedTextLabel(row.widget);
    row.field2->setTextElideMode(Qt::ElideMiddle);
    layout->addWidget(row.field2);
    row.info = new KSqueezedTextLabel(row.widget);
    row.info->setTextElideMode(Qt::ElideMiddle);
    layout->addWidget(row.info);

    row.bar = new QProgressBar(row.widget);
    row.bar->setTextVisible(false);
    row.bar->setFixedHeight(10);
    layout->addWidget(row.bar);

    row.stats = new QLabel(row.widget);
    layout->addWidget(row.stats);

    m_layout->insertWidget(m_layout->count() - 1, row.widget); // before the stretch

    if (const TrackedJob *info = JobTracker::instance()->info(job))
        updateRow(row, *info);
    m_rows.insert(job, row);
    notifyChanged();

    // The window a job's dialogs (overwrite? skip?) are parented to is the
    // MainWindow it was started from. That window may now be closed while the
    // job keeps going; hand its dialogs to this window instead.
    if (QWidget *origin = KJobWidgets::window(job)) {
        QPointer<KJob> guarded(job);
        connect(origin, &QObject::destroyed, this, [this, guarded]() {
            if (guarded && JobTracker::instance()->info(guarded))
                KJobWidgets::setWindow(guarded, this);
        });
    }
}

void OperationsWindow::updateRow(Row &row, const TrackedJob &info)
{
    row.title->setText(info.title.isEmpty() ? i18n("Working…") : info.title);

    auto setField = [](KSqueezedTextLabel *label, const QPair<QString, QString> &field) {
        const QString text = field.first.isEmpty()
                                 ? field.second
                                 : i18nc("job field label: value", "%1: %2", field.first, field.second);
        label->setText(text);
        label->setToolTip(field.second);
        label->setVisible(!field.second.isEmpty());
    };
    setField(row.field1, info.field1);
    setField(row.field2, info.field2);

    const QString message = !info.warning.isEmpty() ? info.warning : info.infoMessage;
    row.info->setText(message);
    row.info->setVisible(!message.isEmpty());

    if (info.hasPercent) {
        row.bar->setRange(0, 100);
        row.bar->setValue(static_cast<int>(info.percent));
    } else {
        row.bar->setRange(0, 0); // busy: nothing measurable reported yet
    }

    QStringList parts;
    if (info.suspended)
        parts << i18n("Paused");

    const qulonglong bytesTotal = info.total.value(KJob::Bytes);
    const qulonglong bytesDone = info.processed.value(KJob::Bytes);
    if (bytesTotal > 0)
        parts << i18nc("processed of total size", "%1 of %2",
                       KIO::convertSize(bytesDone), KIO::convertSize(bytesTotal));
    else if (bytesDone > 0)
        parts << KIO::convertSize(bytesDone);

    const qulonglong filesTotal = info.total.value(KJob::Files);
    if (filesTotal > 1)
        parts << i18np("%2 of %1 file", "%2 of %1 files", filesTotal, info.processed.value(KJob::Files));

    if (info.speed > 0 && !info.suspended) {
        parts << i18n("%1/s", KIO::convertSize(info.speed));
        if (bytesTotal > bytesDone)
            parts << i18n("%1 remaining",
                          KIO::convertSeconds(KIO::calculateRemainingSeconds(bytesTotal, bytesDone, info.speed)));
    }

    row.stats->setText(parts.join(QStringLiteral(" · ")));
}

void OperationsWindow::removeRowWidget(QWidget *widget)
{
    m_finishedRows.remove(widget);
    delete widget;
    notifyChanged();
    closeIfEmpty();
}

void OperationsWindow::clearFinishedRows()
{
    const QSet<QWidget *> finished = m_finishedRows;
    m_finishedRows.clear();
    qDeleteAll(finished);
    notifyChanged();
}

// ── Window ────────────────────────────────────────────────────────────────────

void OperationsWindow::present()
{
    if (!hasContent())
        return;
    show();
    raise();
    activateWindow();
}

void OperationsWindow::showWindow()
{
    if (isVisible())
        return;
    show();
}

void OperationsWindow::closeIfEmpty()
{
    if (!m_rows.isEmpty() || !m_finishedRows.isEmpty() || !isVisible())
        return;
    // close(), not hide(): only a close lets Qt quit when this was the last
    // window and no job holds the application open any more.
    m_closingQuietly = true;
    close();
    m_closingQuietly = false;
}

void OperationsWindow::updateTitle()
{
    const int running = m_rows.size();
    setWindowTitle(running > 0
                       ? i18np("%1 Operation Running — Vela", "%1 Operations Running — Vela", running)
                       : i18n("Operations — Vela"));
}

void OperationsWindow::notifyChanged()
{
    updateTitle();
    Q_EMIT contentChanged();
}

void OperationsWindow::closeEvent(QCloseEvent *event)
{
    if (m_closingQuietly || m_rows.isEmpty()) {
        clearFinishedRows();
        event->accept();
        return;
    }

    QMessageBox box(QMessageBox::Warning,
                    i18n("Operations Still Running"),
                    i18np("An operation is still running.",
                          "%1 operations are still running.", m_rows.size()),
                    QMessageBox::NoButton, this);
    box.setInformativeText(
        i18n("Cancelling stops them now; anything already copied or written stays on disk, "
             "including partially written files and archives.\n\n"
             "Running them in the background hides this window; Vela stays open until they finish."));
    QPushButton *cancelOps = box.addButton(i18n("Cancel Operations"), QMessageBox::DestructiveRole);
    cancelOps->setIcon(QIcon::fromTheme(QStringLiteral("dialog-cancel")));
    QPushButton *background = box.addButton(i18n("Keep Running in Background"), QMessageBox::AcceptRole);
    QPushButton *goBack = box.addButton(i18n("Go Back"), QMessageBox::RejectRole);
    box.setDefaultButton(goBack);
    box.setEscapeButton(goBack);
    prepareMessageBox(box, 1);
    box.exec();

    if (box.clickedButton() == cancelOps) {
        // kill() with EmitResult finishes each job synchronously, which moves
        // it out of m_rows via onJobFinished(); iterate over a copy.
        const QList<KJob *> jobs = m_rows.keys();
        for (KJob *job : jobs) {
            if (job->capabilities() & KJob::Killable)
                job->kill(KJob::EmitResult);
        }
        clearFinishedRows();
        // A job that cannot be killed keeps its row and the window.
        if (!m_rows.isEmpty()) {
            event->ignore();
            return;
        }
        event->accept();
        return;
    }

    if (box.clickedButton() == background) {
        clearFinishedRows();
        event->accept();
        return;
    }

    event->ignore();
}
