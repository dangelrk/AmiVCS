/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "mainwindow.h"
#include "logtablemodel.h"
#include "graphdelegate.h"
#include "remotedialog.h"

extern "C" {
#include "config.h"
}

#include <QListWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTableView>
#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QProgressDialog>
#include <QHeaderView>
#include <QListWidgetItem>
#include <QToolBar>
#include <QStyle>

static QString kindLabel(char kind)
{
    switch (kind) {
    case 'N': return QObject::tr("new");
    case 'M': return QObject::tr("modified");
    case 'D': return QObject::tr("deleted");
    }
    return QString();
}

static QString shortBranchName(const char *fullRefName)
{
    QString n = QString::fromUtf8(fullRefName);
    if (n.startsWith(QStringLiteral("refs/heads/"))) return n.mid(11);
    return n;
}

MainWindow::MainWindow(const QString &cwd, const AcConfig &cfg, QWidget *parent)
    : QMainWindow(parent), m_cfg(cfg), m_cwd(cwd), m_job(nullptr)
{
    vcsgui_session_init(&m_session);

    setWindowTitle(tr("AMIVCS GUI"));
    resize(900, 700);

    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);

    m_branchLabel = new QLabel(this);
    mainLayout->addWidget(m_branchLabel);

    auto *changesSplit = new QSplitter(Qt::Horizontal, this);

    auto *unstagedBox = new QWidget(this);
    auto *unstagedLayout = new QVBoxLayout(unstagedBox);
    unstagedLayout->addWidget(new QLabel(tr("Changes not staged for commit"), this));
    m_unstagedList = new QListWidget(this);
    m_unstagedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedLayout->addWidget(m_unstagedList);
    auto *unstagedBtns = new QHBoxLayout;
    m_stageBtn = new QPushButton(tr("Stage →"), this);
    m_stageAllBtn = new QPushButton(tr("Stage All"), this);
    unstagedBtns->addWidget(m_stageBtn);
    unstagedBtns->addWidget(m_stageAllBtn);
    unstagedLayout->addLayout(unstagedBtns);
    changesSplit->addWidget(unstagedBox);

    auto *stagedBox = new QWidget(this);
    auto *stagedLayout = new QVBoxLayout(stagedBox);
    stagedLayout->addWidget(new QLabel(tr("Changes to be committed"), this));
    m_stagedList = new QListWidget(this);
    m_stagedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    stagedLayout->addWidget(m_stagedList);
    auto *stagedBtns = new QHBoxLayout;
    m_unstageBtn = new QPushButton(tr("← Unstage"), this);
    stagedBtns->addWidget(m_unstageBtn);
    stagedLayout->addLayout(stagedBtns);
    changesSplit->addWidget(stagedBox);

    mainLayout->addWidget(changesSplit, 2);

    auto *commitBox = new QWidget(this);
    auto *commitLayout = new QVBoxLayout(commitBox);
    commitLayout->addWidget(new QLabel(tr("Commit message"), this));
    m_commitMsg = new QPlainTextEdit(this);
    m_commitMsg->setMaximumHeight(80);
    commitLayout->addWidget(m_commitMsg);
    auto *commitBtnRow = new QHBoxLayout;
    m_stageAllCheck = new QCheckBox(tr("Stage all changes before commit (-a)"), this);
    m_commitBtn = new QPushButton(tr("Commit"), this);
    commitBtnRow->addWidget(m_stageAllCheck);
    commitBtnRow->addStretch();
    commitBtnRow->addWidget(m_commitBtn);
    commitLayout->addLayout(commitBtnRow);
    mainLayout->addWidget(commitBox);

    mainLayout->addWidget(new QLabel(tr("History"), this));
    m_historyView = new QTableView(this);
    m_logModel = new LogTableModel(&m_session, this);
    m_historyView->setModel(m_logModel);
    m_graphDelegate = new GraphDelegate(m_logModel, this);
    m_historyView->setItemDelegateForColumn(LogTableModel::ColGraph, m_graphDelegate);
    m_historyView->verticalHeader()->setVisible(false);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyView->horizontalHeader()->setStretchLastSection(true);
    mainLayout->addWidget(m_historyView, 3);

    setCentralWidget(central);

    auto *repoMenu = menuBar()->addMenu(tr("&Repository"));
    m_actInit = repoMenu->addAction(tr("&Init Repository Here"), this, &MainWindow::onInitRepoHere);
    m_actRefresh = repoMenu->addAction(tr("&Refresh"), this, &MainWindow::onRefresh);
    repoMenu->addSeparator();
    m_actNewBranch = repoMenu->addAction(tr("&New Branch..."), this, &MainWindow::onNewBranch);
    m_actSwitch = repoMenu->addAction(tr("&Switch Branch..."), this, &MainWindow::onSwitchBranch);
    m_actMerge = repoMenu->addAction(tr("&Merge Branch..."), this, &MainWindow::onMerge);

    auto *remoteMenu = menuBar()->addMenu(tr("Remo&te"));
    m_actClone = remoteMenu->addAction(tr("&Clone..."), this, &MainWindow::onClone);
    m_actPush = remoteMenu->addAction(tr("&Push"), this, &MainWindow::onPush);
    m_actPull = remoteMenu->addAction(tr("Pu&ll..."), this, &MainWindow::onPull);

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, &MainWindow::onAbout);

    /* A visible Refresh button, not just the Repository menu entry above --
     * the same action either way (m_actRefresh), so the enabled/disabled
     * state in updateActionsEnabled() covers both. */
    auto *mainToolBar = addToolBar(tr("Main"));
    mainToolBar->setMovable(false);
    m_actRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    mainToolBar->addAction(m_actRefresh);

    statusBar()->showMessage(tr("Ready."));

    connect(m_stageBtn, &QPushButton::clicked, this, &MainWindow::onStageSelected);
    connect(m_unstagedList, &QListWidget::itemDoubleClicked, this, &MainWindow::onStageSelected);
    connect(m_stageAllBtn, &QPushButton::clicked, this, &MainWindow::onStageAll);
    connect(m_unstageBtn, &QPushButton::clicked, this, &MainWindow::onUnstageSelected);
    connect(m_stagedList, &QListWidget::itemDoubleClicked, this, &MainWindow::onUnstageSelected);
    connect(m_commitBtn, &QPushButton::clicked, this, &MainWindow::onCommit);

    openOrOfferInit(cwd);
}

