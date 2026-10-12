#include "FileFilterProxyModel.h"

#include "FileSystemModel.h"
#include "StringMatch.h"

FileFilterProxyModel::FileFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
}

void FileFilterProxyModel::setFilterPattern(const QString &pattern)
{
    if (m_pattern == pattern)
        return;
    m_pattern = pattern;
    m_patternLower = pattern.toLower();

    // Pre-compile a glob regex only when the pattern contains wildcard chars.
    // Plain text uses subsequence matching at filter time (no regex needed).
    if (pattern.contains(QLatin1Char('*')) || pattern.contains(QLatin1Char('?'))) {
        m_isGlob = true;
        m_regex = QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(
                pattern, QRegularExpression::UnanchoredWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
    } else {
        m_isGlob = false;
        m_regex = QRegularExpression();
    }

    beginFilterChange();
    endFilterChange();
}

void FileFilterProxyModel::setFoldersFirst(bool first)
{
    if (m_foldersFirst == first)
        return;
    m_foldersFirst = first;
    invalidate(); // re-sort
}

void FileFilterProxyModel::setSourceModel(QAbstractItemModel *model)
{
    m_base = qobject_cast<FileItemModelBase *>(model);
    QSortFilterProxyModel::setSourceModel(model);
}

// Fast path over FileItemModelBase: reads the items directly. The generic path
// below made 2–4 data() calls per comparison, each boxing a QVariant and
// copying a string, and collated names from scratch every time — millions of
// each for a 100k-row sort.
bool FileFilterProxyModel::lessThanItems(int lr, int rr, int col) const
{
    const KFileItem &li = m_base->itemAt(lr);
    const KFileItem &ri = m_base->itemAt(rr);

    if (m_foldersFirst && li.isDir() != ri.isDir()) {
        // See lessThan: keeps folders first in both sort orders.
        return sortOrder() == Qt::AscendingOrder ? li.isDir() : !li.isDir();
    }

    switch (col) {
    case FileItemModelBase::Size:
        return m_base->numericSizeAtRow(lr) < m_base->numericSizeAtRow(rr);
    case FileItemModelBase::DateModified:
        return li.time(KFileItem::ModificationTime) < ri.time(KFileItem::ModificationTime);
    case FileItemModelBase::DateCreated:
        return li.time(KFileItem::CreationTime) < ri.time(KFileItem::CreationTime);
    case FileItemModelBase::DateAccessed:
        return li.time(KFileItem::AccessTime) < ri.time(KFileItem::AccessTime);
    case FileItemModelBase::Name: {
        const int c = m_base->nameSortKey(lr).compare(m_base->nameSortKey(rr));
        if (c != 0)
            return c < 0;
        return QString::compare(li.name(), ri.name(), Qt::CaseSensitive) < 0;
    }
    case FileItemModelBase::Type: {
        // Cached collation keys (one per distinct type text), not a
        // collator.compare per comparison.
        const int c = m_base->typeSortKey(lr).compare(m_base->typeSortKey(rr));
        if (c != 0)
            return c < 0;
        // Same type: keep the order stable and useful — by name.
        return m_base->nameSortKey(lr).compare(m_base->nameSortKey(rr)) < 0;
    }
    default:
        break;
    }
    return lessThanGeneric(m_base->index(lr, col), m_base->index(rr, col), /*dirsDone=*/true);
}

bool FileFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (m_base && sortRole() == Qt::DisplayRole)
        return lessThanItems(left.row(), right.row(), left.column());
    return lessThanGeneric(left, right, /*dirsDone=*/false);
}

bool FileFilterProxyModel::lessThanGeneric(const QModelIndex &left, const QModelIndex &right,
                                           bool dirsDone) const
{
    if (m_foldersFirst && !dirsDone) {
        const QModelIndex l0 = left.siblingAtColumn(0);
        const QModelIndex r0 = right.siblingAtColumn(0);
        const bool lDir = sourceModel()->data(l0, FileSystemModel::IsDirRole).toBool();
        const bool rDir = sourceModel()->data(r0, FileSystemModel::IsDirRole).toBool();
        if (lDir != rDir) {
            // Qt calls lessThan(right, left) for descending sort, so the arguments
            // are already swapped relative to visual order. Using sortOrder() lets us
            // always return the result that places dirs before files visually.
            return sortOrder() == Qt::AscendingOrder ? lDir : !lDir;
        }
    }

    // Sort the Size column by raw byte count so "900 MB" correctly beats "1 KB".
    // Dirs without a calculated size get -1 and sort first (unknown < any known size).
    if (left.column() == FileSystemModel::Size) {
        const qint64 ls = sourceModel()->data(left,  FileSystemModel::NumericSizeRole).toLongLong();
        const qint64 rs = sourceModel()->data(right, FileSystemModel::NumericSizeRole).toLongLong();
        return ls < rs;
    }

    // Sort date columns by actual QDateTime so chronological order is always correct.
    // Comparing the display string (DD/MM/YYYY) gives wrong results: "29/04" > "16/06"
    // lexicographically even though April precedes June.
    const int col = left.column();
    if (col == FileSystemModel::DateModified ||
        col == FileSystemModel::DateCreated  ||
        col == FileSystemModel::DateAccessed) {
        const QDateTime ld = sourceModel()->data(left,  FileSystemModel::DateTimeRole).toDateTime();
        const QDateTime rd = sourceModel()->data(right, FileSystemModel::DateTimeRole).toDateTime();
        return ld < rd;
    }

    // Text columns: natural, case-insensitive order (matches FolderPreview).
    // Names equal ignoring case fall back to a code-point compare so the order is deterministic.
    const QVariant lv = sourceModel()->data(left,  sortRole());
    const QVariant rv = sourceModel()->data(right, sortRole());
    if (lv.userType() == QMetaType::QString && rv.userType() == QMetaType::QString) {
        const QString ls = lv.toString();
        const QString rs = rv.toString();
        const int c = m_collator.compare(ls, rs);
        if (c != 0)
            return c < 0;
        return QString::compare(ls, rs, Qt::CaseSensitive) < 0;
    }

    return QSortFilterProxyModel::lessThan(left, right);
}

bool FileFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_pattern.isEmpty())
        return true;

    // Our own models: the name straight off the item. data() boxed a QString
    // copy in a QVariant for every row on every keystroke.
    QString name;
    if (m_base) {
        name = m_base->itemAt(sourceRow).name();
    } else {
        const QModelIndex nameIdx = sourceModel()->index(sourceRow, 0, sourceParent);
        name = sourceModel()->data(nameIdx, Qt::DisplayRole).toString();
    }

    if (m_isGlob)
        return m_regex.match(name).hasMatch();

    return subsequenceMatchLowered(m_patternLower, name);
}
