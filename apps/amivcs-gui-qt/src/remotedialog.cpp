/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "remotedialog.h"

extern "C" {
#include "vcsconfig.h"
}

#include <QFormLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <cstring>

RemoteDialog::RemoteDialog(const QString &url, const QString &user, bool refused, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Login"));

    auto *intro = new QLabel(refused ? tr("<b>Login refused</b> -- try again.<br>%1").arg(url.toHtmlEscaped())
                                     : url.toHtmlEscaped(), this);
    m_user = new QLineEdit(user, this);
    m_pass = new QLineEdit(this);
    m_pass->setEchoMode(QLineEdit::Password);
    m_remember = new QCheckBox(tr("Remember password (plain text in .amivcs/config)"), this);

    auto *form = new QFormLayout;
    form->addRow(tr("User:"), m_user);
    form->addRow(tr("Password:"), m_pass);
    form->addRow(QString(), m_remember);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(buttons);
    (user.isEmpty() ? m_user : m_pass)->setFocus();
}

VcsGuiLogin RemoteDialog::result() const
{
    VcsGuiLogin login;
    std::memset(&login, 0, sizeof(login));
    QByteArray user = m_user->text().trimmed().toUtf8();
    QByteArray pass = m_pass->text().toUtf8();
    std::strncpy(login.user, user.constData(), sizeof(login.user) - 1);
    std::strncpy(login.pass, pass.constData(), sizeof(login.pass) - 1);
    login.remember = m_remember->isChecked() ? 1 : 0;
    return login;
}

/* ------------------------------------------------------------------ */

static QString repoValue(const QString &amivcsDir, const char *key)
{
    char path[1024], v[1024];
    vcs_repo_config_path(amivcsDir.toUtf8().constData(), path, sizeof(path));
    return vcs_config_file_get(path, key, v, sizeof(v)) ? QString::fromUtf8(v) : QString();
}

static QString globalValue(const char *key)
{
    char v[256];
    return vcs_global_get(key, v, sizeof(v)) ? QString::fromUtf8(v) : QString();
}

SettingsDialog::SettingsDialog(const QString &amivcsDir, QWidget *parent)
    : QDialog(parent), m_amivcsDir(amivcsDir)
{
    setWindowTitle(tr("AMIVCS Settings"));

    auto *repoBox = new QGroupBox(tr("This repository"), this);
    auto *repoForm = new QFormLayout(repoBox);
    m_remote = new QLineEdit(repoValue(amivcsDir, "remote"), this);
    m_remote->setPlaceholderText(QStringLiteral("https://server/name"));
    m_user = new QLineEdit(repoValue(amivcsDir, "user"), this);
    m_name = new QLineEdit(repoValue(amivcsDir, "name"), this);
    m_name->setPlaceholderText(tr("(global default)"));
    m_email = new QLineEdit(repoValue(amivcsDir, "email"), this);
    m_email->setPlaceholderText(tr("(global default)"));
    m_forget = new QCheckBox(tr("Forget the remembered password"), this);
    m_forget->setEnabled(!repoValue(amivcsDir, "pass").isEmpty());
    repoForm->addRow(tr("Remote URL:"), m_remote);
    repoForm->addRow(tr("User:"), m_user);
    repoForm->addRow(QString(), m_forget);
    repoForm->addRow(tr("Name:"), m_name);
    repoForm->addRow(tr("E-mail:"), m_email);

    auto *globalBox = new QGroupBox(tr("Defaults for all repositories"), this);
    auto *globalForm = new QFormLayout(globalBox);
    m_globalName = new QLineEdit(globalValue("name"), this);
    m_globalEmail = new QLineEdit(globalValue("email"), this);
    globalForm->addRow(tr("Name:"), m_globalName);
    globalForm->addRow(tr("E-mail:"), m_globalEmail);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(repoBox);
    layout->addWidget(globalBox);
    layout->addWidget(buttons);
    resize(520, sizeHint().height());
}

void SettingsDialog::save()
{
    const QByteArray dir = m_amivcsDir.toUtf8();
    const QString remote = m_remote->text().trimmed();
    if (!remote.isEmpty()) {
        char server[1024], name[256];
        if (!vcs_url_split(remote.toUtf8().constData(), server, sizeof(server), name, sizeof(name))) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("'%1' is not a repository URL (https://server/name).").arg(remote));
            return;
        }
    }
    auto set = [&dir](const char *key, const QString &v) {
        vcs_repo_config_set(dir.constData(), key, v.trimmed().toUtf8().constData());
    };
    set("remote", remote);
    set("user", m_user->text());
    set("name", m_name->text());
    set("email", m_email->text());
    if (m_forget->isChecked()) vcs_repo_config_set(dir.constData(), "pass", nullptr);
    vcs_global_set("name", m_globalName->text().trimmed().toUtf8().constData());
    vcs_global_set("email", m_globalEmail->text().trimmed().toUtf8().constData());
    accept();
}
