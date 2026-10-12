#include "FileItemModelBase.h"

#include "ThumbnailLoader.h"
#include "VelaSettings.h"

#include <KIO/Global>
#include <KLocalizedString>
#include <KUser>
#include <KIO/UDSEntry>
#include <QIcon>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSet>

#include <algorithm>
#include <functional>

// ── Process-wide caches ───────────────────────────────────────────────────────
// All of these are read from data(), which runs on the GUI thread only, so no
// locking is needed. They exist because data() is called once per visible cell
// per repaint — anything non-trivial in there shows up as scroll stutter.

namespace {

// Theme icon name → QIcon. QIcon::fromTheme is not free (theme lookup plus
// engine construction), and KFileItem::iconName() resolves the MIME type.
QHash<QString, QIcon> s_iconCache;

// MIME type name → human-readable comment. Keyed by MIME type rather than URL,
// so this stays at a few dozen entries instead of one per file in the listing.
QHash<QString, QString> s_mimeCommentCache;

// Date format string, refreshed when the setting changes rather than rebuilt
// per cell.
QString s_dateFormat;
bool    s_dateFormatValid = false;

const QString &dateFormat()
{
    if (!s_dateFormatValid) {
        s_dateFormat = VelaSettings::self()->dateFormat() == 1
                           ? QStringLiteral("MM/dd/yyyy HH:mm")
                           : QStringLiteral("dd/MM/yyyy HH:mm");
        s_dateFormatValid = true;
    }
    return s_dateFormat;
}

QString formatFileTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return {};
    return dt.toString(dateFormat());
}

QIcon cachedIcon(const KFileItem &item)
{
    const QString name = item.iconName();
    auto it = s_iconCache.constFind(name);
    if (it == s_iconCache.constEnd())
        it = s_iconCache.insert(name, QIcon::fromTheme(name));
    return it.value();
}

// Human-readable comment for a MIME type name, cached by type (a few dozen
// entries) rather than by URL (one per file).
QString commentForMimeName(const QString &mimeName)
{
    auto it = s_mimeCommentCache.constFind(mimeName);
    if (it == s_mimeCommentCache.constEnd()) {
        static QMimeDatabase db;
        it = s_mimeCommentCache.insert(mimeName, db.mimeTypeForName(mimeName).comment());
    }
    return it.value();
}

// Owner column. When the worker sent no name (UDS_USER/UDS_GROUP),
// KFileItem::user() resolves the uid with KUser on every call — a passwd/NSS
// lookup per painted cell, which over LDAP/SSSD is a network round trip.
// Cached by id for the process; a renamed account shows its old name until
// restart, which is the accepted trade.
QHash<int, QString> s_userNames;
QHash<int, QString> s_groupNames;

QString ownerName(const KFileItem &item)
{
    if (item.entry().contains(KIO::UDSEntry::UDS_USER))
        return item.user(); // already a string in the entry: no lookup
    const int uid = item.userId();
    if (uid < 0)
        return item.user();
    auto it = s_userNames.constFind(uid);
    if (it == s_userNames.constEnd())
        it = s_userNames.insert(uid, KUser(KUserId(uid)).loginName());
    return it.value();
}

QString groupName(const KFileItem &item)
{
    if (item.entry().contains(KIO::UDSEntry::UDS_GROUP))
        return item.group();
    const int gid = item.groupId();
    if (gid < 0)
        return item.group();
    auto it = s_groupNames.constFind(gid);
    if (it == s_groupNames.constEnd())
        it = s_groupNames.insert(gid, KUserGroup(KGroupId(gid)).name());
    return it.value();
}

} // namespace

void FileItemModelBase::clearIconCache()
{
    s_iconCache.clear();
}

void FileItemModelBase::refreshDateFormatCache()
{
    s_dateFormatValid = false;
}

// ── Construction ──────────────────────────────────────────────────────────────

