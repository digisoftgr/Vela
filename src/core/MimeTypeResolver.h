#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <atomic>
#include <memory>

// Resolves MIME types that can only be determined by reading file contents,
// off the GUI thread.
//
// Most files are classified by extension alone, which costs nothing. The
// remainder — extension-less files like Makefile, LICENSE, or a binary — need
// QMimeDatabase to sniff the first bytes. KFileItem::mimetype() does that
// synchronously ("a possibly slow operation", per its own documentation), so
// calling it from data() stalls painting; on a network mount it can freeze the
// window outright.
//
// Requests are deduplicated and answered on the GUI thread via resolved().
// All resolvers run on WorkerPools::io().
class MimeTypeResolver : public QObject
{
    Q_OBJECT

public:
    explicit MimeTypeResolver(QObject *parent = nullptr);
    ~MimeTypeResolver() override;

    // Enqueue `url` for content-based determination. Repeat calls for a URL
    // already in flight are ignored, so repainting the same row is free.
    void resolve(const QUrl &url);

    // Cancels everything requested so far: queued tasks skip their file read
    // and nothing already in flight is delivered.
    void clear();

Q_SIGNALS:
    // Always delivered on the GUI thread (queued from the worker).
    // `comment` is a .desktop entry's own Comment, empty otherwise. `iconName`
    // is the entry's Icon for a .desktop file, else the resolved type's icon.
    void resolved(const QUrl &url, const QString &mimeName, const QString &comment,
                  const QString &iconName);

private:
    // Shared with the tasks, which may outlive this object.
    struct Shared {
        std::atomic<int> generation{0};
    };

    std::shared_ptr<Shared> m_shared;
    QSet<QUrl> m_pending;
};
