/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

extern "C" {
#include "session.h"
}

#include "remotejob.h"

class QListWidget;
class QPlainTextEdit;
class QCheckBox;
class QPushButton;
class QTableView;
class QLabel;
class QListWidgetItem;
class LogTableModel;
class GraphDelegate;

/* Single-window git-gui-style AMIVCS frontend -- staged/unstaged split,
 * commit box, full multi-branch history with a drawn commit graph, and
 * clone/push/pull with a cancellable progress dialog. Opens the current
 * working directory as the repository (no picker), matching the CLI's
 * "must be run from the repository root" rule (repo.h). All VCS/staging
 * logic goes through VcsGuiSession (libcloudvcsgui) -- this class only
 * owns widgets and wires them to it. */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString &cwd, const AcConfig &cfg, QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStageSelected();
    void onUnstageSelected();
    void onStageAll();
    void onCommit();
    void onRefresh();
    void onNewBranch();
    void onSwitchBranch();
    void onMerge();
    void onInitRepoHere();
    void onClone();
    void onPush();
    void onPull();
    void onAbout();

private:
    void openOrOfferInit(const QString &dir);
    void refreshUI();
    void updateActionsEnabled();
    void showPreview(QListWidgetItem *item);
    bool ensureRemoteConfigured();
    QString pickBranch(const QString &title, const QString &label);
    void startRemoteJob(RemoteJob::Kind kind, const QString &repoName, const QString &extra);

    VcsGuiSession m_session;
    AcConfig m_cfg;
    QString m_cwd;

    QListWidget *m_unstagedList;
    QListWidget *m_stagedList;
    QPushButton *m_stageBtn;
    QPushButton *m_unstageBtn;
    QPushButton *m_stageAllBtn;
    QPlainTextEdit *m_commitMsg;
    QCheckBox *m_stageAllCheck;
    QPushButton *m_commitBtn;
    QTableView *m_historyView;
    LogTableModel *m_logModel;
    GraphDelegate *m_graphDelegate;
    QLabel *m_branchLabel;
    QLabel *m_previewTitle;
    QPlainTextEdit *m_preview;
    QPushButton *m_rescanBtn;
    QPushButton *m_pushBtn;
    QPushButton *m_pullBtn;

    QAction *m_actInit;
    QAction *m_actClone;
    QAction *m_actPush;
    QAction *m_actPull;
    QAction *m_actNewBranch;
    QAction *m_actSwitch;
    QAction *m_actMerge;
    QAction *m_actRefresh;

    RemoteJob *m_job;
};

#endif // MAINWINDOW_H
