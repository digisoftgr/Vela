#pragma once

#include <QRegularExpression>
#include <QSet>
#include <QStyledItemDelegate>
#include <QUrl>

// Item delegate that draws a yellow highlight over the portion of the Name
// column that matches the current filter/search term.
class MatchHighlightDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit MatchHighlightDelegate(QObject *parent = nullptr);

    void setHighlightText(const QString &text, bool caseSensitive = false);
    void clearHighlight();

    void setCutUrls(const QSet<QUrl> &urls);
    void clearCutUrls();

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void tabToNext(); // Tab pressed in editor: committed, move to next item
    void tabToPrev(); // Shift+Tab pressed in editor: committed, move to previous item

private:
    static QUrl urlAt(const QModelIndex &index);

    QString    m_highlightText;
    bool       m_caseSensitive = false;
    // Compiled once per term, not per painted cell. Invalid (empty pattern)
    // when the term is a subsequence query rather than a glob.
    QRegularExpression m_globRe;
    QSet<QUrl> m_cutUrls;
};
