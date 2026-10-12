#pragma once

#include <QCollator>
#include <QRegularExpression>
#include <QSortFilterProxyModel>

class FileItemModelBase;

class FileFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit FileFilterProxyModel(QObject *parent = nullptr);

    void setFilterPattern(const QString &pattern);
    QString filterPattern() const { return m_pattern; }
    bool hasFilter() const { return !m_pattern.isEmpty(); }

    void setFoldersFirst(bool first);
    bool foldersFirst() const { return m_foldersFirst; }

    void setSourceModel(QAbstractItemModel *model) override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    bool lessThanItems(int leftRow, int rightRow, int column) const;
    bool lessThanGeneric(const QModelIndex &left, const QModelIndex &right, bool dirsDone) const;

    FileItemModelBase *m_base = nullptr; // the source, when it is one of ours
    QString m_pattern;
    QString m_patternLower; // lowered once per pattern, not once per row
    QRegularExpression m_regex;
    QCollator m_collator;
    bool m_isGlob     = false;
    bool m_foldersFirst = true;
};
