#include "PreviewContainer.h"

#include "ui/MessageBoxUtil.h"

#include "core/VelaSettings.h"
#include "core/WorkerPools.h"

#include "FolderPreview.h"
#include "ImagePreview.h"
#include "PdfPreview.h"
#include "TextPreview.h"
#include "ui/Panel.h"

#include <KColorScheme>
#include <KFormat>
#include <KLocalizedString>

#include <QFile>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QApplication>
#include <QContextMenuEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>
#include <memory>

namespace {

// Page height when a child opens, as a share of the column's viewport, and
// the least any page may be dragged down to. The deepest container's page
// never gets less than kLeafMinHeight; past that the column scrolls.
constexpr double kSplitShare    = 0.4;
constexpr int    kMinBodyHeight = 120;
constexpr int    kLeafMinHeight = 200;
// Room at the left of a sub container for the line that shows its nesting.
constexpr int    kIndent        = 8;

// The theme icon flattened to one colour, keeping its shape (alpha). The
// disabled pixmap is the theme's own greyed one, so a button with nothing to
// act on does not look clickable in red or blue.
QIcon tintedIcon(const QString &name, const QColor &color, const QSize &size, qreal dpr)
{
    const QIcon base = QIcon::fromTheme(name);
    const QPixmap src = base.pixmap(size, dpr);
    QPixmap tinted(src.size());
    tinted.setDevicePixelRatio(src.devicePixelRatio());
    tinted.fill(Qt::transparent);
    QPainter p(&tinted);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(QRect(QPoint(0, 0), size), color);
    p.end();

    QIcon icon;
    icon.addPixmap(tinted, QIcon::Normal);
    icon.addPixmap(base.pixmap(size, dpr, QIcon::Disabled), QIcon::Disabled);
    return icon;
}

} // namespace

// ── Helper widgets ───────────────────────────────────────────────────────────

// A label that elides its text (in the middle) instead of demanding the
// width of all of it. A plain QLabel's minimum width is its whole text, which
// in a scrolling column makes one long file name widen every level.
class ElidedLabel : public QLabel
{
public:
    using QLabel::QLabel;

    QSize minimumSizeHint() const override
    {
        QSize s = QLabel::minimumSizeHint();
        s.setWidth(fontMetrics().averageCharWidth() * 4);
        return s;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(palette().color(foregroundRole()));
        const QRect r = contentsRect();
        p.drawText(r, int(alignment()) | Qt::TextSingleLine,
                   fontMetrics().elidedText(text(), Qt::ElideMiddle, r.width()));
    }
};

// Icon + name, drawn as a link. Clicking it runs `onClick`. The header's one
// name: a folder's or a file's.
class ItemLink : public QWidget
{
public:
    explicit ItemLink(QWidget *parent)
        : QWidget(parent)
    {
        auto *l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        m_icon = new QLabel(this);
        m_text = new ElidedLabel(this);
        m_text->setTextFormat(Qt::PlainText);
        m_text->setForegroundRole(QPalette::Link);
        m_text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        QFont f = m_text->font();
        f.setBold(true);
        m_text->setFont(f);
        l->addWidget(m_icon);
        l->addWidget(m_text, 1);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClick;

    void setTarget(const QString &iconName, const QString &text, const QString &toolTip)
    {
        m_iconName = iconName;
        m_text->setText(text);
        setToolTip(toolTip);
        updateIcon();
    }

protected:
    void enterEvent(QEnterEvent *) override { setUnderline(true); }
    void leaveEvent(QEvent *) override { setUnderline(false); }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())
            && onClick)
            onClick();
    }
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::DevicePixelRatioChange)
            updateIcon();
    }

private:
    void updateIcon()
    {
        const int s = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        m_icon->setPixmap(QIcon::fromTheme(m_iconName, QIcon::fromTheme(QStringLiteral("unknown")))
                              .pixmap(s, s));
    }
    void setUnderline(bool on)
    {
        QFont f = m_text->font();
        f.setUnderline(on);
        m_text->setFont(f);
    }

    QString      m_iconName;
    QLabel      *m_icon = nullptr;
    ElidedLabel *m_text = nullptr;
};

// The grip between a page and its child. Dragging it sets the page height.
class SectionHandle : public QWidget
{
public:
    explicit SectionHandle(QWidget *parent)
        : QWidget(parent)
    {
        setFixedHeight(8);
        setCursor(Qt::SplitVCursor);
        setToolTip(i18n("Drag to resize"));
    }