FileItemModelBase::FileItemModelBase(QObject *parent)
    : QAbstractItemModel(parent)
    , m_mimeResolver(new MimeTypeResolver(this))
{
    // A late-arriving MIME type repaints exactly one row; the URL index makes
    // finding it O(1).
    connect(m_mimeResolver, &MimeTypeResolver::resolved, this,
            [this](const QUrl &url, const QString &mimeName) {
        // Row check first: a result for a row that has gone is dropped rather
        // than cached (it used to pile up in a URL-keyed map).
        const int row = rowForUrl(url);
        if (row < 0)
            return;
        RowCache &rc = m_rowCache[row];
        rc.typeText  = commentForMimeName(mimeName);
        rc.typeState = RowCache::TypeFinal;
        rc.typeKeyId = -1;
        const QModelIndex idx = createIndex(row, Type);
        Q_EMIT dataChanged(idx, idx, {TypeResolvedRole});
    });
}

// ── Direct row accessors ──────────────────────────────────────────────────────

namespace {
// Natural, case-insensitive — the same order FolderPreview uses.
QCollator &nameCollator()
{
    static QCollator c = [] {
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        return col;
    }();
    return c;
}
} // namespace

qint64 FileItemModelBase::numericSizeAtRow(int row) const
{
    const KFileItem &item = m_items.at(row);
    if (!item.isDir())
        return static_cast<qint64>(item.size());
    const QVariant calculated = folderSizeData(item, NumericSizeRole);
    return calculated.isValid() ? calculated.toLongLong() : qint64(-1);
}

const QCollatorSortKey &FileItemModelBase::nameSortKey(int row) const
{
    RowCache &rc = m_rowCache[row];
    if (!rc.nameKey)
        rc.nameKey.emplace(nameCollator().sortKey(m_items.at(row).name()));
    return *rc.nameKey;
}

QString FileItemModelBase::sortTypeText(int row) const
{
    return typeTextForRow(row, /*requestSniff=*/false);
}

QCollatorSortKey FileItemModelBase::typeSortKey(int row) const
{
    const QString text = typeTextForRow(row, /*requestSniff=*/false);
    RowCache &rc = m_rowCache[row];
    if (rc.typeKeyId >= 0)
        return m_typeKeys.at(rc.typeKeyId);

    auto it = m_typeKeyIds.constFind(text);
    if (it == m_typeKeyIds.constEnd()) {
        it = m_typeKeyIds.insert(text, m_typeKeys.size());
        m_typeKeys.append(nameCollator().sortKey(text));
    }
    // Only a final type is pinned to the row; an unresolved one may still
    // change when its sniff comes back.
    if (rc.typeState == RowCache::TypeFinal)
        rc.typeKeyId = it.value();
    return m_typeKeys.at(it.value());
}

// ── Type column ───────────────────────────────────────────────────────────────

