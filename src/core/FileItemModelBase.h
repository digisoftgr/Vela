#pragma once

#include "MimeTypeResolver.h"
#include "ThumbnailLoader.h" // complete type required by QPointer below

#include <QAbstractItemModel>
#include <QCollator>
#include <QHash>
#include <QPointer>
#include <QUrl>
#include <KFileItem>
#include <KIO/Global>

#include <optional>
#include <vector>

// Common QAbstractItemModel adapter over a flat KFileItemList.
//
// FileSystemModel adds the KCoreDirLister and folder-size calculation on top of
// this; SearchResultsModel adds append-only result batches. Both therefore share
// one definition of the columns, the roles, and the (cached) data() formatting,
// so adding a column no longer means editing two classes that silently drift.
//
// Row lookup by URL is O(1): m_urlToRow is kept in sync by the mutation helpers
// below. Every mutation must go through them — touching m_items directly will
// desynchronise the index.
class FileItemModelBase : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column {
        Name = 0,
        Size,
        Type,
        DateModified,
        Permissions,
        // Optional columns (hidden by default)
        DateCreated,
        DateAccessed,
        Extension,
        Owner,
        ColumnCount
    };

    enum Role {
        FileItemRole    = Qt::UserRole + 1,
        UrlRole,
        IsDirRole,
        NumericSizeRole, // raw bytes (qint64); -1 means unknown (dir without calculated size)
        DateTimeRole,    // QDateTime for date columns — used for correct chronological sorting
        // Never returned by data(). Emitted in dataChanged when a sniffed MIME
        // type arrives: views repaint the cell, but a proxy sorted on
        // DisplayRole does not re-sort, so rows don't jump while types resolve.
        TypeResolvedRole,
    };

    explicit FileItemModelBase(QObject *parent = nullptr);

    int rowForUrl(const QUrl &url) const;

    // Byte size of the file at `row`; 0 for a directory or an out-of-range row.
    // Reads the item vector directly, deliberately bypassing data()/QVariant:
    // the status line sums this over every selected row, and going through the
    // role machinery costs a KFileItem copy boxed in a QVariant per file — the
    // difference between a tight loop and a visible stall on a Ctrl+A over a
    // directory with hundreds of thousands of entries.
    KIO::filesize_t fileSizeAtRow(int row) const
    {
        if (row < 0 || row >= m_items.size())
            return 0;
        const KFileItem &item = m_items.at(row);
        return item.isDir() ? 0 : item.size();
    }

    // Direct row access for the sort/filter proxy and the status line, for the
    // same reason as fileSizeAtRow: sorting 100k rows makes millions of
    // comparisons, and each data() call boxes a QVariant and copies a string.
    // `row` must be valid.
    const KFileItem &itemAt(int row) const { return m_items.at(row); }
    // Size column value in bytes; -1 for a directory without a calculated size.
    qint64 numericSizeAtRow(int row) const;
    // Collation key for the Name column (natural, case-insensitive), cached
    // per row so a sort compares keys instead of re-collating strings.
    const QCollatorSortKey &nameSortKey(int row) const;
    // Type column text for sorting. Cached per row, and never starts a content
    // sniff: sorting by Type used to queue one for every ambiguous file.
    QString sortTypeText(int row) const;
    // Collation key for sortTypeText(row). One key per distinct type text (a
    // few dozen), so sorting by Type compares keys instead of collating the
    // two strings on every comparison. By value: the table may grow between
    // the two lookups of one comparison.
    QCollatorSortKey typeSortKey(int row) const;

    // Maintained incrementally by the mutation helpers, so counting folders
    // vs files is O(1) instead of a walk over every row.
    int dirCount() const { return m_dirCount; }

    // Optional image-thumbnail source, shared with the owning Panel. When set
    // and enabled, DecorationRole prefers a generated thumbnail over the MIME
    // icon. The loader is not owned.
    void setThumbnailLoader(ThumbnailLoader *loader);
    // Thumbnails are only meaningful in the icon view modes; Details/List keep
    // 16px MIME icons and generate nothing.
    void setThumbnailsEnabled(bool enabled);
    bool thumbnailsEnabled() const { return m_thumbnailsEnabled; }
    ThumbnailLoader *thumbnailLoader() const { return m_thumbLoader; }

    void invalidateDateColumns();

    // Icons are cached process-wide by theme icon name. The theme can change at
    // runtime (KDE colour scheme / style switch), so the cache must be dropped.
    static void clearIconCache();
    // Re-reads the date format from VelaSettings. Called by invalidateDateColumns().
    static void refreshDateFormatCache();

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override;

protected:
    // Subclass hook: FileSystemModel overrides this to report calculated folder
    // sizes. Returns an invalid QVariant to fall through to the default handling.
    virtual QVariant folderSizeData(const KFileItem &item, int role) const;
    // Subclass hook: called when a row is dropped so subclasses can evict their
    // own per-URL caches (folder sizes, pending jobs).
    virtual void onRowDropped(const QUrl &url);

    // ── Mutation helpers — the only supported way to change m_items ───────────
    void appendItems(const KFileItemList &items);
    void replaceItemAt(int row, const KFileItem &item);
    void removeRowsAt(QList<int> rows);          // any order; handled bottom-up
    void clearItems();

    KFileItemList m_items;

private:
    void reindexFrom(int firstRow);
    // Type-column text without ever forcing a synchronous MIME determination.
    // `requestSniff` = queue a content sniff when the name is not decisive
    // (painting a cell); false for sorting.
    QString typeTextForRow(int row, bool requestSniff) const;
    // DecorationRole for the Name column, without any file access.
    QIcon iconForRow(int row) const;

    QHash<QUrl, int> m_urlToRow;

    // Per-row derived data, parallel to m_items and kept in step by the
    // mutation helpers. Mutable: filled lazily from const data()/sort calls.
    struct RowCache {
        enum TypeState : quint8 { TypeUnknown, TypePending, TypeFinal };
        QString  typeText;
        TypeState typeState = TypeUnknown;
        std::optional<QCollatorSortKey> nameKey;
        int typeKeyId = -1; // into m_typeKeys; reset whenever typeText changes
        // Icon from a worker-side resolution (content sniff, .desktop Icon=);
        // empty means the item's own name-based icon.
        QString iconName;
    };
    mutable std::vector<RowCache> m_rowCache;
    int m_dirCount = 0;
    mutable QList<QCollatorSortKey> m_typeKeys;
    mutable QHash<QString, int>     m_typeKeyIds; // type text → index in m_typeKeys

    // Content-sniffed MIME types, for the minority of files whose name is not
    // decisive. Results land in m_rowCache.
    MimeTypeResolver *m_mimeResolver = nullptr;

    QPointer<ThumbnailLoader> m_thumbLoader;
    bool m_thumbnailsEnabled = false;
};
