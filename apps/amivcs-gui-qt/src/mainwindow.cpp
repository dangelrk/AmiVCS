/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "mainwindow.h"
#include "logtablemodel.h"
#include "graphdelegate.h"
#include "remotedialog.h"

extern "C" {
#include "config.h"
#include "vcsname.h"
#include "vcsconfig.h"
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
#include <QTabWidget>
#include <QFontDatabase>
#include <QFile>
#include <QDir>

static QString kindLabel(char kind)
{
    switch (kind) {
    case 'N': return QObject::tr("new");
    case 'M': return QObject::tr("modified");
    case 'D': return QObject::tr("deleted");
    }
    return QString();
}

// A repository path for display: UTF-8, or ISO-8859-1 for a name an
// older Amiga build stored in its local codeset (vcsname.h). The raw
// bytes stay in the item's UserRole for add/remove.
static QString shownPath(const char *path)
{
    char *u = vcs_text_utf8(path);
    QString s = QString::fromUtf8(u ? u : path);
    free(u);
    return s;
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

    // Laid out like git gui: changed and staged files on the left, the
    // selected file on the right, buttons and the commit message below;
    // the history is a tab of its own.
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);

    m_branchLabel = new QLabel(this);
    m_branchLabel->setTextFormat(Qt::RichText);
    mainLayout->addWidget(m_branchLabel);

    auto *tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs, 1);

    // ---- tab "Changes" ----
    auto *changesPage = new QWidget(this);
    auto *changesLayout = new QVBoxLayout(changesPage);
    auto *vertSplit = new QSplitter(Qt::Vertical, changesPage);
    auto *topSplit = new QSplitter(Qt::Horizontal, vertSplit);
    auto *listsSplit = new QSplitter(Qt::Vertical, topSplit);

    auto *unstagedBox = new QWidget(listsSplit);
    auto *unstagedLayout = new QVBoxLayout(unstagedBox);
    unstagedLayout->setContentsMargins(0, 0, 0, 0);
    unstagedLayout->addWidget(new QLabel(tr("<b>Unstaged Changes</b>"), this));
    m_unstagedList = new QListWidget(this);
    m_unstagedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedLayout->addWidget(m_unstagedList);
    auto *unstagedBtns = new QHBoxLayout;
    m_stageBtn = new QPushButton(tr("&Stage selected"), this);
    m_stageAllBtn = new QPushButton(tr("Stage &all"), this);
    unstagedBtns->addWidget(m_stageBtn);
    unstagedBtns->addWidget(m_stageAllBtn);
    unstagedLayout->addLayout(unstagedBtns);

    auto *stagedBox = new QWidget(listsSplit);
    auto *stagedLayout = new QVBoxLayout(stagedBox);
    stagedLayout->setContentsMargins(0, 0, 0, 0);
    stagedLayout->addWidget(new QLabel(tr("<b>Staged Changes (Will Commit)</b>"), this));
    m_stagedList = new QListWidget(this);
    m_stagedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    stagedLayout->addWidget(m_stagedList);
    m_unstageBtn = new QPushButton(tr("&Unstage selected"), this);
    stagedLayout->addWidget(m_unstageBtn);

    auto *previewBox = new QWidget(topSplit);
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewTitle = new QLabel(this);
    m_previewTitle->setTextFormat(Qt::RichText);
    previewLayout->addWidget(m_previewTitle);
    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    previewLayout->addWidget(m_preview);
    topSplit->setStretchFactor(0, 35);
    topSplit->setStretchFactor(1, 65);

    auto *bottom = new QWidget(vertSplit);
    auto *bottomLayout = new QHBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    auto *btnColumn = new QVBoxLayout;
    auto *rescanBtn = new QPushButton(tr("&Rescan"), this);
    m_commitBtn = new QPushButton(tr("&Commit"), this);
    auto *pushBtn = new QPushButton(tr("&Push"), this);
    auto *pullBtn = new QPushButton(tr("Pu&ll"), this);
    btnColumn->addWidget(rescanBtn);
    btnColumn->addWidget(m_commitBtn);
    btnColumn->addWidget(pushBtn);
    btnColumn->addWidget(pullBtn);
    btnColumn->addStretch();
    bottomLayout->addLayout(btnColumn);
    auto *msgLayout = new QVBoxLayout;
    msgLayout->addWidget(new QLabel(tr("<b>Commit message:</b>"), this));
    m_commitMsg = new QPlainTextEdit(this);
    msgLayout->addWidget(m_commitMsg);
    m_stageAllCheck = new QCheckBox(tr("Stage all changed files before commit (-a)"), this);
    msgLayout->addWidget(m_stageAllCheck);
    bottomLayout->addLayout(msgLayout, 1);
    vertSplit->setStretchFactor(0, 70);
    vertSplit->setStretchFactor(1, 30);

    changesLayout->addWidget(vertSplit);
    tabs->addTab(changesPage, tr("Changes"));

    // ---- tab "History" ----
    auto *historyPage = new QWidget(this);
    auto *historyLayout = new QVBoxLayout(historyPage);
    m_historyView = new QTableView(this);
    m_logModel = new LogTableModel(&m_session, this);
    m_historyView->setModel(m_logModel);
    m_graphDelegate = new GraphDelegate(m_logModel, this);
    m_historyView->setItemDelegateForColumn(LogTableModel::ColGraph, m_graphDelegate);
    m_historyView->verticalHeader()->setVisible(false);
    m_historyView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyView->horizontalHeader()->setStretchLastSection(true);
    historyLayout->addWidget(m_historyView);
    auto *branchBtns = new QHBoxLayout;
    auto *newBranchBtn = new QPushButton(tr("New Branch..."), this);
    auto *switchBtn = new QPushButton(tr("Switch..."), this);
    auto *mergeBtn = new QPushButton(tr("Merge..."), this);
    auto *cloneBtn = new QPushButton(tr("Clone..."), this);
    branchBtns->addWidget(newBranchBtn);
    branchBtns->addWidget(switchBtn);
    branchBtns->addWidget(mergeBtn);
    branchBtns->addWidget(cloneBtn);
    historyLayout->addLayout(branchBtns);
    tabs->addTab(historyPage, tr("History"));

    setCentralWidget(central);

    connect(rescanBtn, &QPushButton::clicked, this, &MainWindow::onRefresh);
    connect(pushBtn, &QPushButton::clicked, this, &MainWindow::onPush);
    connect(pullBtn, &QPushButton::clicked, this, &MainWindow::onPull);
    connect(newBranchBtn, &QPushButton::clicked, this, &MainWindow::onNewBranch);
    connect(switchBtn, &QPushButton::clicked, this, &MainWindow::onSwitchBranch);
    connect(mergeBtn, &QPushButton::clicked, this, &MainWindow::onMerge);
    connect(cloneBtn, &QPushButton::clicked, this, &MainWindow::onClone);
    connect(m_unstagedList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) { showPreview(item); });
    connect(m_stagedList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) { showPreview(item); });
    m_pushBtn = pushBtn;
    m_pullBtn = pullBtn;
    m_rescanBtn = rescanBtn;

    auto *repoMenu = menuBar()->addMenu(tr("&Repository"));
    m_actInit = repoMenu->addAction(tr("&Init Repository Here"), this, &MainWindow::onInitRepoHere);
    m_actRefresh = repoMenu->addAction(tr("&Refresh"), this, &MainWindow::onRefresh);
    m_actSettings = repoMenu->addAction(tr("S&ettings..."), this, &MainWindow::onSettings);
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
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(kindLabel(c.kind), shownPath(c.path)));
            item->setData(Qt::UserRole, QByteArray(c.path));   // raw repository bytes
            item->setData(Qt::UserRole + 1, QChar(c.kind));
            m_unstagedList->addItem(item);
        }
        for (int i = 0; i < m_session.staged.n; i++) {
            const VcsChange &c = m_session.staged.items[i];
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(kindLabel(c.kind), shownPath(c.path)));
            item->setData(Qt::UserRole, QByteArray(c.path));   // raw repository bytes
            item->setData(Qt::UserRole + 1, QChar(c.kind));
            m_stagedList->addItem(item);
        }

        QString branchText = m_session.head.detached
            ? tr("(detached HEAD)")
            : shortBranchName(m_session.head.branch);
        char remote[1024], user[64];
        vcsgui_remote_info(&m_session, remote, sizeof(remote), user, sizeof(user), nullptr);
        m_branchLabel->setText(tr("Current branch: <b>%1</b>&nbsp;&nbsp;&nbsp;&nbsp;Repository: %2"
                                  "&nbsp;&nbsp;&nbsp;&nbsp;Remote: %3")
                                   .arg(branchText.toHtmlEscaped(), m_cwd.toHtmlEscaped(),
                                        remote[0] ? QString::fromUtf8(remote).toHtmlEscaped() : tr("(none)")));
        setWindowTitle(QStringLiteral("AMIVCS GUI - %1 [%2]").arg(m_cwd, branchText));
    } else {
        m_branchLabel->setText(tr("(not a repository -- use Repository > Init Repository Here)"));
        setWindowTitle(tr("AMIVCS GUI"));
    }

    showPreview(nullptr);
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
    m_rescanBtn->setEnabled(open);
    m_pushBtn->setEnabled(open);
    m_pullBtn->setEnabled(open);
    m_actSettings->setEnabled(open);
}