MainWindow::~MainWindow()
{
    vcsgui_close(&m_session);
}

void MainWindow::openOrOfferInit(const QString &dir)
{
    if (vcsgui_open(&m_session, dir.toUtf8().constData())) { refreshUI(); return; }

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Not a repository"),
        tr("'%1' is not an AMIVCS repository yet.\n\nInitialize one here?").arg(dir),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        if (!vcsgui_init_repo(&m_session, dir.toUtf8().constData())) {
            QMessageBox::critical(this, tr("Init failed"), tr("Could not initialize a repository here."));
        }
    }
    refreshUI();
}

void MainWindow::onInitRepoHere()
{
    if (m_session.is_open) { QMessageBox::information(this, tr("Init"), tr("Already an open repository.")); return; }
    if (!vcsgui_init_repo(&m_session, m_cwd.toUtf8().constData())) {
        QMessageBox::critical(this, tr("Init failed"), tr("Could not initialize a repository here."));
    }
    refreshUI();
}

void MainWindow::refreshUI()
{
    m_unstagedList->clear();
    m_stagedList->clear();

    if (m_session.is_open) {
        for (int i = 0; i < m_session.unstaged.n; i++) {
            const VcsChange &c = m_session.unstaged.items[i];
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(kindLabel(c.kind), QString::fromUtf8(c.path)));
            item->setData(Qt::UserRole, QString::fromUtf8(c.path));
            m_unstagedList->addItem(item);
        }
        for (int i = 0; i < m_session.staged.n; i++) {
            const VcsChange &c = m_session.staged.items[i];
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(kindLabel(c.kind), QString::fromUtf8(c.path)));
            item->setData(Qt::UserRole, QString::fromUtf8(c.path));
            m_stagedList->addItem(item);
        }

        QString branchText = m_session.head.detached
            ? tr("(detached HEAD)")
            : shortBranchName(m_session.head.branch);
        m_branchLabel->setText(tr("Repository: %1  —  Branch: %2").arg(m_cwd, branchText));
        setWindowTitle(QStringLiteral("AMIVCS GUI - %1 [%2]").arg(m_cwd, branchText));
    } else {
        m_branchLabel->setText(tr("(not a repository -- use Repository > Init Repository Here)"));
        setWindowTitle(tr("AMIVCS GUI"));
    }

    m_logModel->refresh();
    m_historyView->resizeColumnsToContents();
    updateActionsEnabled();
}

