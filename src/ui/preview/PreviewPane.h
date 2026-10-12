#pragma once

#include <QPointer>
#include <QWidget>

class Panel;
class PreviewContainer;
class QScrollArea;
class QTimer;

/**
 * The window's preview column. One per MainWindow, shown to the right of the
 * whole panel area and toggled with Space.
 *
 * It follows one Panel at a time — whichever the window last marked active —
 * and shows that panel's sole selected item in its root PreviewContainer.
 * Zero or several selected items get a plain message instead. A folder's
 * entries open in nested containers below it (see PreviewContainer); the
 * column scrolls as a whole.
 *
 * Any container may hold unsaved text edits. Every path that would replace
 * them (selection change, tab switch, closing the pane, closing the window)
 * goes through confirmDiscard(), which asks Save / Discard / Cancel. On
 * Cancel the pane simply keeps showing the dirty documents, ignoring the
 * panel until the user resolves it.
 */
class PreviewPane : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewPane(QWidget *parent = nullptr);
    ~PreviewPane() override;

    // Follow `panel` (nullptr detaches). Re-reads its selection immediately.
    void setPanel(Panel *panel);
    Panel *panel() const { return m_panel; }

    bool hasUnsavedChanges() const;
    // Save / Discard / Cancel. True when the caller may proceed to replace or
    // drop the document (it was saved, discarded, or was never dirty).
    bool confirmDiscard();
    // Like confirmDiscard() but without a Cancel button, for moments where
    // the document is going away regardless (the panel it came from closed).
    void resolveUnsavedChanges();

    // The last width the column had while visible, for session persistence.
    // Falls back to `fallback` when it has never been shown.
    int rememberedWidth(int fallback) const;
    void setRememberedWidth(int w) { m_rememberedWidth = w; }

    // The preview's live folder view holding keyboard focus, if any: the
    // window's folder actions (Back, F5, New Folder, Ctrl+H, …) act on it.
    Panel *focusedPanel() const;
    // Every live folder view in the column, for settings changes.
    QList<Panel *> livePanels() const;

Q_SIGNALS:
    // Space in one of the preview's folder views.
    void previewToggleRequested();
    // A live folder view navigated, appeared or went away.
    void liveNavigationChanged();

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void connectPanel(Panel *panel);
    void disconnectPanel();
    // Re-reads the followed panel's selection and swaps the shown item.
    void refresh();

    QPointer<Panel>   m_panel;
    QTimer           *m_refreshDebounce = nullptr;
    QScrollArea      *m_scroll = nullptr;
    PreviewContainer *m_root   = nullptr;

    int m_rememberedWidth = -1;
};