// The right-hand pane: the selected file's current content (the first
// 32 KB, binary files only named) -- a diff is to come.
void MainWindow::showPreview(QListWidgetItem *item)
{
    if (!item) {
        m_previewTitle->clear();
        m_preview->clear();
        return;
    }
    const QByteArray raw = item->data(Qt::UserRole).toByteArray();
    const QChar kind = item->data(Qt::UserRole + 1).toChar();
    const QString shown = shownPath(raw.constData());
    m_previewTitle->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;(%2)")
                                .arg(shown.toHtmlEscaped(), kindLabel(kind.toLatin1())));
    if (kind == QLatin1Char('D')) {
        m_preview->setPlainText(tr("(file deleted in the working directory)"));
        return;
    }
    QFile f(QDir(m_cwd).filePath(QString::fromUtf8(raw)));
    if (!f.open(QIODevice::ReadOnly)) {
        m_preview->setPlainText(tr("(cannot read the file)"));
        return;
    }
    const qint64 limit = 32 * 1024;
    QByteArray data = f.read(limit);
    if (data.contains('\0')) {
        m_preview->setPlainText(tr("(binary file)"));
        return;
    }
    QString text = QString::fromUtf8(data);
    if (f.size() > limit)
        text += tr("\n\n... (only the first 32 KB shown)");
    m_preview->setPlainText(text);
}

