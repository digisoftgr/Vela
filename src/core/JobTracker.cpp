#include "JobTracker.h"

#include "VelaSettings.h"

#include <KIO/JobTracker>
#include <KInhibitionJobTracker>
#include <QCoreApplication>
#include <QEventLoopLocker>
#include <QVariant>

JobTracker *JobTracker::instance()
{
    // Parented to the application rather than a function-local static, so it
    // is destroyed — and KIO handed back its own tracker — while KIO's globals
    // still exist.
    static JobTracker *s_instance = new JobTracker(QCoreApplication::instance());
    return s_instance;
}

JobTracker::JobTracker(QObject *parent)
    : KJobTrackerInterface(parent)
    , m_inhibition(new KInhibitionJobTracker(this))
{
}

JobTracker::~JobTracker()
{
    if (KIO::getJobTracker() == this)
        KIO::setJobTracker(m_previous);
}

void JobTracker::install()
{
    KJobTrackerInterface *current = KIO::getJobTracker();
    if (current == this)
        return;
    m_previous = current;
    KIO::setJobTracker(this);
}

void JobTracker::registerJob(KJob *job)
{
    if (!job || m_jobs.count(job))
        return;

    Entry &e = m_jobs[job];
    e.lock = std::make_unique<QEventLoopLocker>();

    // Chosen per job at registration, and remembered: toggling the setting
    // mid-job must not leave a job registered with one tracker and
    // unregistered from the other.
    if (!VelaSettings::self()->suppressPlasmaJobNotifications())
        e.forwardedTo = m_previous;
    else if (!job->property("kio_no_inhibit_suspend").toBool())
        e.forwardedTo = m_inhibition; // same opt-out KIO's own tracker honours

    // Both possible targets connect KJob::finished to their own unregisterJob,
    // so only registration is forwarded here — never unregistration on finish.
    if (e.forwardedTo)
        e.forwardedTo->registerJob(job);

    // Deliberately not KJobTrackerInterface::registerJob(): it connects
    // finished() to unregisterJob() *before* finished(), so the final state
    // would be discarded before it could be reported.
    connect(job, &KJob::finished, this, &JobTracker::onFinished);
    connect(job, &QObject::destroyed, this, [this](QObject *obj) {
        // A job deleted without ever emitting finished().
        auto *dead = static_cast<KJob *>(obj);
        if (m_jobs.count(dead)) {
            drop(dead);
            Q_EMIT jobRemoved(dead);
        }
    });
    connect(job, &KJob::description, this,
            [this](KJob *j, const QString &title, const QPair<QString, QString> &f1,
                   const QPair<QString, QString> &f2) {
                if (Entry *en = entry(j)) {
                    en->info.title = title;
                    en->info.field1 = f1;
                    en->info.field2 = f2;
                    Q_EMIT jobChanged(j);
                }
            });
    connect(job, &KJob::infoMessage, this, [this](KJob *j, const QString &message) {
        if (Entry *en = entry(j)) {
            en->info.infoMessage = message;
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::warning, this, [this](KJob *j, const QString &message) {
        if (Entry *en = entry(j)) {
            en->info.warning = message;
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::totalAmountChanged, this, [this](KJob *j, KJob::Unit unit, qulonglong amount) {
        if (Entry *en = entry(j)) {
            en->info.total.insert(unit, amount);
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::processedAmountChanged, this, [this](KJob *j, KJob::Unit unit, qulonglong amount) {
        if (Entry *en = entry(j)) {
            en->info.processed.insert(unit, amount);
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::percentChanged, this, [this](KJob *j, unsigned long pct) {
        if (Entry *en = entry(j)) {
            en->info.percent = pct;
            en->info.hasPercent = true;
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::speed, this, [this](KJob *j, unsigned long bytesPerSecond) {
        if (Entry *en = entry(j)) {
            en->info.speed = bytesPerSecond;
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::suspended, this, [this](KJob *j) {
        if (Entry *en = entry(j)) {
            en->info.suspended = true;
            Q_EMIT jobChanged(j);
        }
    });
    connect(job, &KJob::resumed, this, [this](KJob *j) {
        if (Entry *en = entry(j)) {
            en->info.suspended = false;
            Q_EMIT jobChanged(j);
        }
    });

    Q_EMIT jobRegistered(job);
}

void JobTracker::unregisterJob(KJob *job)
{
    // Reached only through an explicit call: this class never connects
    // finished() to it. The caller is withdrawing the job before it finished,
    // so the forwarded tracker has to be told as well.
    Entry *e = entry(job);
    if (!e)
        return;
    if (e->forwardedTo)
        e->forwardedTo->unregisterJob(job);
    drop(job);
    Q_EMIT jobRemoved(job);
}

const TrackedJob *JobTracker::info(KJob *job) const
{
    const auto it = m_jobs.find(job);
    return it == m_jobs.end() ? nullptr : &it->second.info;
}

int JobTracker::runningJobCount() const
{
    return static_cast<int>(m_jobs.size());
}

JobTracker::Entry *JobTracker::entry(KJob *job)
{
    const auto it = m_jobs.find(job);
    return it == m_jobs.end() ? nullptr : &it->second;
}

void JobTracker::onFinished(KJob *job)
{
    Entry *e = entry(job);
    if (!e)
        return;
    e->info.error = job->error();
    e->info.errorString = job->error() ? job->errorString() : QString();
    Q_EMIT jobFinished(job);
    // Releasing the lock last: if this was the final job and no window is
    // visible, the application quits once control returns to the event loop.
    drop(job);
}

void JobTracker::drop(KJob *job)
{
    const auto it = m_jobs.find(job);
    if (it == m_jobs.end())
        return;
    disconnect(job, nullptr, this, nullptr);
    m_jobs.erase(it);
}
