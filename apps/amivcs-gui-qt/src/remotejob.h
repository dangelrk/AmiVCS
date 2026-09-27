/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef REMOTEJOB_H
#define REMOTEJOB_H

#include <QThread>
#include <QString>

extern "C" {
#include "session.h"
}

/* Runs vcsgui_clone()/push()/pull() on a worker QThread so the main
 * window stays responsive and a QProgressDialog can offer real cancel --
 * same shape as amicloud-commander-qt's TransferJob, but simpler: unlike
 * ui_hooks.h's UiHooks.progress, VcsGuiSession::progress_cb DOES take a
 * userdata pointer, so this needs no g_activeJob-style static bridge --
 * session.progress_userdata is just `this`. */
class RemoteJob : public QThread
{
    Q_OBJECT
public:
    enum Kind { Clone, Push, Pull };

    /* repoName may be empty (reuse .amivcs/config's remembered remote,
     * push/pull only). extra is the clone target directory (Clone) or
     * the branch to pull (Pull, empty = current branch); unused for Push. */
    RemoteJob(Kind kind, VcsGuiSession *session, const AcConfig &cfg,
             const QString &repoName, const QString &extra, QObject *parent = nullptr);

    void requestCancel();
    bool ok() const { return m_ok; }

signals:
    void progress(qint64 done, qint64 total, QString label);
    void jobFinished(bool ok);

protected:
    void run() override;

private:
    static int progressBridge(void *userdata, long done, long total, const char *label);

    Kind m_kind;
    VcsGuiSession *m_session;
    AcConfig m_cfg;
    QString m_repoName;
    QString m_extra;
    bool m_ok;
};

#endif // REMOTEJOB_H