// Refresh: read the working tree again (files changed outside), then
// redraw -- refreshUI() alone only shows what the session holds
void MainWindow::onRefresh()
{
    if (m_session.is_open) vcsgui_refresh(&m_session);
    refreshUI();
}

void MainWindow::onStageSelected()
{
    QList<QListWidgetItem *> items = m_unstagedList->selectedItems();
    if (items.isEmpty() && m_unstagedList->currentItem()) items << m_unstagedList->currentItem();
    for (QListWidgetItem *item : items) {
        QByteArray path = item->data(Qt::UserRole).toByteArray();
        vcsgui_add(&m_session, path.constData());
    }
    refreshUI();
}

void MainWindow::onUnstageSelected()
{
    QList<QListWidgetItem *> items = m_stagedList->selectedItems();
    if (items.isEmpty() && m_stagedList->currentItem()) items << m_stagedList->currentItem();
    for (QListWidgetItem *item : items) {
        QByteArray path = item->data(Qt::UserRole).toByteArray();
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
    if (m_stageAllCheck->isChecked()) onRefresh();   // -a takes what is changed now
    if (m_session.staged.n == 0 && !(m_stageAllCheck->isChecked() && m_session.unstaged.n > 0)) {
        QMessageBox::information(this, tr("Commit"),
            tr("Nothing to commit.\n\nStage files first (double-click them in \"Unstaged Changes\", "
               "or \"Stage selected\"/\"Stage all\"), or tick \"Stage all changed files before commit\"."));
        return;
    }
    {
        // like git: no commit without a name; asked once, kept globally
        char ident[300];
        if (!vcsgui_identity(&m_session, ident, sizeof(ident))) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Who are you?"),
                tr("Commits carry your name and e-mail. They are kept as defaults for every\n"
                   "repository (Repository > Settings... changes them).\n\nYour name:"),
                QLineEdit::Normal, QString(), &ok).trimmed();
            if (!ok || name.isEmpty()) return;
            const QString email = QInputDialog::getText(this, tr("Who are you?"), tr("Your e-mail (may be empty):"),
                                                        QLineEdit::Normal, QString(), &ok).trimmed();
            if (!ok) return;
            vcs_global_set("name", name.toUtf8().constData());
            vcs_global_set("email", email.toUtf8().constData());
        }
    }
    QString msg = m_commitMsg->toPlainText().trimmed();
    if (msg.isEmpty()) {
        QMessageBox::information(this, tr("Commit"),
            tr("Please type a commit message into the \"Commit message\" field first."));
        m_commitMsg->setFocus();
        return;
    }
    QByteArray msgUtf8 = msg.normalized(QString::NormalizationForm_C).toUtf8();   // repository text: UTF-8 NFC
    VcsOid oid;
    if (!vcsgui_commit(&m_session, msgUtf8.constData(), m_stageAllCheck->isChecked() ? 1 : 0, &oid)) {
        QMessageBox::warning(this, tr("Commit"), QString::fromUtf8(vcsgui_last_message()));
        return;
    }
    m_commitMsg->clear();
    m_stageAllCheck->setChecked(false);
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