    std::function<int()>     currentHeight;
    std::function<void(int)> setHeight;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        const int y = height() / 2;
        p.drawLine(0, y, width(), y);
        // A short grip in the middle, so it reads as draggable.
        const int cx = width() / 2;
        p.drawLine(cx - 12, y - 2, cx + 12, y - 2);
        p.drawLine(cx - 12, y + 2, cx + 12, y + 2);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        m_startY = event->globalPosition().y();
        m_startHeight = currentHeight ? currentHeight() : 0;
        m_dragging = true;
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging && setHeight)
            setHeight(m_startHeight + int(event->globalPosition().y() - m_startY));
    }
    void mouseReleaseEvent(QMouseEvent *) override { m_dragging = false; }

private:
    bool  m_dragging = false;
    qreal m_startY = 0;
    int   m_startHeight = 0;
};


// ── Construction ─────────────────────────────────────────────────────────────

PreviewContainer::PreviewContainer(int depth, std::function<Panel *()> mainPanel, QWidget *parent)
    : QWidget(parent)
    , m_depth(depth)
    , m_mainPanel(std::move(mainPanel))
{
    // Click focus, so a click on a part that takes none itself (the header,
    // an image) makes this the container Esc closes.
    setFocusPolicy(Qt::ClickFocus);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(depth > 0 ? kIndent : 0, 0, 0, 0);
    m_layout->setSpacing(0);

    // Row 1: the item's button, close (sub containers).
    m_linkRow = new QWidget(this);
    auto *linkLayout = new QHBoxLayout(m_linkRow);
    linkLayout->setContentsMargins(4, 4, 4, 2);
    linkLayout->setSpacing(2);
    m_link = new ItemLink(m_linkRow);
    m_link->onClick = [this]() {
        if (m_item.isNull())
            return;
        if (m_item.isDir())
            Q_EMIT revealRequested(folderTarget(), folderReveal());
        else if (m_source)
            m_source->openItem(m_item); // as a double-click on it would
    };
    m_closeBtn = new QToolButton(m_linkRow);
    m_closeBtn->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    m_closeBtn->setToolTip(i18n("Close this preview (Esc)"));
    m_closeBtn->setAutoRaise(true);
    m_closeBtn->setVisible(depth > 0);
    connect(m_closeBtn, &QToolButton::clicked, this, &PreviewContainer::closeRequested);
    linkLayout->addSpacing(2);
    linkLayout->addWidget(m_link, 1);
    linkLayout->addWidget(m_closeBtn);

    // Row 2: the text editor's buttons, shown for text only.
    m_editRow = new QWidget(this);
    auto *editLayout = new QHBoxLayout(m_editRow);
    editLayout->setContentsMargins(8, 0, 4, 4);
    editLayout->setSpacing(4);

    m_wrapBtn = new QToolButton(m_editRow);
    m_wrapBtn->setIcon(QIcon::fromTheme(QStringLiteral("text-wrap")));
    m_wrapBtn->setText(i18n("Wrap"));
    m_wrapBtn->setToolTip(i18n("Wrap long lines to the pane width"));
    m_wrapBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_wrapBtn->setCheckable(true);
    m_wrapBtn->setAutoRaise(true);

    m_discardBtn = new QToolButton(m_editRow);
    m_discardBtn->setText(i18n("Discard all changes"));
    m_discardBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_discardBtn->setToolTip(i18n("Throw away every edit and reload the file from disk"));
    m_discardBtn->setAutoRaise(true);
    m_discardBtn->setEnabled(false);

    m_saveBtn = new QToolButton(m_editRow);
    m_saveBtn->setText(i18n("Save changes"));
    m_saveBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_saveBtn->setToolTip(i18n("Save (Ctrl+S)"));
    m_saveBtn->setAutoRaise(true);
    m_saveBtn->setEnabled(false);
    // Flat like Discard; the tinted icon (see applyEditIcons) is what signals
    // there is something to save. A highlight background would swallow it.
    m_saveBtn->setStyleSheet(QStringLiteral("QToolButton { padding: 2px 8px; border-radius: 4px; }"));
    m_discardBtn->setStyleSheet(QStringLiteral("QToolButton { padding: 2px 8px; border-radius: 4px; }"));
    // A stylesheet drops the native checked look, so spell the on state out.
    m_wrapBtn->setStyleSheet(QStringLiteral(
        "QToolButton { padding: 2px 8px; border-radius: 4px; }"
        "QToolButton:checked { background: palette(midlight); border: 1px solid palette(mid); }"));

    applyEditIcons();

    editLayout->addStretch(1);
    editLayout->addWidget(m_wrapBtn);
    editLayout->addWidget(m_discardBtn);
    editLayout->addWidget(m_saveBtn);
    m_editRow->hide();

    m_stack = new QStackedWidget(this);
    m_stack->setMinimumHeight(kLeafMinHeight);
    // The message page is the one always there: it is the resting state.
    m_message = new QLabel(m_stack);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_pages[Message] = m_message;
    m_stack->addWidget(m_message);

    m_handle = new SectionHandle(this);
    m_handle->currentHeight = [this]() { return m_stack->height(); };
    m_handle->setHeight = [this](int h) { setBodyHeight(h); };
    m_handle->hide();

    m_layout->addWidget(m_linkRow);
    m_layout->addWidget(m_editRow);
    m_layout->addWidget(m_stack, 1);
    m_layout->addWidget(m_handle);

    // toggled, not clicked: presentFile() syncs the button from the setting
    // with setChecked(), and that must reach the editor too.
    connect(m_wrapBtn, &QToolButton::toggled, this, [this](bool on) {
        if (auto *text = static_cast<TextPreview *>(m_pages[Text]))
            text->setWordWrap(on);
    });
    connect(m_wrapBtn, &QToolButton::clicked, this, [](bool on) {
        VelaSettings::setPreviewWordWrap(on);
    });

    // A held arrow key in the live folder view changes its selection dozens
    // of times a second; the child only needs the one it lands on.
    m_childDebounce = new QTimer(this);
    m_childDebounce->setSingleShot(true);
    m_childDebounce->setInterval(80);
    connect(m_childDebounce, &QTimer::timeout, this, &PreviewContainer::syncChild);

    updateHeader();
}