void MainWindow::updateActionsEnabled()
{
    bool open = m_session.is_open;
    m_actInit->setEnabled(!open);
    m_actRefresh->setEnabled(open);
    m_actNewBranch->setEnabled(open);
    m_actSwitch->setEnabled(open);
    m_actMerge->setEnabled(open);
    m_actPush->setEnabled(open);
    m_actPull->setEnabled(open);
    m_stageBtn->setEnabled(open);
    m_unstageBtn->setEnabled(open);
    m_stageAllBtn->setEnabled(open);
    m_commitBtn->setEnabled(open);
}

void MainWindow::onRefresh()
{
    refreshUI();
}

void MainWindow::onStageSelected()
{
    QList<QListWidgetItem *> items = m_unstagedList->selectedItems();
    if (items.isEmpty() && m_unstagedList->currentItem()) items << m_unstagedList->currentItem();
    for (QListWidgetItem *item : items) {
        QByteArray path = item->data(Qt::UserRole).toString().toUtf8();
        vcsgui_add(&m_session, path.constData());
    }
    refreshUI();
}

void MainWindow::onUnstageSelected()
{
    QList<QListWidgetItem *> items = m_stagedList->selectedItems();
    if (items.isEmpty() && m_stagedList->currentItem()) items << m_stagedList->currentItem();
    for (QListWidgetItem *item : items) {
        QByteArray path = item->data(Qt::UserRole).toString().toUtf8();
        vcsgui_remove(&m_session, path.constData());
    }
    refreshUI();
}

void MainWindow::onStageAll()
{
    vcsgui_add_all(&m_session);
    refreshUI();
}

void MainWindow::onCommit()
{
    QString msg = m_commitMsg->toPlainText().trimmed();
    if (msg.isEmpty()) {
        QMessageBox::warning(this, tr("Commit"), tr("Enter a commit message first."));
        return;
    }
    QByteArray msgUtf8 = msg.toUtf8();
    VcsOid oid;
    if (!vcsgui_commit(&m_session, msgUtf8.constData(), m_stageAllCheck->isChecked() ? 1 : 0, &oid)) {
        QMessageBox::warning(this, tr("Commit"), QString::fromUtf8(vcsgui_last_message()));
        return;
    }
    m_commitMsg->clear();
    refreshUI();
    statusBar()->showMessage(tr("Committed."), 3000);
}

