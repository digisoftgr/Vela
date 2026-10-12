#include "MatchHighlightDelegate.h"

#include "core/FileSystemModel.h"
#include "core/StringMatch.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>

#include <limits>

MatchHighlightDelegate::MatchHighlightDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void MatchHighlightDelegate::setHighlightText(const QString &text, bool caseSensitive)
{
    m_highlightText = text;
    m_caseSensitive = caseSensitive;
    m_globRe = isGlobPattern(text)
                   ? globHighlightRegex(text, caseSensitive ? Qt::CaseSensitive
                                                            : Qt::CaseInsensitive)
                   : QRegularExpression();
}

void MatchHighlightDelegate::clearHighlight()
{
    m_highlightText.clear();
    m_globRe = QRegularExpression();
}

void MatchHighlightDelegate::setCutUrls(const QSet<QUrl> &urls)
{
    m_cutUrls = urls;
}

void MatchHighlightDelegate::clearCutUrls()
{
    m_cutUrls.clear();
}

// The row's URL without data()/QVariant when the view sits on one of our
// proxies over one of our models (always, in practice): this runs for every
// painted cell while anything is cut.
QUrl MatchHighlightDelegate::urlAt(const QModelIndex &index)
{
    if (const auto *proxy = qobject_cast<const QSortFilterProxyModel *>(index.model())) {
        if (const auto *base = qobject_cast<const FileItemModelBase *>(proxy->sourceModel())) {
            const QModelIndex src = proxy->mapToSource(index);
            if (src.isValid())
                return base->itemAt(src.row()).url();
        }
    }
    return index.data(FileSystemModel::UrlRole).toUrl();
}

void MatchHighlightDelegate::paint(QPainter *painter,
                                    const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
{
    const bool isCut = !m_cutUrls.isEmpty() && m_cutUrls.contains(urlAt(index));

    if (isCut)
        painter->setOpacity(0.4);

    if (m_highlightText.isEmpty() || index.column() != FileSystemModel::Name) {
        QStyledItemDelegate::paint(painter, option, index);
        if (isCut) painter->setOpacity(1.0);
        return;
    }

    const QString displayText = index.data(Qt::DisplayRole).toString();
    const Qt::CaseSensitivity cs = m_caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;

    // Globs match through a regex (a subsequence match on "*.txt" looks for a
    // literal '*' and never matches); plain terms are subsequence queries.
    const QList<int> positions = m_globRe.pattern().isEmpty()
                                     ? matchedPositions(m_highlightText, displayText, cs)
                                     : globMatchedPositions(m_globRe, displayText);
    if (positions.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        // Must restore here too: this early return used to leak the cut-item
        // opacity into every row painted after it.
        if (isCut) painter->setOpacity(1.0);
        return;
    }

    // Draw the item's background, selection state and icon — but not the Name
    // text, which we render ourselves below in a single pass together with the
    // match highlighting, so glyphs are shaped/hinted once instead of twice.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();

    // Ask for the text rect while opt.text is still set: in icon mode the style
    // sizes that rect from the text itself, so with the text cleared it came
    // back empty or wrong and the name was not drawn where the view expects it.
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);

    // Draw the item's background, selection state and icon — but not the Name
    // text, which we render ourselves below in a single pass together with the
    // match highlighting, so glyphs are shaped/hinted once instead of twice.
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    if (textRect.isEmpty()) {
        if (isCut) painter->setOpacity(1.0);
        return;
    }

    // Same horizontal inset QCommonStyle applies around item text.
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, opt.widget) + 1;
    const QRectF area = QRectF(textRect).adjusted(margin, 0, -margin, 0);
    const qreal width = qMax<qreal>(area.width(), 1.0);
    const bool wrap = opt.features & QStyleOptionViewItem::WrapText;
    const Qt::Alignment align = QStyle::visualAlignment(opt.direction, opt.displayAlignment);

    // Follow the view's own text layout instead of one unwrapped, left-aligned
    // line: wrapped and centred in the icon modes, single-line in details, and
    // elided when it does not fit — as QStyledItemDelegate would draw it.
    QTextOption textOption;
    textOption.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                : QTextOption::NoWrap);
    textOption.setAlignment(align & Qt::AlignHorizontal_Mask);
    textOption.setTextDirection(opt.direction);

    // Lays out at most maxLines lines and returns them; line breaks for a given
    // prefix do not depend on what follows it, so re-laying an elided string
    // reproduces the same leading lines.
    const auto layoutLines = [&](QTextLayout &layout, int maxLines) {
        QList<QTextLine> lines;
        layout.setTextOption(textOption);
        layout.beginLayout();
        for (int i = 0; i < maxLines; ++i) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width);
            lines.append(line);
        }
        layout.endLayout();
        return lines;
    };

    // Pass 1: how many lines fit, and does the text overflow them?
    QString text = displayText;
    int keepUpTo = displayText.size(); // highlight positions below this survive eliding
    int lineCount = 1;
    {
        QTextLayout probe(displayText, opt.font);
        const QList<QTextLine> all = layoutLines(probe, wrap ? std::numeric_limits<int>::max() : 1);
        qreal used = 0;
        lineCount = 0;
        for (const QTextLine &l : all) {
            if (lineCount > 0 && used + l.height() > area.height())
                break;
            used += l.height();
            ++lineCount;
        }
        lineCount = qMax(lineCount, 1);
        const QTextLine last = all.at(lineCount - 1);
        const bool overflow = lineCount < all.size() || last.naturalTextWidth() > width;
        if (overflow && opt.textElideMode != Qt::ElideNone) {
            // Elide only the last visible line. ElideRight keeps the leading
            // characters in place, so highlight positions map straight across.
            const int lastStart = last.textStart();
            const QString elided = QFontMetricsF(opt.font).elidedText(
                displayText.mid(lastStart), Qt::ElideRight, width);
            const bool hasEllipsis = elided.endsWith(QChar(0x2026));
            text = displayText.left(lastStart) + elided;
            keepUpTo = lastStart + int(elided.size()) - (hasEllipsis ? 1 : 0);
        }
    }

    const QColor highlightColor(255, 185, 0, 255); // amber, opaque

    // Build one text layout for the whole name and mark the matched characters
    // with a background/foreground format, then draw it in a single call. Doing
    // the highlight and the glyphs in one draw (instead of drawing the string,
    // then separately re-drawing individual matched letters on top) guarantees
    // they're pixel-identical — no risk of the second draw landing on slightly
    // different pixels due to per-glyph hinting depending on neighboring glyphs.
    QTextLayout textLayout(text, opt.font);

    QList<QTextLayout::FormatRange> formats;
    formats.reserve(positions.size());
    for (int pos : positions) {
        if (pos >= keepUpTo)
            continue; // elided away
        QTextCharFormat charFormat;
        charFormat.setBackground(highlightColor);
        charFormat.setForeground(Qt::black);
        formats.append({pos, 1, charFormat});
    }
    textLayout.setFormats(formats);

    const QList<QTextLine> lines = layoutLines(textLayout, lineCount);
    qreal totalHeight = 0;
    for (QTextLine line : lines) {
        line.setPosition(QPointF(0, totalHeight));
        totalHeight += line.height();
    }

    qreal y = area.top();
    if (align & Qt::AlignBottom)
        y = area.bottom() - totalHeight;
    else if (!(align & Qt::AlignTop))
        y = area.top() + (area.height() - totalHeight) / 2.0;

    painter->save();
    painter->setClipRect(option.rect);
    painter->setPen(opt.state & QStyle::State_Selected
                     ? opt.palette.color(QPalette::HighlightedText)
                     : opt.palette.color(QPalette::Text));
    textLayout.draw(painter, QPointF(area.left(), y));
    painter->restore();

    if (isCut) painter->setOpacity(1.0);
}