// Children are QObject children; nothing to wait for. The page widgets
// already cope with being destroyed mid-load (see their destructors).
PreviewContainer::~PreviewContainer() = default;

void PreviewContainer::setSource(Panel *source)
{
    m_source = source;
}

// ── Pages ────────────────────────────────────────────────────────────────────

FolderPreview *PreviewContainer::folderPage()
{
    if (!m_pages[Folder]) {
        auto *folder = new FolderPreview(m_stack);
        // Each gesture fires inside the snapshot's own event handling, and
        // going live hides the snapshot and moves focus — so later, not now.
        auto later = [this](Gesture g, const KFileItem &target, const QPoint &pos = {}) {
            QTimer::singleShot(0, this, [this, g, target, pos]() { goLive(g, target, pos); });
        };
        connect(folder, &FolderPreview::engaged, this, [later]() {
            later(Gesture::Focus, KFileItem());
        });
        connect(folder, &FolderPreview::rowClicked, this, [later](const KFileItem &item) {
            later(Gesture::Click, item);
        });
        connect(folder, &FolderPreview::contextMenuRequested, this,
                [later](const KFileItem &item, const QPoint &pos) {
            later(Gesture::Menu, item, pos);
        });
        m_pages[Folder] = folder;
        m_stack->addWidget(folder);
    }
    return static_cast<FolderPreview *>(m_pages[Folder]);
}

ImagePreview *PreviewContainer::imagePage()
{
    if (!m_pages[Image]) {
        m_pages[Image] = new ImagePreview(m_stack);
        m_stack->addWidget(m_pages[Image]);
    }
    return static_cast<ImagePreview *>(m_pages[Image]);
}

PdfPreview *PreviewContainer::pdfPage()
{
    if (!m_pages[Pdf]) {
        m_pages[Pdf] = new PdfPreview(m_stack);
        m_stack->addWidget(m_pages[Pdf]);
    }
    return static_cast<PdfPreview *>(m_pages[Pdf]);
}

TextPreview *PreviewContainer::textPage()
{
    if (!m_pages[Text]) {
        auto *text = new TextPreview(m_stack);
        connect(m_saveBtn,    &QToolButton::clicked, text, &TextPreview::save);
        connect(m_discardBtn, &QToolButton::clicked, text, &TextPreview::discardChanges);
        connect(text, &TextPreview::modifiedChanged, m_saveBtn,    &QToolButton::setEnabled);
        connect(text, &TextPreview::modifiedChanged, m_discardBtn, &QToolButton::setEnabled);
        // The editor's own menu, with the file's menu as a submenu of it.
        connect(text, &TextPreview::editorContextMenuRequested, this,
                [this, text](const QPoint &globalPos) {
            std::unique_ptr<QMenu> menu(text->createEditorMenu());
            if (m_source && !m_item.isNull())
                m_source->showItemContextMenu(m_item, globalPos, menu.get());
            else
                menu->exec(globalPos);
        });
        m_pages[Text] = text;
        m_stack->addWidget(text);
    }
    return static_cast<TextPreview *>(m_pages[Text]);
}

