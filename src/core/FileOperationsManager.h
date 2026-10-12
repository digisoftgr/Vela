#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class KJob;
class QWidget;

namespace KIO {
class CopyJob;
}

class FileOperationsManager : public QObject
{
    Q_OBJECT

public:
    static FileOperationsManager *instance();

    // Every operation below except del() is recorded with
    // KIO::FileUndoManager, so Edit ▸ Undo can reverse it. None of them
    // replaces an existing file without asking: conflicts go through KIO's
    // Overwrite / Skip / Rename prompt.
    KIO::CopyJob *copy(const QList<QUrl> &sources, const QUrl &destDir, QWidget *window = nullptr);
    // Copies into `destDir` letting KIO pick a free "name (1)" for every
    // conflict, decided on the worker at copy time. For duplicating files into
    // the folder they already live in, where the only sensible answer to the
    // conflict is a new name.
    KIO::CopyJob *duplicate(const QList<QUrl> &sources, const QUrl &destDir, QWidget *window = nullptr);
    KIO::CopyJob *move(const QList<QUrl> &sources, const QUrl &destDir, QWidget *window = nullptr);
    KJob *trash(const QList<QUrl> &sources, QWidget *window = nullptr);
    KJob *del(const QList<QUrl> &sources, QWidget *window = nullptr); // not undoable
    KJob *mkdir(const QUrl &parentDir, const QString &name, QWidget *window = nullptr);
    // New empty file at `dest`; fails (reported by KIO) if it already exists.
    KJob *createEmptyFile(const QUrl &dest, QWidget *window = nullptr);

private:
    explicit FileOperationsManager(QObject *parent = nullptr);
};
