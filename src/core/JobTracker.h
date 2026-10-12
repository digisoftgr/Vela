#pragma once

#include <KJob>
#include <KJobTrackerInterface>
#include <QHash>
#include <QPair>
#include <QString>

#include <memory>
#include <unordered_map>

class KInhibitionJobTracker;
class QEventLoopLocker;

// What a job has reported so far. KJob has no getters for its description or
// messages — they exist only as signal arguments — so they are kept here.
struct TrackedJob {
    QString title;
    QPair<QString, QString> field1;
    QPair<QString, QString> field2;
    QString infoMessage;
    QString warning;
    QHash<int, qulonglong> processed; // keyed by KJob::Unit
    QHash<int, qulonglong> total;     // keyed by KJob::Unit
    unsigned long percent = 0;
    bool hasPercent = false;
    unsigned long speed = 0;
    bool suspended = false;
    // Valid only during jobFinished().
    int error = KJob::NoError;
    QString errorString;
};

// Vela's KIO job tracker: every job that reports progress — copy, move, trash,
// delete, and jobs started by plugins such as Ark's Compress, whose job Vela
// never gets a pointer to — is registered here.
//
// It sits in front of the tracker KIOWidgets installs at library load (the one
// that feeds Plasma's notifications and inhibits sleep) and forwards each job to
// it, or — when Plasma notifications are suppressed — to a KInhibitionJobTracker
// alone, so sleep is still blocked while files are being written.
//
// Each running job also holds a QEventLoopLocker: Qt does not quit on the last
// window closing while one exists, so closing every Vela window no longer kills
// a half-finished copy or archive.
//
// Installed through KIO::setJobTracker(), which KIO marks \internal: it is how
// KIOWidgets itself registers, and the only way to see jobs created inside
// plugins. If KIO ever changes that, only install() needs revisiting.
class JobTracker : public KJobTrackerInterface
{
    Q_OBJECT

public:
    static JobTracker *instance();

    // Replaces KIO's tracker with this one. Call once, after the QApplication
    // exists and before any job is created.
    void install();

    void registerJob(KJob *job) override;
    void unregisterJob(KJob *job) override;

    // nullptr if the job is not (or no longer) tracked.
    const TrackedJob *info(KJob *job) const;
    int runningJobCount() const;

Q_SIGNALS:
    void jobRegistered(KJob *job);
    void jobChanged(KJob *job);
    // Emitted once, while info(job) is still valid; the job is dropped right after.
    void jobFinished(KJob *job);
    // Unregistered without finishing (explicit unregisterJob(), or destroyed).
    void jobRemoved(KJob *job);

private:
    explicit JobTracker(QObject *parent);
    ~JobTracker() override;

    struct Entry {
        TrackedJob info;
        KJobTrackerInterface *forwardedTo = nullptr;
        std::unique_ptr<QEventLoopLocker> lock;
    };

    Entry *entry(KJob *job);
    void onFinished(KJob *job);
    void drop(KJob *job);

    std::unordered_map<KJob *, Entry> m_jobs;
    KJobTrackerInterface *m_previous = nullptr;
    KInhibitionJobTracker *m_inhibition = nullptr;
};