QWidget *PreviewContainer::genericPage()
{
    if (!m_pages[Generic]) {
        auto *page = new QWidget(m_stack);
        auto *gl = new QVBoxLayout(page);
        gl->setContentsMargins(16, 16, 16, 16);
        gl->addStretch(2);
        m_genericIcon = new QLabel(page);
        m_genericIcon->setAlignment(Qt::AlignCenter);
        m_genericText = new QLabel(page);
        m_genericText->setAlignment(Qt::AlignCenter);
        m_genericText->setWordWrap(true);
        m_genericText->setTextFormat(Qt::PlainText);
        gl->addWidget(m_genericIcon);
        gl->addSpacing(8);
        gl->addWidget(m_genericText);
        gl->addStretch(3);
        m_pages[Generic] = page;
        m_stack->addWidget(page);
    }
    return m_pages[Generic];
}

void PreviewContainer::clearPagesExcept(Page keep)
{
    if (keep != Live)
        dropLive();
    if (keep != Folder && m_pages[Folder])
        static_cast<FolderPreview *>(m_pages[Folder])->clear();
    if (keep != Image && m_pages[Image])
        static_cast<ImagePreview *>(m_pages[Image])->clear();
    if (keep != Pdf && m_pages[Pdf])
        static_cast<PdfPreview *>(m_pages[Pdf])->clear();
    if (keep != Text && m_pages[Text])
        static_cast<TextPreview *>(m_pages[Text])->clear();
}

void PreviewContainer::setCurrentPage(Page page)
{
    m_stack->setCurrentWidget(m_pages[page]);
}

// ── Content ──────────────────────────────────────────────────────────────────

void PreviewContainer::showMessage(const QString &text)
{
    closeChild(false);
    ++m_classifyGeneration;
    m_item = KFileItem();
    m_editRow->hide();
    clearPagesExcept(Message);
    m_message->setText(text);
    setCurrentPage(Message);
    updateHeader();
}

void PreviewContainer::showItem(const KFileItem &item, bool showHidden)
{
    closeChild(false);
    ++m_classifyGeneration;
    m_item = item;
    m_showHidden = showHidden;
    m_editRow->hide();
    // iconName() would determine an unknown type itself, synchronously.
    m_fileIcon = item.isMimeTypeKnown() ? item.iconName() : QStringLiteral("unknown");
    updateHeader();

    if (item.isDir()) {
        // The snapshot; the full view only once the user reaches for it.
        clearPagesExcept(Folder);
        folderPage()->setFolder(item.url(), showHidden);
        setCurrentPage(Folder);
        return;
    }

    // Classify without touching the file when possible. item.mimetype() would
    // sniff the contents on the GUI thread (a stall, or a freeze on a hung
    // network mount); the known type or the name decides the vast majority.
    QString mimeName;
    if (item.isMimeTypeKnown()) {
        mimeName = item.currentMimeType().name();
    } else {
        static QMimeDatabase db;
        const QMimeType byName = db.mimeTypeForFile(item.name(), QMimeDatabase::MatchExtension);
        if (byName.isValid())
            mimeName = byName.name();
    }
    static const QString kDefaultMime = QStringLiteral("application/octet-stream");
    if (!mimeName.isEmpty() && mimeName != kDefaultMime) {
        presentFile(item, pageForMime(mimeName), mimeName);
        return;
    }

    // Undecided by name (no or unknown extension). Remote files get the card;
    // local ones are sniffed — type, then "no NUL in the first 8 KB" for text,
    // the heuristic `file` and git use — on a worker.
    if (!item.isLocalFile()) {
        presentFile(item, Generic, kDefaultMime);
        return;
    }
    clearPagesExcept(Message);
    m_message->clear();
    setCurrentPage(Message);

    const int generation = m_classifyGeneration;
    const QString path = item.localPath();
    const KIO::filesize_t size = item.size();
    auto *watcher = new QFutureWatcher<QPair<int, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<int, QString>>::finished, this,
            [this, watcher, generation, item]() {
        watcher->deleteLater();
        if (generation != m_classifyGeneration)
            return; // the item changed while we were reading
        const auto [page, mime] = watcher->result();
        presentFile(item, static_cast<Page>(page), mime);
    });
    watcher->setFuture(QtConcurrent::run(WorkerPools::io(), [path, size]() -> QPair<int, QString> {
        QMimeDatabase db;
        const QMimeType mime = db.mimeTypeForFile(path); // may read content
        Page page = pageForMime(mime.name());
        if (page == Generic && mime.isDefault() && size > 0
            && size <= KIO::filesize_t(TextPreview::kMaxBytes)) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly) && !f.read(8192).contains('\0'))
                page = Text;
        }
        return {int(page), mime.name()};
    }));
}