void MainWindow::onNewBranch()
{
    bool ok;
    QString name = QInputDialog::getText(this, tr("New Branch"), tr("Branch name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;
    if (!vcsgui_branch_create(&m_session, name.toUtf8().constData())) {
        QMessageBox::warning(this, tr("New Branch"), QString::fromUtf8(vcsgui_last_message()));
    }
    refreshUI();
}

QString MainWindow::pickBranch(const QString &title, const QString &label)
{
    QStringList names;
    for (int i = 0; i < m_session.branches.n; i++) names << shortBranchName(m_session.branches.items[i].name);
    if (names.isEmpty()) {
        QMessageBox::information(this, title, tr("No branches yet -- commit first."));
        return QString();
    }
    bool ok;
    QString chosen = QInputDialog::getItem(this, title, label, names, 0, false, &ok);
    return ok ? chosen : QString();
}

void MainWindow::onSwitchBranch()
{
    QString branch = pickBranch(tr("Switch Branch"), tr("Branch:"));
    if (branch.isEmpty()) return;
    if (!vcsgui_switch(&m_session, branch.toUtf8().constData())) {
        QMessageBox::warning(this, tr("Switch Branch"), QString::fromUtf8(vcsgui_last_message()));
    }
    refreshUI();
}

void MainWindow::onMerge()
{
    QString branch = pickBranch(tr("Merge Branch"), tr("Merge which branch into the current one?"));
    if (branch.isEmpty()) return;
    if (!vcsgui_merge(&m_session, branch.toUtf8().constData())) {
        QMessageBox::warning(this, tr("Merge"), QString::fromUtf8(vcsgui_last_message()));
    } else {
        statusBar()->showMessage(QString::fromUtf8(vcsgui_last_message()), 5000);
    }
    refreshUI();
}

bool MainWindow::ensureRemoteConfigured()
{
    if (m_cfg.server[0] && m_cfg.username[0] && m_cfg.password[0]) return true;
    RemoteDialog dlg(m_cfg, this);
    if (dlg.exec() != QDialog::Accepted) return false;
    m_cfg = dlg.result();
    return m_cfg.server[0] && m_cfg.username[0] && m_cfg.password[0];
}

void MainWindow::startRemoteJob(RemoteJob::Kind kind, const QString &repoName, const QString &extra)
{
    if (m_job) return;

    auto *dlg = new QProgressDialog(tr("Working..."), tr("Cancel"), 0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);

    m_job = new RemoteJob(kind, &m_session, m_cfg, repoName, extra, this);

    connect(m_job, &RemoteJob::progress, this, [dlg](qint64 done, qint64 total, QString label) {
        if (total > 0) {
            dlg->setMaximum((int)total);
            dlg->setValue((int)done);
        } else {
            dlg->setMaximum(0);
        }
        if (!label.isEmpty()) dlg->setLabelText(label);
    });
    connect(dlg, &QProgressDialog::canceled, m_job, &RemoteJob::requestCancel);
    connect(m_job, &RemoteJob::jobFinished, this, [this, dlg](bool ok) {
        dlg->close();
        dlg->deleteLater();
        RemoteJob *job = m_job;
        m_job = nullptr;
        job->deleteLater();
        refreshUI();
        statusBar()->showMessage(ok ? tr("Done.") : QString::fromUtf8(vcsgui_last_message()), 5000);
    });

    m_job->start();
    dlg->show();
}

void MainWindow::onClone()
{
    if (!ensureRemoteConfigured()) return;
    bool ok;
    QString repoName = QInputDialog::getText(this, tr("Clone"), tr("Repository name on server:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || repoName.isEmpty()) return;
    QString dir = QFileDialog::getExistingDirectory(this, tr("Clone into..."));
    if (dir.isEmpty()) return;
    startRemoteJob(RemoteJob::Clone, repoName, dir);
}

void MainWindow::onPush()
{
    if (!m_session.is_open) return;
    if (!ensureRemoteConfigured()) return;
    startRemoteJob(RemoteJob::Push, QString(), QString());
}

void MainWindow::onPull()
{
    if (!m_session.is_open) return;
    if (!ensureRemoteConfigured()) return;
    startRemoteJob(RemoteJob::Pull, QString(), QString());
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About AmiVCS GUI"),
        tr("AmiVCS GUI v%1\n\nA minimal, git-gui-style graphical front-end for AmiVCS.\n\n"
           "(C) 2026 Ronny Krueger\nGNU General Public License v3 or later").arg(QString::fromUtf8(AC_VERSION)));
}
