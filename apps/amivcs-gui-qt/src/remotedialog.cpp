/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "remotedialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <cstring>

RemoteDialog::RemoteDialog(const AcConfig &initial, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AMIVCS - Remote Server"));

    m_server = new QLineEdit(QString::fromUtf8(initial.server), this);
    m_user   = new QLineEdit(QString::fromUtf8(initial.username), this);
    m_pass   = new QLineEdit(QString::fromUtf8(initial.password), this);
    m_pass->setEchoMode(QLineEdit::Password);
    m_verifySsl = new QCheckBox(tr("Verify TLS certificate"), this);
    m_verifySsl->setChecked(initial.verify_ssl != 0);

    auto *form = new QFormLayout;
    form->addRow(tr("Server URL:"), m_server);
    form->addRow(tr("Username:"), m_user);
    form->addRow(tr("Password:"), m_pass);
    form->addRow(QString(), m_verifySsl);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

AcConfig RemoteDialog::result() const
{
    AcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));

    QByteArray server = m_server->text().toUtf8();
    QByteArray user   = m_user->text().toUtf8();
    QByteArray pass   = m_pass->text().toUtf8();

    std::strncpy(cfg.server, server.constData(), sizeof(cfg.server) - 1);
    std::strncpy(cfg.username, user.constData(), sizeof(cfg.username) - 1);
    std::strncpy(cfg.password, pass.constData(), sizeof(cfg.password) - 1);
    cfg.basepath[0] = '/';
    cfg.verify_ssl = m_verifySsl->isChecked() ? 1 : 0;
    return cfg;
}