// Never calls KFileItem::mimetype()/determineMimeType(). Those are documented
// as "a possibly slow operation" because they fall back to reading the file's
// contents, and data() runs on the GUI thread for every visible cell.
//
// Three tiers, cheapest first:
//   1. the type is already known — free;
//   2. the name is decisive (the overwhelming majority) — pure string matching,
//      no file access at all;
//   3. genuinely ambiguous (extension-less files) — hand off to a worker and
//      leave the cell blank for the moment it takes to come back.
//
// The result is cached per row: the name-based lookup used to run on every
// call, which a sort by Type turned into O(n log n) lookups.
QString FileItemModelBase::typeTextForRow(int row, bool requestSniff) const
{
    RowCache &rc = m_rowCache[row];
    if (rc.typeState == RowCache::TypeFinal)
        return rc.typeText;
    if (rc.typeState == RowCache::TypePending)
        return {}; // sniff already queued; its result will fill the cache

    const KFileItem &item = m_items.at(row);
    const auto finalText = [&rc](const QString &text) {
        rc.typeText  = text;
        rc.typeState = RowCache::TypeFinal;
        rc.typeKeyId = -1;
        return text;
    };

    // .desktop entries describe themselves, and KFileItem::mimeComment()
    // surfaces that. Safe to call here specifically because the ".desktop"
    // suffix is decisive — the type resolves from the name, so no content sniff
    // is triggered. Symlinks deliberately are *not* special-cased: isLink() is
    // cheap but mimeComment() on one forces determination of the target, and
    // directories like /usr/lib are full of them.
    if (item.name().endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive))
        return finalText(item.mimeComment());

    if (item.isMimeTypeKnown())
        return finalText(commentForMimeName(item.currentMimeType().name()));

    // Name-only matching: guaranteed not to touch the file.
    static QMimeDatabase db;
    const QMimeType byName = db.mimeTypeForFile(item.name(), QMimeDatabase::MatchExtension);
    if (byName.isValid() && !byName.isDefault())
        return finalText(commentForMimeName(byName.name()));

    // Ambiguous — needs a content sniff, which must not happen here. Only a
    // painted cell asks for one; a sort leaves the state Unknown.
    if (requestSniff) {
        rc.typeState = RowCache::TypePending;
        m_mimeResolver->resolve(item.url());
    }
    return {};
}

// ── Thumbnails ────────────────────────────────────────────────────────────────

void FileItemModelBase::setThumbnailLoader(ThumbnailLoader *loader)
{
    if (m_thumbLoader == loader)
        return;
    if (m_thumbLoader)
        disconnect(m_thumbLoader, nullptr, this, nullptr);
    m_thumbLoader = loader;
    if (!loader)
        return;

    // Thumbnails arrive in bursts. Rather than one dataChanged (and one view
    // repaint) per image, collapse the batch into contiguous row ranges — a
    // freshly generated page of thumbnails is usually one or two spans.
    connect(loader, &ThumbnailLoader::thumbnailsReady, this,
            [this](const QList<QUrl> &urls) {
        if (!m_thumbnailsEnabled || urls.isEmpty())
            return;

        QList<int> rows;
        rows.reserve(urls.size());
        for (const QUrl &url : urls) {
            const int row = rowForUrl(url); // O(1) via the URL index
            if (row >= 0)
                rows.append(row);
        }
        if (rows.isEmpty())
            return;

        std::sort(rows.begin(), rows.end());
        int runStart = rows.first();
        int runEnd   = runStart;
        for (int i = 1; i < rows.size(); ++i) {
            if (rows.at(i) == runEnd + 1 || rows.at(i) == runEnd) {
                runEnd = rows.at(i);
                continue;
            }
            Q_EMIT dataChanged(createIndex(runStart, Name), createIndex(runEnd, Name),
                               {Qt::DecorationRole});
            runStart = runEnd = rows.at(i);
        }
        Q_EMIT dataChanged(createIndex(runStart, Name), createIndex(runEnd, Name),
                           {Qt::DecorationRole});
    });
}

void FileItemModelBase::setThumbnailsEnabled(bool enabled)
{
    // Flag only. This used to emit dataChanged across the whole model, which
    // the proxy had to map row by row and the view had to process — all
    // synchronously, on the view-mode switch, before anything could be drawn.
    // The caller repaints the viewport instead: that re-queries only the cells
    // actually on screen and costs nothing on a large directory.
    m_thumbnailsEnabled = enabled;
}

// ── Row index ─────────────────────────────────────────────────────────────────

int FileItemModelBase::rowForUrl(const QUrl &url) const
{
    return m_urlToRow.value(url, -1);
}

void FileItemModelBase::reindexFrom(int firstRow)
{
    for (int i = firstRow; i < m_items.size(); ++i)
        m_urlToRow.insert(m_items.at(i).url(), i);
}

