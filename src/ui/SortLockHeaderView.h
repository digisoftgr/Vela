#pragma once
#include <QHeaderView>

class SortLockHeaderView : public QHeaderView
{
    Q_OBJECT
public:
    explicit SortLockHeaderView(Qt::Orientation orientation, QWidget *parent = nullptr);

    void setLockedSection(int section);
    int  lockedSection() const { return m_lockedSection; }

Q_SIGNALS:
    void lockClicked(int section);

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRect lockIconRect(const QRect &sectionRect) const;

    int m_lockedSection = 0; // FileSystemModel::Name by default
    static constexpr int kIconSize = 17;
};
