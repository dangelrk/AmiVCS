/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef REMOTEDIALOG_H
#define REMOTEDIALOG_H

#include <QDialog>

extern "C" {
#include "session.h"
}

class QLineEdit;
class QCheckBox;

/* Login for one clone/push/pull: user, password, "remember the password"
 * (kept in the repository's .amivcs/config, see vcsconfig.h). */
class RemoteDialog : public QDialog
{
    Q_OBJECT
public:
    RemoteDialog(const QString &url, const QString &user, bool refused, QWidget *parent = nullptr);
    VcsGuiLogin result() const;

private:
    QLineEdit *m_user;
    QLineEdit *m_pass;
    QCheckBox *m_remember;
};

/* Repository > Settings...: the repository's remote URL, user and
 * identity, the project's long-name switch (.amivcsproject, versioned)
 * and the global name/email defaults. */
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(const QString &amivcsDir, const VcsRepo *repo, QWidget *parent = nullptr);

private slots:
    void save();

private:
    QString m_amivcsDir;
    const VcsRepo *m_repo;
    QLineEdit *m_remote, *m_user, *m_name, *m_email, *m_globalName, *m_globalEmail;
    QCheckBox *m_forget;
    QCheckBox *m_longNames;
};

#endif // REMOTEDIALOG_H
