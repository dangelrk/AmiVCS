/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef LOGTABLEMODEL_H
#define LOGTABLEMODEL_H

#include <QAbstractTableModel>

extern "C" {
#include "session.h"
}

/* Thin QAbstractTableModel over VcsGuiSession::log_all/graph -- no local
 * copy of the data, just re-reads the session's cached fields (already
 * kept current by vcsgui_refresh(), called after every mutating session
 * call). Column 0 (Graph) renders no text -- GraphDelegate paints it
 * from graph()/log() directly. */
class LogTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { ColGraph = 0, ColOid, ColAuthor, ColDate, ColMessage, ColCount };

    explicit LogTableModel(VcsGuiSession *session, QObject *parent = nullptr);

    /* Call after vcsgui_refresh() changed session->log_all/graph. */
    void refresh();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    const VcsGraphLayout *graph() const { return &m_session->graph; }
    const VcsLogList *log() const { return &m_session->log_all; }

private:
    VcsGuiSession *m_session;
};

#endif // LOGTABLEMODEL_H
