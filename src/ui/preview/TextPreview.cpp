#include "TextPreview.h"

#include "core/WorkerPools.h"

#include <KDirWatch>
#include <KFormat>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QStringConverter>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

KSyntaxHighlighting::Repository &TextPreview::repository()
{
    // Loading the definition index costs a few ms and some memory; once per
    // process is plenty, every pane in every window shares it.
    static KSyntaxHighlighting::Repository repo;
    return repo;
}

TextPreview::TextPreview(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_banner = new KMessageWidget(this);
    m_banner->setCloseButtonVisible(true);
    m_banner->setWordWrap(true);
    m_banner->hide();

    m_edit = new QPlainTextEdit(this);
    m_edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_edit->setFrameShape(QFrame::NoFrame);
    m_edit->setTabStopDistance(4 * m_edit->fontMetrics().horizontalAdvance(QLatin1Char(' ')));

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_edit->document());
    applyTheme();

    layout->addWidget(m_banner);
    layout->addWidget(m_edit, 1);

    connect(m_edit->document(), &QTextDocument::modificationChanged,
            this, &TextPreview::modifiedChanged);

    m_edit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_edit, &QPlainTextEdit::customContextMenuRequested, this, [this](const QPoint &pos) {
        Q_EMIT editorContextMenuRequested(m_edit->viewport()->mapToGlobal(pos));
    });

    auto *saveSc = new QShortcut(QKeySequence::Save, this);
    saveSc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveSc, &QShortcut::activated, this, &TextPreview::save);

    m_watch = new KDirWatch(this);
    connect(m_watch, &KDirWatch::dirty,   this, &TextPreview::onDiskChanged);
    connect(m_watch, &KDirWatch::created, this, &TextPreview::onDiskChanged);
    connect(m_watch, &KDirWatch::deleted, this, &TextPreview::onDiskDeleted);

    // One reload per burst of change notifications, not one per event.
    m_reloadDebounce = new QTimer(this);
    m_reloadDebounce->setSingleShot(true);
    m_reloadDebounce->setInterval(150);
    connect(m_reloadDebounce, &QTimer::timeout, this, [this]() {
        const bool hadEdits = isModified();
        readFromDisk(true, [this, hadEdits](bool ok) {
            if (!ok)
                return;
            notify(hadEdits
                ? i18n("File changed on disk — reloaded. Your unsaved edits were replaced.")
                : i18n("File changed on disk — reloaded."), hadEdits);
        });
    });
}

TextPreview::~TextPreview()
{
    unwatch();
}

bool TextPreview::isModified() const { return m_edit->document()->isModified(); }
bool TextPreview::isReadOnly() const { return m_edit->isReadOnly(); }

void TextPreview::setWordWrap(bool on)
{
    m_edit->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

// ── Loading ──────────────────────────────────────────────────────────────────

void TextPreview::clear()
{
    ++m_generation;
    m_liveGeneration->store(m_generation);
    m_reloadDebounce->stop();
    unwatch();
    m_url.clear();
    m_edit->clear();
    m_edit->document()->setModified(false);
    m_edit->setReadOnly(false);
    m_banner->hide();
    m_crlf = false;
    m_bom = false;
    m_decodeFailed = false;
    m_mixedEol = false;
}

void TextPreview::load(const KFileItem &item, const QString &mimeName)
{
    clear();
    m_url = item.url();
    if (!m_url.isLocalFile()) {
        m_edit->setPlainText(i18n("Text preview is only available for local files."));
        m_edit->setReadOnly(true);
        return;
    }
    applyDefinition(item.name(), mimeName);
    // Not editable until the text is in: a keystroke typed into the empty
    // editor would be thrown away by the load.
    m_edit->setReadOnly(true);
    const QString path = m_url.toLocalFile();
    readFromDisk(false, [this, path](bool ok) {
        if (ok)
            watch(path);
    });
}

TextPreview::ReadResult TextPreview::readFile(const QString &path)
{
    ReadResult r;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        r.error = file.errorString();
        return r;
    }
    const QByteArray bytes = file.readAll();
    r.ok = true;
    r.size = bytes.size();

    // The decoder drops a leading BOM; remember it so save() can put it back.
    r.bom = bytes.startsWith("\xEF\xBB\xBF");
    QStringDecoder decoder(QStringConverter::Utf8);
    r.text = decoder.decode(bytes);
    r.decodeFailed = decoder.hasError();

    // Line endings round-trip only when they are uniform. The editor keeps a
    // single kind, so a file mixing LF and CRLF (or with bare CRs) could not be
    // saved back as it was — it opens read-only instead of being silently
    // normalised on the next save.
    const qsizetype crlfCount = r.text.count(QLatin1String("\r\n"));
    const qsizetype lfCount   = r.text.count(QLatin1Char('\n'));
    const qsizetype crCount   = r.text.count(QLatin1Char('\r'));
    r.mixedEol = (crlfCount > 0 && crlfCount != lfCount) || crCount != crlfCount;
    r.crlf = crlfCount > 0 && !r.mixedEol;
    if (r.crlf)
        r.text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    return r;
}

void TextPreview::readFromDisk(bool keepPosition, std::function<void(bool)> done)
{
    const int generation = m_generation;
    const auto live = m_liveGeneration;
    const QString path = m_url.toLocalFile();
    auto *watcher = new QFutureWatcher<ReadResult>(this);
    connect(watcher, &QFutureWatcher<ReadResult>::finished, this,
            [this, watcher, generation, keepPosition, done]() {
        watcher->deleteLater();
        const ReadResult r = watcher->result();
        if (r.skipped || generation != m_generation)
            return; // another file (or none) is showing now
        applyRead(r, keepPosition);
        if (done)
            done(r.ok);
    });
    watcher->setFuture(QtConcurrent::run(WorkerPools::io(), [path, live, generation]() {
        if (live->load() != generation) {
            ReadResult skipped;
            skipped.skipped = true;
            return skipped;
        }
        return readFile(path);
    }));
}

