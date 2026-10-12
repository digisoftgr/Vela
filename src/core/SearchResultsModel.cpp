#include "SearchResultsModel.h"

SearchResultsModel::SearchResultsModel(QObject *parent)
    : FileItemModelBase(parent)
{
}

void SearchResultsModel::clear()
{
    clearItems();
}

void SearchResultsModel::addResults(const KFileItemList &items)
{
    appendItems(items);
}

KFileItemList SearchResultsModel::takeItems(const QList<QUrl> &urls)
{
    KFileItemList taken;
    QList<int> rows;
    rows.reserve(urls.size());
    for (const QUrl &url : urls) {
        const int row = rowForUrl(url);
        if (row < 0)
            continue;
        taken.append(m_items.at(row));
        rows.append(row);
        onRowDropped(url);
    }
    removeRowsAt(rows);
    return taken;
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex &index) const
{
    // Same as the base class minus in-place editing: a result row's parent
    // directory is not the directory currently being listed, so committing an
    // inline rename here would have nowhere to apply.
    return FileItemModelBase::flags(index) & ~Qt::ItemIsEditable;
}