void FileItemModelBase::appendItems(const KFileItemList &items)
{
    if (items.isEmpty())
        return;
    const int first = m_items.size();
    beginInsertRows({}, first, first + items.size() - 1);
    m_items.append(items);
    m_rowCache.resize(m_items.size());
    for (const KFileItem &item : items) {
        if (item.isDir())
            ++m_dirCount;
    }
    m_urlToRow.reserve(m_items.size());
    reindexFrom(first);
    endInsertRows();
}

void FileItemModelBase::replaceItemAt(int row, const KFileItem &item)
{
    if (row < 0 || row >= m_items.size())
        return;
    const QUrl oldUrl = m_items.at(row).url();
    const QUrl newUrl = item.url();
    // One row per URL. Callers must drop a row already holding newUrl first;
    // overwriting its index entry here is what used to strand ghost rows.
    Q_ASSERT(m_urlToRow.value(newUrl, row) == row);
    if (oldUrl != newUrl)
        m_urlToRow.remove(oldUrl);
    m_dirCount += int(item.isDir()) - int(m_items.at(row).isDir());
    m_items[row] = item;
    m_rowCache[row] = RowCache(); // name and type may both have changed
    m_urlToRow.insert(newUrl, row);
}

void FileItemModelBase::removeRowsAt(QList<int> rows)
{
    if (rows.isEmpty())
        return;

    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [this](int r) { return r < 0 || r >= m_items.size(); }),
               rows.end());
    if (rows.isEmpty())
        return;

    // One beginRemoveRows per contiguous run, not per row: each call makes the
    // proxy remap and the vector shift, so deleting 10k of 100k rows one at a
    // time cost 10k of each. Runs are removed bottom-up so the indices of the
    // runs still to go stay valid.
    int runEnd = rows.size() - 1;
    while (runEnd >= 0) {
        int runStart = runEnd;
        while (runStart > 0 && rows.at(runStart - 1) == rows.at(runStart) - 1)
            --runStart;
        const int first = rows.at(runStart);
        const int last  = rows.at(runEnd);

        beginRemoveRows({}, first, last);
        for (int r = first; r <= last; ++r) {
            const KFileItem &item = m_items.at(r);
            m_urlToRow.remove(item.url());
            if (item.isDir())
                --m_dirCount;
        }
        m_items.remove(first, last - first + 1);
        m_rowCache.erase(m_rowCache.begin() + first, m_rowCache.begin() + last + 1);
        endRemoveRows();

        runEnd = runStart - 1;
    }
    // Every row after the lowest removal shifted down; rebuild their indices.
    reindexFrom(rows.constFirst());
}

void FileItemModelBase::clearItems()
{
    // Cancels sniffs still queued for the listing being dropped.
    m_mimeResolver->clear();

    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    m_rowCache.clear();
    m_dirCount = 0;
    m_urlToRow.clear();
    endResetModel();
}

// ── Subclass hooks ────────────────────────────────────────────────────────────

QVariant FileItemModelBase::folderSizeData(const KFileItem &, int) const
{
    return {};
}

void FileItemModelBase::onRowDropped(const QUrl &)
{
}

// ── QAbstractItemModel ────────────────────────────────────────────────────────

QModelIndex FileItemModelBase::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= m_items.size()
        || column < 0 || column >= ColumnCount) {
        return {};
    }
    return createIndex(row, column);
}

QModelIndex FileItemModelBase::parent(const QModelIndex &) const
{
    return {};
}

int FileItemModelBase::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

int FileItemModelBase::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

void FileItemModelBase::invalidateDateColumns()
{
    refreshDateFormatCache();
    if (m_items.isEmpty())
        return;
    Q_EMIT dataChanged(createIndex(0, DateModified),
                       createIndex(m_items.size() - 1, DateAccessed),
                       {Qt::DisplayRole});
}

