/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "graphdelegate.h"
#include "logtablemodel.h"

#include <QPainter>
#include <algorithm>

GraphDelegate::GraphDelegate(LogTableModel *model, QObject *parent)
    : QStyledItemDelegate(parent), m_model(model)
{
}

int GraphDelegate::columnWidth() const
{
    int lanes = std::max(m_model->graph()->max_lanes, 1);
    return kLaneWidth * lanes + 8;
}

void GraphDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());
    else
        painter->fillRect(option.rect, option.palette.base());

    const VcsGraphLayout *g = m_model->graph();
    int row = index.row();
    if (row < 0 || row >= g->n) { painter->restore(); return; }
    const VcsGraphRow &r = g->items[row];

    int left = option.rect.left();
    int cyTop = option.rect.top();
    int cyMid = option.rect.top() + option.rect.height() / 2;
    int cyBot = option.rect.bottom() + 1;

    QPen linePen(QColor(140, 140, 140), 2);
    painter->setPen(linePen);

    /* Full-height passthrough for every other lane alive at this row. */
    for (int i = 0; i < r.nactive_lanes; i++) {
        int lane = r.active_lanes[i];
        if (lane == r.lane) continue;
        int x = left + lane * kLaneWidth + kLaneWidth / 2;
        painter->drawLine(x, cyTop, x, cyBot);
    }

    int x0 = left + r.lane * kLaneWidth + kLaneWidth / 2;

    /* Inbound top-half segment, only if this lane was also active in the
     * row above (a genuine branch tip has nothing to connect upward to). */
    if (row > 0) {
        const VcsGraphRow &prev = g->items[row - 1];
        bool activeAbove = false;
        for (int i = 0; i < prev.nactive_lanes; i++) {
            if (prev.active_lanes[i] == r.lane) { activeAbove = true; break; }
        }
        if (activeAbove) painter->drawLine(x0, cyTop, x0, cyMid);
    }

    /* Outbound segment(s) down to this commit's parent lane(s). */
    for (int i = 0; i < r.nparent_lanes; i++) {
        int x1 = left + r.parent_lanes[i] * kLaneWidth + kLaneWidth / 2;
        painter->drawLine(x0, cyMid, x1, cyBot);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(40, 90, 190));
    painter->drawEllipse(QPoint(x0, cyMid), 4, 4);

    painter->restore();
}

QSize GraphDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(columnWidth(), kRowHeight);
}
