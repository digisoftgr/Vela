#pragma once

#include "FileItemModelBase.h"

#include <KFileItem>

// Append-only list of search results.
//
// Columns, roles and data() formatting all come from FileItemModelBase, so this
// model is column-for-column identical to FileSystemModel — which is what lets
// Panel address either of them uniformly through activeModel(), and lets the
// details view stay sortable in search mode.
class SearchResultsModel : public FileItemModelBase
{
    Q_OBJECT

public:
    explicit SearchResultsModel(QObject *parent = nullptr);

    void clear();
    void addResults(const KFileItemList &items);
    // Removes the rows for `urls` (those present) and returns their items, so
    // a caller whose operation fails can put back the ones that survived.
    KFileItemList takeItems(const QList<QUrl> &urls);
    int resultCount() const { return m_items.size(); }
    const KFileItemList &items() const { return m_items; }

    void setSearchTerm(const QString &term) { m_searchTerm = term; }
    QString searchTerm() const { return m_searchTerm; }

    // Results are draggable but not renamed in place — renaming happens after
    // navigating to the containing folder.
    Qt::ItemFlags flags(const QModelIndex &index) const override;

private:
    QString m_searchTerm;
};
