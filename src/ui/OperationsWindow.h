#pragma once

#include <QHash>
#include <QSet>
#include <QWidget>

class KJob;
class KSqueezedTextLabel;
class QLabel;
class QProgressBar;
class QToolButton;
class QVBoxLayout;
struct TrackedJob;

// The one progress UI for file operations: a standalone top-level window, not
// part of any MainWindow, listing every job JobTracker sees that is still
// running after a short delay — copy, move, trash, delete, Ark's Compress, and
// anything else started in-process that reports progress.
//
// Being its own window is the point: it keeps the application alive after the
// last browsing window closes, so the operation can finish. Closing it while
// jobs run asks whether to cancel them or let them continue in the background.
class OperationsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit OperationsWindow(QWidget *parent = nullptr);
    ~OperationsWindow() override;

    // The single instance created in main(); nullptr before it or after it.
    static OperationsWindow *instance();

    // Operations listed right now: running ones, whether or not the window is
    // visible (it may be hidden in the background).
    int runningCount() const { return m_rows.size(); }
    // Anything to look at: running operations, or finished ones still listed.
    bool hasContent() const { return !m_rows.isEmpty() || !m_finishedRows.isEmpty(); }
    // Explicit user request to see the window: shown, raised and focused.
    void present();

Q_SIGNALS:
    // runningCount() or hasContent() may have changed.
    void contentChanged();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct Row {
        QWidget *widget = nullptr;
        QLabel *title = nullptr;
        KSqueezedTextLabel *field1 = nullptr;
        KSqueezedTextLabel *field2 = nullptr;
        KSqueezedTextLabel *info = nullptr;
        QProgressBar *bar = nullptr;
        QLabel *stats = nullptr;
        QToolButton *button = nullptr;
    };

    void onJobRegistered(KJob *job);
    void onJobChanged(KJob *job);
    void onJobFinished(KJob *job);
    void onJobRemoved(KJob *job);

    void addRow(KJob *job);
    static void updateRow(Row &row, const TrackedJob &info);
    void removeRowWidget(QWidget *widget);
    void clearFinishedRows();
    void showWindow();
    void closeIfEmpty();
    void updateTitle();
    void notifyChanged();

    QVBoxLayout *m_layout = nullptr;
    QWidget *m_container = nullptr;
    // Running jobs that have a row. A row leaves this map when its job
    // finishes (the KJob is deleted soon after) but its widget may stay on
    // screen a while longer to show the outcome.
    QHash<KJob *, Row> m_rows;
    // Registered, still inside the show delay.
    QSet<KJob *> m_pending;
    // Rows of finished jobs still on screen.
    QSet<QWidget *> m_finishedRows;
    bool m_closingQuietly = false;
};