void PreviewContainer::presentFile(const KFileItem &item, Page page, const QString &mimeName)
{
    // The resolved type has the right icon for the file's button.
    {
        static QMimeDatabase db;
        const QMimeType mime = db.mimeTypeForName(mimeName);
        if (mime.isValid()) {
            m_fileIcon = mime.iconName();
            updateHeader();
        }
    }
    // PDF before image is decided in pageForMime.
    if (page == Pdf) {
        clearPagesExcept(Pdf);
        pdfPage()->setDocument(item.url());
        setCurrentPage(Pdf);
        return;
    }
    if (page == Image) {
        clearPagesExcept(Image);
        imagePage()->setImage(item.url());
        setCurrentPage(Image);
        return;
    }
    if (page == Text) {
        if (item.size() > TextPreview::kMaxBytes) {
            clearPagesExcept(Generic);
            showGeneric(item, mimeName,
                        i18n("Too large to open as text (limit %1).",
                             KFormat().formatByteSize(TextPreview::kMaxBytes)));
            return;
        }
        clearPagesExcept(Text);
        TextPreview *text = textPage();
        text->load(item, mimeName);
        // Re-read every time: another window may have flipped it.
        const bool wrap = VelaSettings::previewWordWrap();
        m_wrapBtn->setChecked(wrap);
        text->setWordWrap(wrap);
        m_saveBtn->setEnabled(false);
        m_discardBtn->setEnabled(false);
        m_saveBtn->setVisible(!text->isReadOnly());
        m_discardBtn->setVisible(!text->isReadOnly());
        m_editRow->show();
        setCurrentPage(Text);
        return;
    }
    clearPagesExcept(Generic);
    showGeneric(item, mimeName);
}

void PreviewContainer::showGeneric(const KFileItem &item, const QString &mimeName,
                                   const QString &note)
{
    genericPage();
    // Icon and comment come from the resolved type. item.iconName() and
    // item.mimeComment() would determine the type themselves — synchronously.
    static QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForName(mimeName);
    m_genericIcon->setPixmap(QIcon::fromTheme(mime.iconName(),
                                              QIcon::fromTheme(mime.genericIconName(),
                                                  QIcon::fromTheme(QStringLiteral("unknown"))))
                                 .pixmap(96, 96));
    QStringList lines;
    lines << item.name();
    lines << mime.comment();
    lines << KFormat().formatByteSize(item.size());
    const QDateTime mtime = item.time(KFileItem::ModificationTime);
    if (mtime.isValid())
        lines << i18n("Modified %1", QLocale().toString(mtime, QLocale::ShortFormat));
    if (!note.isEmpty())
        lines << QString() << note;
    m_genericText->setText(lines.join(QLatin1Char('\n')));
    setCurrentPage(Generic);
}

// ── Header ───────────────────────────────────────────────────────────────────

void PreviewContainer::updateHeader()
{
    const bool has = !m_item.isNull();
    const bool dir = has && m_item.isDir();
    if (dir) {
        const QUrl folder = folderTarget();
        const QString path = folder.toDisplayString(QUrl::PreferLocalFile);
        const QString name = folder.adjusted(QUrl::StripTrailingSlash).fileName();
        m_link->setTarget(QStringLiteral("folder"), name.isEmpty() ? path : name,
                          i18n("Open in the tab: %1", path));
    } else if (has) {
        m_link->setTarget(m_fileIcon, m_item.name(),
                          i18n("Open %1", m_item.url().toDisplayString(QUrl::PreferLocalFile)));
    }
    m_link->setVisible(has);
    m_linkRow->setVisible(has || m_depth > 0);
}

QUrl PreviewContainer::folderTarget() const
{
    return m_live ? m_live->currentUrl() : m_item.url();
}

QUrl PreviewContainer::folderReveal() const
{
    if (!m_live)
        return {};
    int count = 0;
    const KFileItem selected = m_live->soleSelectedItem(count);
    return count == 1 ? selected.url() : QUrl();
}

// ── Live folder view ─────────────────────────────────────────────────────────

