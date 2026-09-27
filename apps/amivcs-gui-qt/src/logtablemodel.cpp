/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "logtablemodel.h"

#include <QDateTime>
#include <QTimeZone>

LogTableModel::LogTableModel(VcsGuiSession *session, QObject *parent)
    : QAbstractTableModel(parent), m_session(session)
{
}

void LogTableModel::refresh()
{
    beginResetModel();
    endResetModel();
}

int LogTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_session->log_all.n;
}

int LogTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return ColCount;
}

QVariant LogTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_session->log_all.n) return QVariant();
    const VcsLogEntry &e = m_session->log_all.items[index.row()];

    if (role != Qt::DisplayRole) return QVariant();

    switch (index.column()) {
    case ColGraph:
        return QVariant(); // GraphDelegate paints this column
    case ColOid: {
        char hex[VCS_OID_HEX];
        vcs_oid_to_hex(&e.oid, hex);
        return QString::fromUtf8(hex).left(8);
    }
    case ColAuthor:
        return QString::fromUtf8(e.author);
    case ColDate: {
        QDateTime dt = QDateTime::fromSecsSinceEpoch(e.mtime, QTimeZone::UTC);
        return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    case ColMessage: {
        QString msg = QString::fromUtf8(e.message ? e.message : "");
        if (e.decoration[0]) return QStringLiteral("[%1] %2").arg(QString::fromUtf8(e.decoration), msg);
        return msg;
    }
    }
    return QVariant();
}

QVariant LogTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    switch (section) {
    case ColGraph:   return QString();
    case ColOid:     return tr("OID");
    case ColAuthor:  return tr("Author");
    case ColDate:    return tr("Date");
    case ColMessage: return tr("Message");
    }
    return QVariant();
}