bool MatchHighlightDelegate::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *editor = qobject_cast<QWidget *>(watched);
        const auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Base class commits + closes; we consume so the view never sees it.
            QStyledItemDelegate::eventFilter(watched, event);
            return true;
        case Qt::Key_Tab:
            // Do NOT call the base class — it emits EditNextItem which moves the
            // view one row before our signal fires, causing a double-skip.
            // Commit + close with NoHint so the view stays put, then signal Panel.
            if (editor) {
                Q_EMIT commitData(editor);
                Q_EMIT closeEditor(editor, QAbstractItemDelegate::NoHint);
            }
            Q_EMIT tabToNext();
            return true;
        case Qt::Key_Backtab:
            if (editor) {
                Q_EMIT commitData(editor);
                Q_EMIT closeEditor(editor, QAbstractItemDelegate::NoHint);
            }
            Q_EMIT tabToPrev();
            return true;
        default:
            break;
        }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

void MatchHighlightDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    auto *lineEdit = qobject_cast<QLineEdit *>(editor);
    if (!lineEdit) {
        QStyledItemDelegate::setEditorData(editor, index);
        return;
    }

    // EditRole now returns item.name() (full name, extension always included)
    const QString name = index.data(Qt::EditRole).toString();
    lineEdit->setText(name);

    const bool isDir = index.data(FileSystemModel::IsDirRole).toBool();
    const int dotPos = isDir ? -1 : name.lastIndexOf(QLatin1Char('.'));

    // QAbstractItemView queues setFocus, so selection set right now loses the race.
    // Defer one event-loop tick to guarantee we run after focus is granted.
    QTimer::singleShot(0, lineEdit, [lineEdit, dotPos]() {
        if (dotPos > 0)
            lineEdit->setSelection(0, dotPos);
        else
            lineEdit->selectAll();
    });
}