void PreviewContainer::goLive(Gesture gesture, const KFileItem &target, const QPoint &globalPos)
{
    if (m_live || m_item.isNull() || !m_item.isDir())
        return;

    // What to select once the folder has listed: the row clicked or
    // right-clicked.
    QUrl reveal;
    if (!target.isNull() && (gesture == Gesture::Click || gesture == Gesture::Menu))
        reveal = target.url();

    m_live = Panel::createEmbedded(m_mainPanel, m_stack);
    m_pages[Live] = m_live;
    m_stack->addWidget(m_live);
    connect(m_live, &Panel::previewItemChanged, m_childDebounce, qOverload<>(&QTimer::start));
    connect(m_live, &Panel::urlChanged, this, &PreviewContainer::updateHeader);
    connect(m_live, &Panel::navigationStateChanged, this, &PreviewContainer::updateHeader);
    connect(m_live, &Panel::previewToggleRequested,
            this, &PreviewContainer::previewToggleRequested);
    connect(m_live, &Panel::navigationStateChanged,
            this, &PreviewContainer::liveNavigationChanged);
    if (gesture == Gesture::Menu) {
        // The menu acts on the view's selection, so it waits for the listing.
        QPointer<Panel> live = m_live;
        connect(m_live, &Panel::directoryVisited, this, [live, target, globalPos]() {
            // After the load's own selection pass, which runs right after this.
            QTimer::singleShot(0, live, [live, target, globalPos]() {
                if (live)
                    live->showItemContextMenu(target, globalPos);
            });
        }, Qt::SingleShotConnection);
    }

    // As the panel above shows it, like the snapshot did; not the default.
    m_live->model()->setShowHidden(m_showHidden);
    m_live->startEmbedded(m_item.url(), reveal);
    if (auto *snapshot = static_cast<FolderPreview *>(m_pages[Folder]))
        snapshot->clear();
    setCurrentPage(Live);
    m_live->focusView();
    updateHeader();

    if (gesture == Gesture::Click && !target.isNull()) {
        // The clicked item opens below now, from what the snapshot knows,
        // not once the listing is in. The listing then selects the same item
        // and syncChild finds nothing to change.
        m_pendingChild = target.url();
        connect(m_live, &Panel::directoryVisited, this, [this]() {
            // After the load's own selection pass: if the item did not turn
            // up (gone meanwhile), the child follows the real selection.
            QTimer::singleShot(0, this, [this]() {
                m_pendingChild.clear();
                syncChild();
            });
        }, Qt::SingleShotConnection);
        ensureChild()->showItem(target, m_showHidden);
        startDoubleClickCatch(target);
    }
}

void PreviewContainer::startDoubleClickCatch(const KFileItem &target)
{
    if (!m_catchTimer) {
        m_catchTimer = new QTimer(this);
        m_catchTimer->setSingleShot(true);
        connect(m_catchTimer, &QTimer::timeout, this, [this]() {
            if (!m_swallowRelease) // else: until its release
                stopDoubleClickCatch();
        });
    }
    m_catchTarget = target;
    m_swallowRelease = false;
    if (!m_catching) {
        qApp->installEventFilter(this);
        m_catching = true;
    }
    m_catchTimer->start(QApplication::doubleClickInterval());
}

void PreviewContainer::stopDoubleClickCatch()
{
    if (m_catching) {
        qApp->removeEventFilter(this);
        m_catching = false;
    }
    if (m_catchTimer)
        m_catchTimer->stop();
    m_catchTarget = KFileItem();
    m_swallowRelease = false;
}

bool PreviewContainer::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_catching)
        return QWidget::eventFilter(watched, event);
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonDblClick
        && !(m_swallowRelease && type == QEvent::MouseButtonRelease))
        return QWidget::eventFilter(watched, event);
    auto *w = qobject_cast<QWidget *>(watched);
    if (!w || (w != this && !isAncestorOf(w)))
        return QWidget::eventFilter(watched, event);

    if (type == QEvent::MouseButtonRelease) {
        stopDoubleClickCatch();
        return true;
    }
    if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
        return QWidget::eventFilter(watched, event);
    // The second click of a double-click on the row that woke the snapshot.
    const KFileItem target = std::exchange(m_catchTarget, KFileItem());
    m_swallowRelease = true;
    if (m_live && !target.isNull()) {
        if (target.isDir())
            m_pendingChild.clear(); // entering it: the child goes with the selection
        m_live->openItem(target); // a folder is entered, a file opened
    }
    return true;
}

void PreviewContainer::dropLive()
{
    if (!m_live)
        return;
    m_childDebounce->stop();
    stopDoubleClickCatch();
    m_pendingChild.clear();
    disconnect(m_live, nullptr, this, nullptr);
    disconnect(m_live, nullptr, m_childDebounce, nullptr);
    m_stack->removeWidget(m_live);
    m_live->hide();
    // Later: this may run from inside one of the panel's own signals.
    m_live->deleteLater();
    m_live = nullptr;
    m_pages[Live] = nullptr;
    updateHeader();
    Q_EMIT liveNavigationChanged();
}

