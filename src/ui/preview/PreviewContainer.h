#pragma once

#include <KFileItem>
#include <QPointer>
#include <QWidget>

#include <functional>

class ItemLink;
class FolderPreview;
class ImagePreview;
class Panel;
class PdfPreview;
class QLabel;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;
class SectionHandle;
class TextPreview;

/**
 * One level of the preview column: the sole selected item of a source panel
 * — a folder, a scaled image, a PDF, an editable text document, or an icon +
 * metadata card — with a header above it.
 *
 * The source is the panel the window's preview follows, for the root; for a
 * sub container it is the folder view of the container above. Everything the
 * header and the right-click do to the shown file goes through the source,
 * as if done in that view: the file's button opens it like a double-click,
 * and a right-click anywhere on the preview brings up the same menu. In the
 * text editor that menu is a submenu of the editor's own.
 *
 * A folder starts as a snapshot (FolderPreview: capped, unwatched, cheap).
 * The first time the user reaches for it — focus, a click, a double-click, a
 * right-click — it is replaced by a full file view: an embedded Panel, with
 * everything a tab has, and the gesture is carried out there. It stays live
 * until the container shows something else. A live folder's selection drives
 * a child container nested below it, to any depth: one selected item is
 * previewed there, several get a count, none closes it. The column scrolls
 * as a whole; a drag handle between a page and its child sets how tall the
 * page is, and the deepest container fills whatever height is left.
 *
 * Header: for a folder, the folder's button, which moves the panel the window follows into that folder, selecting the entry
 * open below (revealRequested). For a file, the file's button. Sub
 * containers also get a close button, and close on Esc while focus is inside
 * them; closing clears the selection that opened them.
 *
 * Pages are created on first use, so a deep chain of folders does not carry
 * an idle PDF viewer and editor per level.
 *
 * Every container may hold unsaved text edits. The unsaved-edit calls cover
 * this container and all of its descendants, deepest first, and anything
 * that replaces or drops a child asks about that child's subtree only.
 * showItem() and showMessage() drop the child without asking: callers
 * resolve edits first, as with the document itself.
 */
class PreviewContainer : public QWidget
{
    Q_OBJECT

public:
    // `depth` 0 is the root, owned by PreviewPane; children are created here.
    // `mainPanel` resolves the panel the window's preview follows: new tabs
    // opened from any level's folder view go there.
    PreviewContainer(int depth, std::function<Panel *()> mainPanel, QWidget *parent = nullptr);
    ~PreviewContainer() override;

    // The panel whose selection this container shows.
    void setSource(Panel *source);

    void showItem(const KFileItem &item, bool showHidden);
    void showMessage(const QString &text);
    // What is currently shown (null for messages).
    KFileItem item() const { return m_item; }

    bool hasUnsavedChanges() const;
    // Save / Discard / Cancel for every dirty document in this subtree. True
    // when the caller may replace or drop it.
    bool confirmDiscard();
    // Save / Discard only, for when the subtree goes away regardless.
    void resolveUnsavedChanges();

    // This container's live folder view and all below it.
    QList<Panel *> livePanels() const;

    // Builds the image-format table pageForMime relies on. Call once on the
    // GUI thread before any classification worker could touch it first.
    static void primeClassification();

Q_SIGNALS:
    // The panel the window follows should enter `folder` and select `item`
    // (may be empty). Bubbles up from descendants unchanged.
    void revealRequested(const QUrl &folder, const QUrl &item);
    // Space in a live folder view: toggle the preview column, as in a tab.
    // Bubbles up unchanged.
    void previewToggleRequested();
    // A live folder view here or below navigated, or appeared / went away:
    // the window's Back / Up state may have changed. Bubbles up unchanged.
    void liveNavigationChanged();
    // A sub container's close button or Esc; the parent decides.
    void closeRequested();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    enum Page { Message = 0, Folder, Live, Image, Pdf, Text, Generic, PageCount };

    // Tints the Save/Discard icons from the colour scheme. Re-run on palette
    // and scale changes so a theme switch or HiDPI move stays correct.
    void applyEditIcons();
    void updateHeader();