void MainWindow::onSettings()
{
    if (!m_session.is_open) return;
    SettingsDialog dlg(QString::fromUtf8(m_session.repo.amivcs_dir), this);
    if (dlg.exec() == QDialog::Accepted) refreshUI();
}

// Starts a clone/push/pull. The login is asked for when the repository
// remembers no password, or again (up to three times) after the server
// refused it -- see the jobFinished handler.
void MainWindow::startRemoteJob(RemoteJob::Kind kind, const QString &url, const QString &extra, int attempt)
{
    if (m_job) return;

    char remote[1024], user[64];
    int hasPass = 0;
    QString shownUrl = url;
    if (kind != RemoteJob::Clone) {
        vcsgui_remote_info(&m_session, remote, sizeof(remote), user, sizeof(user), &hasPass);
        if (shownUrl.isEmpty()) shownUrl = QString::fromUtf8(remote);
        else hasPass = 0;
    } else {
        user[0] = '\0';
    }

    VcsGuiLogin login;
    std::memset(&login, 0, sizeof(login));
    std::strncpy(login.user, m_lastUser.isEmpty() ? user : m_lastUser.toUtf8().constData(), sizeof(login.user) - 1);
    if (attempt > 0 || !hasPass) {
        RemoteDialog dlg(shownUrl, QString::fromUtf8(login.user), attempt > 0, this);
        if (dlg.exec() != QDialog::Accepted) return;
        login = dlg.result();
        if (!login.user[0] || !login.pass[0]) return;
        m_lastUser = QString::fromUtf8(login.user);
    }

    auto *dlg = new QProgressDialog(tr("Working..."), tr("Cancel"), 0, 0, this);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);

    m_job = new RemoteJob(kind, &m_session, m_cfg, url, login, extra, this);
    std::memset(login.pass, 0, sizeof(login.pass));

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
    connect(m_job, &RemoteJob::jobFinished, this, [this, dlg, kind, url, extra, attempt](bool ok) {
        dlg->close();
        dlg->deleteLater();
        RemoteJob *job = m_job;
        m_job = nullptr;
        job->deleteLater();
        if (!ok && vcsgui_auth_failed() && attempt < 2) {
            startRemoteJob(kind, url, extra, attempt + 1);   // login refused: ask again
            return;
        }
        refreshUI();
        if (ok) statusBar()->showMessage(QString::fromUtf8(vcsgui_last_message()), 5000);
        else QMessageBox::warning(this, tr("AMIVCS"), QString::fromUtf8(vcsgui_last_message()));
    });

    m_job->start();
    dlg->show();
}

void MainWindow::onClone()
{
    bool ok;
    QString url = QInputDialog::getText(this, tr("Clone"), tr("Repository URL (https://server/name):"),
                                        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || url.isEmpty()) return;
    char server[1024], name[256];
    if (!vcs_url_split(url.toUtf8().constData(), server, sizeof(server), name, sizeof(name))) {
        QMessageBox::warning(this, tr("Clone"), tr("'%1' is not a repository URL (https://server/name).").arg(url));
        return;
    }
    QString parent = QFileDialog::getExistingDirectory(this, tr("Clone into which directory?"));
    if (parent.isEmpty()) return;
    // like git: a new directory named after the repository
    startRemoteJob(RemoteJob::Clone, url, QDir(parent).filePath(QString::fromUtf8(name)), 0);
}

void MainWindow::onPush()
{
    if (!m_session.is_open) return;
    char remote[1024], user[64];
    QString url;
    if (!vcsgui_remote_info(&m_session, remote, sizeof(remote), user, sizeof(user), nullptr)) {
        // first push: where to? A new name is created on the server.
        bool ok;
        url = QInputDialog::getText(this, tr("Push"), tr("Push to repository URL (https://server/name):"),
                                    QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || url.isEmpty()) return;
    }
    startRemoteJob(RemoteJob::Push, url, QString(), 0);
}

void MainWindow::onPull()
{
    if (!m_session.is_open) return;
    char remote[1024], user[64];
    if (!vcsgui_remote_info(&m_session, remote, sizeof(remote), user, sizeof(user), nullptr)) {
        QMessageBox::information(this, tr("Pull"),
                                 tr("This repository has no remote yet. Push it once, or set the URL "
                                    "under Repository > Settings..."));
        return;
    }
    startRemoteJob(RemoteJob::Pull, QString(), QString(), 0);
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About AmiVCS GUI"),
        tr("AmiVCS GUI v%1\n\nA minimal, git-gui-style graphical front-end for AmiVCS.\n\n"
           "(C) 2026 Ronny Krueger\nGNU General Public License v3 or later").arg(QString::fromUtf8(AC_VERSION)));
}