void PreviewContainer::syncChild()
{
    if (!m_live)
        return;
    int count = 0;
    const KFileItem selected = m_live->soleSelectedItem(count);

    // Still loading: the clicked item is shown below, not yet selected.
    if (count == 0 && m_pendingChild.isValid())
        return;

    // Same item still selected: a directory reload echoes a selection change
    // without the selection having moved. Keep the child as it is.
    if (m_child && count == 1 && !m_child->item().isNull()
        && m_child->item().url() == selected.url())
        return;
    // Anything below replaces the child's documents. Cancel keeps them on
    // screen; the new selection is not followed until the user decides.
    if (m_child && !m_child->confirmDiscard())
        return;

    if (count == 0) {
        closeChild(false);
        return;
    }
    m_pendingChild.clear(); // the selection speaks for itself from here
    PreviewContainer *child = ensureChild();
    if (count > 1)
        child->showMessage(i18np("%1 item selected", "%1 items selected", count));
    else
        child->showItem(selected, m_live->model() && m_live->model()->showHidden());
}

PreviewContainer *PreviewContainer::ensureChild()
{
    if (m_child)
        return m_child;
    m_child = new PreviewContainer(m_depth + 1, m_mainPanel, this);
    m_child->setSource(m_live);
    connect(m_child, &PreviewContainer::revealRequested,
            this, &PreviewContainer::revealRequested);
    connect(m_child, &PreviewContainer::previewToggleRequested,
            this, &PreviewContainer::previewToggleRequested);
    connect(m_child, &PreviewContainer::liveNavigationChanged,
            this, &PreviewContainer::liveNavigationChanged);
    connect(m_child, &PreviewContainer::closeRequested, this, [this]() {
        if (!closeChild(true))
            return;
        // What opened it was the selection; without this the next selection
        // change would bring it straight back.
        if (m_live) {
            m_live->clearSelection();
            m_live->focusView();
        }
    });
    m_layout->addWidget(m_child, 1);
    m_child->show();
    m_layout->setStretchFactor(m_stack, 0);
    setBodyHeight(m_bodyHeight > 0 ? m_bodyHeight : int(viewportHeight() * kSplitShare));
    m_handle->show();

    // Bring the new level into view once the column has laid it out.
    QPointer<PreviewContainer> child = m_child;
    QTimer::singleShot(0, this, [this, child]() {
        if (!child)
            return;
        for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
            if (auto *scroll = qobject_cast<QScrollArea *>(w)) {
                scroll->ensureWidgetVisible(child->m_linkRow, 0, 0);
                break;
            }
        }
    });
    return m_child;
}

bool PreviewContainer::closeChild(bool ask)
{
    if (!m_child)
        return true;
    if (ask && !m_child->confirmDiscard())
        return false;
    // Later, not now: this may run inside the child's own key or click
    // handler (Esc, the close button).
    m_child->hide();
    m_child->deleteLater();
    m_child = nullptr;
    m_handle->hide();
    // Back to the deepest level: the page fills what is left again.
    m_stack->setMinimumHeight(kLeafMinHeight);
    m_stack->setMaximumHeight(QWIDGETSIZE_MAX);
    m_layout->setStretchFactor(m_stack, 1);
    return true;
}

void PreviewContainer::setBodyHeight(int h)
{
    m_bodyHeight = qMax(kMinBodyHeight, h);
    m_stack->setFixedHeight(m_bodyHeight);
}

int PreviewContainer::viewportHeight() const
{
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
        if (auto *scroll = qobject_cast<QAbstractScrollArea *>(w))
            return scroll->viewport()->height();
    }
    return height();
}

QList<Panel *> PreviewContainer::livePanels() const
{
    QList<Panel *> out;
    if (m_live)
        out << m_live;
    if (m_child)
        out << m_child->livePanels();
    return out;
}

// ── Classification ───────────────────────────────────────────────────────────

void PreviewContainer::primeClassification()
{
    pageForMime(QStringLiteral("application/octet-stream"));
}

