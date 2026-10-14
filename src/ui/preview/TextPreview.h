#pragma once

#include <KFileItem>
#include <functional>
#include <KSyntaxHighlighting/Definition>
#include <QDateTime>
#include <QUrl>
#include <QWidget>

#include <atomic>
#include <memory>

class KDirWatch;
class KMessageWidget;
class QMenu;
class QPlainTextEdit;

namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
}

/**
 * Editable text preview with syntax highlighting.
 *
 * Reading: the whole file is decoded as UTF-8. If that fails the document is
 * shown anyway (lossy) but locked read-only — saving it would silently
 * rewrite bytes we could not represent. Line endings are detected on load and
 * written back unchanged, so opening a CRLF file does not turn it into LF.
 *
 * Writing: Ctrl+S or the pane's Save button, through QSaveFile, so a crash
 * mid-write cannot leave a truncated file.
 *
 * External changes: the file is watched. Any change on disk is announced in
 * a banner and the document is reloaded from disk — including over unsaved
 * edits, by design (see ToDo). Our own save is recognised by mtime + size
 * and does not trigger a reload.
 *
 * Reading and decoding (up to kMaxBytes) run on a worker; only putting the
 * text into the editor happens on the GUI thread. A burst of change
 * notifications (an editor's save is often several) is coalesced into one
 * reload.
 *
 * The watch is a private KDirWatch instance, never KDirWatch::self().
 * KCoreDirLister listens on the shared one, and a file registered there
 * makes every rename-over save (QSaveFile, Kate, Vim, git) surface as
 * deleted(file): the lister drops the row on the spot and re-adds it a
 * moment later, taking the panel's selection with it. On a private instance
 * the lister only ever sees the directory update — one refreshItems, row
 * and selection intact. (Verified against KIO 6.29; FileSystemModel keeps
 * its own instance for the same reason.)
 */
class TextPreview : public QWidget
{
    Q_OBJECT

public:
    // Above this the file is not opened here at all; the pane shows the
    // generic card instead. Highlighting a 200 MB log would stall the window.
    static constexpr qint64 kMaxBytes = 8 * 1024 * 1024;
    // Reading up to kMaxBytes is cheap; highlighting that much is not —
    // KSyntaxHighlighting runs on the GUI thread over the whole document. Above
    // this the file still opens, as plain text.
    static constexpr qint64 kMaxHighlightBytes = 1024 * 1024;

    explicit TextPreview(QWidget *parent = nullptr);
    ~TextPreview() override;

    // `mimeName` is the already-resolved type (may be empty); passed in so
    // the preview never determines it synchronously itself.
    void load(const KFileItem &item, const QString &mimeName);
    void clear();

    bool isModified() const;
    bool isReadOnly() const;
    // Writes the document. False (with a banner) when the write failed.
    bool save();
    // Throws away every edit and re-reads the file from disk.
    void discardChanges();

    // Soft-wraps long lines at word boundaries to the editor's width. Display
    // only: the file's line breaks are untouched.
    void setWordWrap(bool on);

    QUrl url() const { return m_url; }

    // The editor's own context menu (Undo, Copy, Paste, …), for the owner to
    // extend and show. Caller owns it.
    QMenu *createEditorMenu();

Q_SIGNALS:
    void modifiedChanged(bool modified);
    // Right-click in the editor. The owner shows createEditorMenu(), extended.
    void editorContextMenuRequested(const QPoint &globalPos);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    void applyDefinition(const QString &fileName, const QString &mimeName);
    // What the worker hands back: everything that does not touch a widget.
    struct ReadResult {
        bool       ok = false;
        bool       skipped = false; // superseded before it started
        QString    error;
        QString    text;            // decoded, CRLF already folded to LF
        qint64     size = 0;
        bool       bom = false;
        bool       decodeFailed = false;
        bool       mixedEol = false;
        bool       crlf = false;
        QDateTime  mtime;           // on-disk state the text was read from
        qint64     diskSize = -1;
    };
    static ReadResult readFile(const QString &path);

    // Reads the file into the editor asynchronously, keeping the cursor/scroll
    // where they were when `keepPosition` is set (reload-on-external-change).
    // `done(ok)` runs on the GUI thread once the text is in, unless a newer
    // load/clear superseded this one.
    void readFromDisk(bool keepPosition, std::function<void(bool)> done = {});
    void applyRead(const ReadResult &r, bool keepPosition);
    void watch(const QString &path);
    void unwatch();
    void onDiskChanged(const QString &path);
    void onDiskDeleted(const QString &path);
    void notify(const QString &text, bool warning = false);
    static KSyntaxHighlighting::Repository &repository();

    QPlainTextEdit *m_edit   = nullptr;
    KSyntaxHighlighting::Definition m_definition; // for the current file, if small enough
    KMessageWidget *m_banner = nullptr;
    KDirWatch      *m_watch  = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;

    QUrl    m_url;
    QString m_watchedPath;
    bool    m_crlf = false;
    bool    m_decodeFailed = false;
    bool    m_bom = false;       // file started with a UTF-8 BOM; save() writes it back
    bool    m_mixedEol = false;  // non-uniform line endings; shown read-only
    // Fingerprint of the last write we made, so KDirWatch's report of it is
    // recognised as ours and not treated as an external change.
    QDateTime m_lastWriteMtime;
    qint64    m_lastWriteSize = -1;

    int m_generation = 0; // bumped by load()/clear(); stale reads are dropped
    std::shared_ptr<std::atomic<int>> m_liveGeneration = std::make_shared<std::atomic<int>>(0);
    class QTimer *m_reloadDebounce = nullptr;
};
