/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef REMOTEDIALOG_H
#define REMOTEDIALOG_H

#include <QDialog>

extern "C" {
#include "amicloud.h"
}

class QLineEdit;
class QCheckBox;

/* Shown only when clone/push/pull is first used and ~/.amivcsrc is
 * missing/incomplete -- same "editing an already-complete config happens
 * by hand-editing the rc file for now" v1 scope as amicloud-commander-
 * qt's ConnectDialog (this dialog does not persist anything to disk
 * either). Deliberately a separate class, not a shared one, even though
 * the fields are identical to ConnectDialog's -- AMIVCS's own AcConfig
 * (~/.amivcsrc) is fully separate from AmiCloud's (~/.amicloudrc). */
class RemoteDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteDialog(const AcConfig &initial, QWidget *parent = nullptr);
    AcConfig result() const;

private:
    QLineEdit *m_server;
    QLineEdit *m_user;
    QLineEdit *m_pass;
    QCheckBox *m_verifySsl;
};

#endif // REMOTEDIALOG_H