// Pure function of the type name — no file access — so it is safe on the
// worker that classifies ambiguous files as well as on the GUI thread.
PreviewContainer::Page PreviewContainer::pageForMime(const QString &mimeName)
{
    // PDF before image: a Qt image plugin (poppler/kimageformats) can
    // register application/pdf as a readable image, which would turn the
    // document into a single rasterised first page.
    if (mimeName == QLatin1String("application/pdf"))
        return Pdf;

    static const QSet<QByteArray> supportedImages = [] {
        const QList<QByteArray> list = QImageReader::supportedMimeTypes();
        return QSet<QByteArray>(list.cbegin(), list.cend());
    }();
    if (supportedImages.contains(mimeName.toLatin1()))
        return Image;

    // shared-mime-info makes JSON, JavaScript, PHP, XML, shell scripts, .desktop
    // (.directory), TOML, YAML and friends all inherit text/plain, so one
    // check covers the whole list without an extension table to maintain.
    static QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForName(mimeName);
    if (mime.isValid() && (mime.inherits(QStringLiteral("text/plain"))
                           || mime.name().startsWith(QLatin1String("text/")))) {
        return Text;
    }
    return Generic;
}

// ── Unsaved edits ────────────────────────────────────────────────────────────

bool PreviewContainer::ownUnsavedChanges() const
{
    auto *text = static_cast<TextPreview *>(m_pages[Text]);
    return text && m_stack->currentWidget() == text && text->isModified();
}

bool PreviewContainer::hasUnsavedChanges() const
{
    return ownUnsavedChanges() || (m_child && m_child->hasUnsavedChanges());
}

bool PreviewContainer::confirmDiscard()
{
    // Deepest first. A Cancel further up leaves an already answered child
    // as it was; a Discard there only takes effect when it is replaced.
    if (m_child && !m_child->confirmDiscard())
        return false;
    if (!ownUnsavedChanges())
        return true;
    QMessageBox box(QMessageBox::Question, i18n("Unsaved Changes"),
                    i18n("\"%1\" has unsaved changes.", m_item.name()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, window());
    box.setDefaultButton(QMessageBox::Save);
    prepareMessageBox(box);
    const auto r = static_cast<QMessageBox::StandardButton>(box.exec());
    if (r == QMessageBox::Cancel)
        return false;
    if (r == QMessageBox::Save)
        return static_cast<TextPreview *>(m_pages[Text])->save();
    return true;
}

void PreviewContainer::resolveUnsavedChanges()
{
    if (m_child)
        m_child->resolveUnsavedChanges();
    if (!ownUnsavedChanges())
        return;
    QMessageBox box(QMessageBox::Question, i18n("Unsaved Changes"),
                    i18n("\"%1\" has unsaved changes and its folder is closing.", m_item.name()),
                    QMessageBox::Save | QMessageBox::Discard, window());
    box.setDefaultButton(QMessageBox::Save);
    prepareMessageBox(box);
    const auto r = static_cast<QMessageBox::StandardButton>(box.exec());
    if (r == QMessageBox::Save)
        static_cast<TextPreview *>(m_pages[Text])->save();
}

// ── Events ───────────────────────────────────────────────────────────────────

void PreviewContainer::contextMenuEvent(QContextMenuEvent *event)
{
    // A right-click anywhere on a file's preview that nothing inside took
    // (the image, the PDF pages, the card, the header): the file's own menu,
    // as if right-clicked in the view it was selected in. Folders have their
    // view's menus; anything else goes on up.
    if (!m_item.isNull() && !m_item.isDir() && m_source) {
        event->accept();
        m_source->showItemContextMenu(m_item, event->globalPos());
        return;
    }
    event->ignore();
}

void PreviewContainer::keyPressEvent(QKeyEvent *event)
{
    // Esc bubbles up from the focused widget; the first container it reaches
    // is the deepest one holding focus. The root leaves it to the window.
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier && m_depth > 0) {
        event->accept();
        Q_EMIT closeRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PreviewContainer::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    if (m_depth == 0)
        return;
    // The nesting line, in the indent.
    QPainter p(this);
    p.setPen(QPen(palette().color(QPalette::Mid), 2));
    p.drawLine(kIndent / 2, 0, kIndent / 2, height());
}

void PreviewContainer::applyEditIcons()
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::Window);
    const qreal dpr = devicePixelRatioF();
    m_discardBtn->setIcon(tintedIcon(QStringLiteral("document-revert"),
                                     scheme.foreground(KColorScheme::NegativeText).color(),
                                     m_discardBtn->iconSize(), dpr));
    m_saveBtn->setIcon(tintedIcon(QStringLiteral("document-save"),
                                  scheme.foreground(KColorScheme::LinkText).color(),
                                  m_saveBtn->iconSize(), dpr));
}

void PreviewContainer::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::StyleChange
        || event->type() == QEvent::DevicePixelRatioChange)
        applyEditIcons();
}