    // The page widgets, created on first use (Live: see goLive).
    FolderPreview *folderPage();
    ImagePreview  *imagePage();
    PdfPreview    *pdfPage();
    TextPreview   *textPage();
    QWidget       *genericPage();
    // Clears every existing page but `keep`, so only the page being shown
    // holds content — a large image or document does not sit in memory
    // behind a folder listing. Leaving Live drops the folder view.
    void clearPagesExcept(Page keep);
    void setCurrentPage(Page page);

    // Shows a file on the page `page` (Pdf / Image / Text / Generic), with
    // `mimeName` as its already-resolved type.
    void presentFile(const KFileItem &item, Page page, const QString &mimeName);
    void showGeneric(const KFileItem &item, const QString &mimeName,
                     const QString &note = QString());
    // Which page a MIME type belongs on, from the type alone (no file access).
    static Page pageForMime(const QString &mimeName);

    // ── Live folder view ─────────────────────────────────────────────────────
    // What the gesture that woke the snapshot asks of the full view.
    enum class Gesture { Focus, Click, Menu };
    // Replaces the snapshot with an embedded Panel on this folder, then
    // carries out `gesture` on `target` (may be null) there. A click also
    // opens `target` below at once, without waiting for the listing.
    void goLive(Gesture gesture, const KFileItem &target, const QPoint &globalPos = {});
    void dropLive();
    // After a click woke the snapshot, the second click of a double-click
    // would land on the new view (or the child's header) — whatever is under
    // the mouse now. For one double-click interval, it is caught here instead
    // and taken as a double-click on `target`.
    void startDoubleClickCatch(const KFileItem &target);
    void stopDoubleClickCatch();
    // Makes the child show the live view's selection.
    void syncChild();
    // Where the folder button leads, and what it selects there (may be empty).
    QUrl folderTarget() const;
    QUrl folderReveal() const;

    PreviewContainer *ensureChild();
    // Drops the child. With `ask`, its unsaved edits get their say first and
    // may cancel; false then.
    bool closeChild(bool ask);
    void setBodyHeight(int h);
    // Height of the scrolling column's viewport, for sizing a new split.
    int viewportHeight() const;

    bool ownUnsavedChanges() const;

    const int m_depth;
    std::function<Panel *()> m_mainPanel;
    QPointer<Panel> m_source;
    KFileItem m_item;
    bool      m_showHidden = false;
    // Bumped whenever the shown item changes, so a content classification
    // finishing on a worker for an item no longer shown is dropped.
    int       m_classifyGeneration = 0;

    QVBoxLayout *m_layout      = nullptr;
    QWidget     *m_linkRow     = nullptr;
    ItemLink    *m_link        = nullptr;
    QToolButton *m_closeBtn    = nullptr;
    QWidget     *m_editRow     = nullptr;
    QToolButton *m_saveBtn     = nullptr;
    QToolButton *m_discardBtn  = nullptr;
    QToolButton *m_wrapBtn     = nullptr;
    QStackedWidget *m_stack    = nullptr;
    QWidget     *m_pages[PageCount] = {};
    QLabel      *m_message     = nullptr;
    QLabel      *m_genericIcon = nullptr;
    QLabel      *m_genericText = nullptr;
    QString      m_fileIcon;   // icon name for the file button

    Panel *m_live = nullptr;   // the Live page while there is one
    QTimer *m_childDebounce = nullptr;
    // The clicked item, shown below before the listing could select it.
    // While set, the live view's "nothing selected" (it is still loading)
    // does not close the child.
    QUrl m_pendingChild;
    KFileItem m_catchTarget;
    QTimer   *m_catchTimer = nullptr;
    bool      m_catching = false;
    bool      m_swallowRelease = false; // the release ending a caught double-click

    SectionHandle    *m_handle = nullptr;
    PreviewContainer *m_child  = nullptr;
    int m_bodyHeight = -1; // page height while a child is open; -1 = not yet set
};