void TextPreview::applyRead(const ReadResult &r, bool keepPosition)
{
    if (!r.ok) {
        m_edit->setPlainText(r.error);
        m_edit->setReadOnly(true);
        return;
    }
    m_bom = r.bom;
    m_decodeFailed = r.decodeFailed;
    m_mixedEol = r.mixedEol;
    m_crlf = r.crlf;

    const int cursorPos = m_edit->textCursor().position();
    const int scrollPos = m_edit->verticalScrollBar()->value();

    // Decided before setPlainText, which is when the highlighter would run.
    // Re-evaluated on every read: an external change can cross the limit.
    const bool highlight = r.size <= kMaxHighlightBytes;
    const KSyntaxHighlighting::Definition wanted =
        highlight ? m_definition : KSyntaxHighlighting::Definition();
    if (m_highlighter->definition() != wanted)
        m_highlighter->setDefinition(wanted);

    m_edit->setPlainText(r.text);
    m_edit->document()->setModified(false);
    m_edit->setReadOnly(m_decodeFailed || m_mixedEol);
    if (!highlight && m_definition.isValid())
        notify(i18n("Syntax highlighting is off for files larger than %1.",
                    KFormat().formatByteSize(kMaxHighlightBytes)));
    if (m_decodeFailed)
        notify(i18n("This file is not valid UTF-8. It is shown read-only so saving cannot corrupt it."), true);
    else if (m_mixedEol)
        notify(i18n("This file mixes line endings. It is shown read-only so saving cannot change them."), true);

    if (keepPosition) {
        QTextCursor c = m_edit->textCursor();
        c.setPosition(qMin(cursorPos, m_edit->document()->characterCount() - 1));
        m_edit->setTextCursor(c);
        m_edit->verticalScrollBar()->setValue(scrollPos);
    }
}

void TextPreview::applyDefinition(const QString &fileName, const QString &mimeName)
{
    auto &repo = repository();
    KSyntaxHighlighting::Definition def = repo.definitionForFileName(fileName);
    if (!def.isValid() && !mimeName.isEmpty())
        def = repo.definitionForMimeType(mimeName);
    // Remembered, not applied: readFromDisk applies it once it knows the size.
    // An invalid definition means plain text.
    m_definition = def;
}

void TextPreview::applyTheme()
{
    m_highlighter->setTheme(repository().themeForPalette(palette()));
    m_highlighter->rehighlight();
}

void TextPreview::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyTheme();
}

// ── Saving ───────────────────────────────────────────────────────────────────

bool TextPreview::save()
{
    if (m_url.isEmpty() || !m_url.isLocalFile() || m_edit->isReadOnly())
        return false;

    QString text = m_edit->toPlainText();
    if (m_crlf)
        text.replace(QLatin1String("\n"), QLatin1String("\r\n"));

    const QString path = m_url.toLocalFile();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        notify(i18n("Could not save: %1", file.errorString()), true);
        return false;
    }
    if (m_bom)
        file.write("\xEF\xBB\xBF");
    file.write(text.toUtf8());
    if (!file.commit()) {
        notify(i18n("Could not save: %1", file.errorString()), true);
        return false;
    }

    const QFileInfo fi(path);
    m_lastWriteMtime = fi.lastModified();
    m_lastWriteSize  = fi.size();
    m_edit->document()->setModified(false);
    m_banner->animatedHide();
    return true;
}

void TextPreview::discardChanges()
{
    if (m_url.isEmpty() || !m_url.isLocalFile() || !isModified())
        return;
    readFromDisk(true);
    m_banner->animatedHide();
}

// ── External changes ─────────────────────────────────────────────────────────

void TextPreview::watch(const QString &path)
{
    m_watchedPath = path;
    m_watch->addFile(path);
}

void TextPreview::unwatch()
{
    if (m_watchedPath.isEmpty())
        return;
    m_watch->removeFile(m_watchedPath);
    m_watchedPath.clear();
}

// INTENDED — do not flag as data loss: an external change reloads the file
// even over unsaved edits, with a banner saying so, and does not ask
// "reload or keep mine". Decided deliberately: the disk is the source of
// truth for this quick-edit pane, and a modal question fired by a background
// change (a build, git, another editor) is worse here than losing an edit
// the banner reports. Keep it this way.
void TextPreview::onDiskChanged(const QString &path)
{
    if (path != m_watchedPath)
        return;
    const QFileInfo fi(path);
    if (fi.lastModified() == m_lastWriteMtime && fi.size() == m_lastWriteSize)
        return; // our own write
    m_reloadDebounce->start();
}

void TextPreview::onDiskDeleted(const QString &path)
{
    if (path != m_watchedPath)
        return;
    // A rename-over save (ours or any editor's) unlinks the old inode, so
    // the watch reports "deleted" for a path that is very much still there.
    // Only a path that is actually gone gets the warning; the rest is a
    // change and handled as one.
    if (QFileInfo::exists(path)) {
        onDiskChanged(path);
        return;
    }
    notify(i18n("This file was deleted or moved on disk. Saving will recreate it."), true);
}

void TextPreview::notify(const QString &text, bool warning)
{
    m_banner->setMessageType(warning ? KMessageWidget::Warning : KMessageWidget::Information);
    m_banner->setText(text);
    m_banner->animatedShow();
}

QMenu *TextPreview::createEditorMenu()
{
    return m_edit->createStandardContextMenu();
}
