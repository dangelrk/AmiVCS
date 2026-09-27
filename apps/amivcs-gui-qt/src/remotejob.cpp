/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "remotejob.h"

RemoteJob::RemoteJob(Kind kind, VcsGuiSession *session, const AcConfig &cfg,
                     const QString &repoName, const QString &extra, QObject *parent)
    : QThread(parent), m_kind(kind), m_session(session), m_cfg(cfg),
      m_repoName(repoName), m_extra(extra), m_ok(false)
{
}

void RemoteJob::requestCancel()
{
    vcsgui_request_cancel(m_session);
}

int RemoteJob::progressBridge(void *userdata, long done, long total, const char *label)
{
    RemoteJob *self = static_cast<RemoteJob *>(userdata);
    self->progress((qint64)done, (qint64)total, QString::fromUtf8(label ? label : ""));
    return self->m_session->cancel_requested ? 0 : 1;
}

void RemoteJob::run()
{
    m_session->progress_cb = &RemoteJob::progressBridge;
    m_session->progress_userdata = this;

    QByteArray repoNameUtf8 = m_repoName.toUtf8();
    const char *repoNamePtr = repoNameUtf8.isEmpty() ? nullptr : repoNameUtf8.constData();

    switch (m_kind) {
    case Clone: {
        QByteArray dirUtf8 = m_extra.toUtf8();
        m_ok = vcsgui_clone(m_session, &m_cfg, repoNamePtr, dirUtf8.constData()) != 0;
        break;
    }
    case Push:
        m_ok = vcsgui_push(m_session, &m_cfg, repoNamePtr) != 0;
        break;
    case Pull: {
        QByteArray branchUtf8 = m_extra.toUtf8();
        const char *branchPtr = branchUtf8.isEmpty() ? nullptr : branchUtf8.constData();
        m_ok = vcsgui_pull(m_session, &m_cfg, repoNamePtr, branchPtr) != 0;
        break;
    }
    }

    m_session->progress_cb = nullptr;
    m_session->progress_userdata = nullptr;
    jobFinished(m_ok);
}
