/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef GRAPHDELEGATE_H
#define GRAPHDELEGATE_H

#include <QStyledItemDelegate>

class LogTableModel;

/* Paints LogTableModel::ColGraph from VcsGraphLayout (libcloudvcsgui's
 * graph.h): a vertical line per active lane passing through a row, plus
 * a dot for the row's own commit and connector lines down to its
 * parent(s)' lanes. The MUI frontend renders the exact same
 * VcsGraphLayout data as ASCII-art text instead (no custom drawing) --
 * see amivcs-gui-mui/src/historylist.c. */
class GraphDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit GraphDelegate(LogTableModel *model, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    int columnWidth() const;

private:
    LogTableModel *m_model;
    static const int kLaneWidth = 14;
    static const int kRowHeight = 22;
};

#endif // GRAPHDELEGATE_H