QVariant FileItemModelBase::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};

    const KFileItem &item = m_items.at(index.row());

    switch (role) {
    case Qt::EditRole:
        // Always the full filename, for inline editing.
        return index.column() == Name ? QVariant(item.name()) : QVariant();

    case Qt::DisplayRole:
        switch (index.column()) {
        case Name:
            return item.name();
        case Size: {
            if (item.isDir()) {
                const QVariant calculated = folderSizeData(item, role);
                return calculated.isValid() ? calculated : QVariant(QString{});
            }
            return KIO::convertSize(item.size());
        }
        case Type:
            return typeTextForRow(index.row(), /*requestSniff=*/true);
        case DateModified:
            return formatFileTime(item.time(KFileItem::ModificationTime));
        case Permissions:
            return item.permissionsString();
        case DateCreated:
            return formatFileTime(item.time(KFileItem::CreationTime));
        case DateAccessed:
            return formatFileTime(item.time(KFileItem::AccessTime));
        case Extension: {
            if (item.isDir())
                return QString{};
            // Deliberately not QFileInfo — this runs per visible cell per
            // repaint and only ever needs the text after the last dot.
            const QString name = item.name();
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            return dot > 0 ? name.mid(dot + 1) : QString{};
        }
        case Owner: {
            const QString user = ownerName(item);
            const QString group = groupName(item);
            return group.isEmpty() ? user : QString(user + QLatin1Char(':') + group);
        }
        }
        break;

    case Qt::DecorationRole:
        if (index.column() == Name) {
            // Prefer a generated thumbnail when one is ready; otherwise fall
            // straight through to the MIME icon. data() never blocks on
            // generation — the loader signals back and we re-emit dataChanged.
            if (m_thumbnailsEnabled && m_thumbLoader) {
                QIcon thumb;
                if (m_thumbLoader->thumbnail(item.url(), thumb))
                    return thumb;
            }
            return cachedIcon(item);
        }
        break;

    case FileItemRole:
        return QVariant::fromValue(item);

    case UrlRole:
        return item.url();

    case IsDirRole:
        return item.isDir();

    case NumericSizeRole: {
        if (item.isDir()) {
            const QVariant calculated = folderSizeData(item, role);
            return calculated.isValid() ? calculated : QVariant(qint64(-1));
        }
        return static_cast<qint64>(item.size());
    }

    case DateTimeRole:
        switch (index.column()) {
        case DateModified: return item.time(KFileItem::ModificationTime);
        case DateCreated:  return item.time(KFileItem::CreationTime);
        case DateAccessed: return item.time(KFileItem::AccessTime);
        default: break;
        }
        break;

    case Qt::ToolTipRole:
        if (index.column() == Name)
            return item.url().toDisplayString(QUrl::PreferLocalFile); // toLocalFile() is empty for sftp/smb/…
        break;
    }

    return {};
}

QVariant FileItemModelBase::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::TextAlignmentRole)
        return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    if (role != Qt::DisplayRole)
        return {};

    switch (section) {
    case Name:         return i18n("Name");
    case Size:         return i18n("Size");
    case Type:         return i18n("Type");
    case DateModified: return i18n("Date Modified");
    case Permissions:  return i18n("Permissions");
    case DateCreated:  return i18n("Date Created");
    case DateAccessed: return i18n("Date Accessed");
    case Extension:    return i18n("Extension");
    case Owner:        return i18n("Owner");
    }
    return {};
}

Qt::ItemFlags FileItemModelBase::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == Name)
        f |= Qt::ItemIsEditable;
    if (index.row() < m_items.size() && m_items.at(index.row()).isDir())
        f |= Qt::ItemIsDropEnabled;
    return f;
}

QMimeData *FileItemModelBase::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    QSet<int> seen;
    for (const QModelIndex &idx : indexes) {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_items.size()
            || seen.contains(idx.row())) {
            continue;
        }
        seen.insert(idx.row());
        urls.append(m_items.at(idx.row()).url());
    }
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

Qt::DropActions FileItemModelBase::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}
