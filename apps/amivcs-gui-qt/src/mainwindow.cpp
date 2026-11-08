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
#include <QFrame>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QItemSelectionModel>
#include <QVector>
#include <QPair>
#include <cstring>

// ---- diff rendering (repodiff.h sink -> coloured lines) ----
namespace {
struct DiffLines {
    QVector<QPair<char, QString>> lines;   // ' ' '+' '-' '@' (hunk) 'F' (file) '!' (note)
    int limit = 4000;
    bool cut = false;
    void add(char op, const QString &t)
    {
        if (lines.size() >= limit) { cut = true; return; }
        lines.append(qMakePair(op, t));
    }
};

void dl_file(const VcsFileDiff *f, void *ctx)
{
    auto *d = static_cast<DiffLines *>(ctx);
    char *u = vcs_text_utf8(f->path);
    QString path = QString::fromUtf8(u ? u : f->path);
    free(u);
    d->add('F', path);
    if (f->binary) d->add('!', QObject::tr("(binary file or larger than 512 KB -- no line diff)"));
}

void dl_hunk(long as, long ac, long bs, long bc, void *ctx)
{
    static_cast<DiffLines *>(ctx)->add('@', QStringLiteral("@@ -%1,%2 +%3,%4 @@").arg(as).arg(ac).arg(bs).arg(bc));
}

void dl_line(char op, const char *text, long len, int eol, void *ctx)
{
    auto *d = static_cast<DiffLines *>(ctx);
    d->add(op, QString::fromUtf8(text, (int)len));
    if (!eol) d->add('!', QObject::tr("\\ No newline at end of file"));
}

const VcsRepoDiffSink kDiffSink = { dl_file, { dl_hunk, dl_line } };

// writes the lines with tinted backgrounds that work on light and dark palettes
void renderDiff(QPlainTextEdit *view, const DiffLines &d, bool showFileHeads)
{
    view->clear();
    QTextCursor c(view->document());
    QTextCharFormat plain, add, del, hunk, file, note;
    add.setBackground(QColor(40, 170, 60, 60));
    del.setBackground(QColor(220, 50, 50, 60));
    hunk.setForeground(QColor(120, 120, 200));
    file.setFontWeight(QFont::Bold);
    note.setFontItalic(true);
    bool first = true;
    for (const auto &l : d.lines) {
        if (l.first == 'F' && !showFileHeads) continue;
        if (!first) c.insertText(QStringLiteral("\n"), plain);
        first = false;
        switch (l.first) {
        case '+': c.insertText(QStringLiteral("+") + l.second, add); break;
        case '-': c.insertText(QStringLiteral("-") + l.second, del); break;
        case '@': c.insertText(l.second, hunk); break;
        case 'F': c.insertText(QStringLiteral("\n") + l.second, file); break;
        case '!': c.insertText(l.second, note); break;
        default:  c.insertText(QStringLiteral(" ") + l.second, plain); break;
        }
    }
    if (d.cut) c.insertText(QObject::tr("\n\n... (only the first %1 lines shown)").arg(d.limit), note);
    view->moveCursor(QTextCursor::Start);
}
} // namespace

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

    // shown while a merge with conflicts is in progress
    m_mergeBanner = new QFrame(this);
    m_mergeBanner->setFrameShape(QFrame::StyledPanel);
    m_mergeBanner->setStyleSheet(QStringLiteral("QFrame { background: rgba(230, 160, 0, 60); }"));
    auto *bannerLayout = new QHBoxLayout(m_mergeBanner);
    m_mergeLabel = new QLabel(m_mergeBanner);
    m_mergeLabel->setTextFormat(Qt::RichText);
    m_mergeLabel->setWordWrap(true);
    bannerLayout->addWidget(m_mergeLabel, 1);
    auto *abortBtn = new QPushButton(tr("A&bort Merge"), m_mergeBanner);
    abortBtn->setToolTip(tr("Give up the merge: every file goes back to how it was before"));
    bannerLayout->addWidget(abortBtn);
    connect(abortBtn, &QPushButton::clicked, this, &MainWindow::onAbortMerge);
    m_mergeBanner->hide();
    mainLayout->addWidget(m_mergeBanner);

    auto *tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs, 1);

    // ---- tab "Changes" ----
    auto *changesPage = new QWidget(this);
    auto *changesLayout = new QVBoxLayout(changesPage);
    auto *vertSplit = new QSplitter(Qt::Vertical, changesPage);
    auto *topSplit = new QSplitter(Qt::Horizontal, vertSplit);
    auto *listsSplit = new QSplitter(Qt::Vertical, topSplit);

    m_conflictBox = new QWidget(listsSplit);
    auto *conflictLayout = new QVBoxLayout(m_conflictBox);
    conflictLayout->setContentsMargins(0, 0, 0, 0);
    conflictLayout->addWidget(new QLabel(tr("<b>Conflicts</b> (edit the file and mark it resolved, or take one side)"), this));
    m_conflictList = new QListWidget(this);
    m_conflictList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    conflictLayout->addWidget(m_conflictList);
    auto *conflictBtns = new QHBoxLayout;
    auto *oursBtn = new QPushButton(tr("Take &ours"), this);
    auto *theirsBtn = new QPushButton(tr("Take th&eirs"), this);
    auto *resolvedBtn = new QPushButton(tr("&Mark resolved"), this);
    oursBtn->setToolTip(tr("Keep this branch's version of the whole file"));
    theirsBtn->setToolTip(tr("Use the merged branch's version of the whole file"));
    resolvedBtn->setToolTip(tr("The file is fixed by hand -- stage it as it is now"));
    conflictBtns->addWidget(oursBtn);
    conflictBtns->addWidget(theirsBtn);
    conflictBtns->addWidget(resolvedBtn);
    conflictLayout->addLayout(conflictBtns);
    connect(oursBtn, &QPushButton::clicked, this, &MainWindow::onTakeOurs);
    connect(theirsBtn, &QPushButton::clicked, this, &MainWindow::onTakeTheirs);
    connect(resolvedBtn, &QPushButton::clicked, this, &MainWindow::onMarkResolved);
    connect(m_conflictList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) { showConflictPreview(item); });
    m_conflictBox->hide();

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
    m_historyView->setSelectionMode(QAbstractItemView::SingleSelection);
    auto *historySplit = new QSplitter(Qt::Vertical, historyPage);
    historySplit->addWidget(m_historyView);
    m_commitDiff = new QPlainTextEdit(this);
    m_commitDiff->setReadOnly(true);
    m_commitDiff->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_commitDiff->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_commitDiff->setPlaceholderText(tr("Select a commit to see its changes."));
    historySplit->addWidget(m_commitDiff);
    historySplit->setStretchFactor(0, 55);
    historySplit->setStretchFactor(1, 45);
    historyLayout->addWidget(historySplit);
    connect(m_historyView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &, const QModelIndex &) { onHistorySelected(); });
    auto *branchBtns = new QHBoxLayout;
    auto *newBranchBtn = new QPushButton(tr("New Branch..."), this);
    auto *switchBtn = new QPushButton(tr("Switch..."), this);
    auto *mergeBtn = new QPushButton(tr("Merge..."), this);
    m_mergeBtn = mergeBtn;
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
    m_actAbortMerge = repoMenu->addAction(tr("&Abort Merge"), this, &MainWindow::onAbortMerge);

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

        m_conflictList->clear();
        if (m_session.merge.active) {
            for (int i = 0; i < m_session.merge.n; i++) {
                const VcsMergeFile &f = m_session.merge.items[i];
                if (!vcs_mergestate_is_conflict(&f)) continue;
                QString text = QStringLiteral("%1  %2").arg(QString::fromLatin1(f.kind), shownPath(f.path));
                if (f.status != 'U') text += tr("  (resolved)");
                else if (f.theirs_file) text += tr("  (theirs beside it as .theirs)");
                auto *item = new QListWidgetItem(text);
                item->setData(Qt::UserRole, QByteArray(f.path));
                item->setData(Qt::UserRole + 1, QChar(f.status));
                if (f.status != 'U') item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
                m_conflictList->addItem(item);
            }
            const int open = vcsgui_conflicts_open(&m_session);
            const QString label = QString::fromUtf8(m_session.merge.label).toHtmlEscaped();
            m_mergeLabel->setText(open
                ? tr("<b>Merging '%1'</b> -- %n conflict(s) to resolve. Edit the files (look for the "
                     "&lt;&lt;&lt;&lt;&lt;&lt;&lt; markers) and mark them resolved, or take one side; "
                     "then commit.", nullptr, open).arg(label)
                : tr("<b>Merging '%1'</b> -- all conflicts resolved. Commit to finish the merge.").arg(label));
            // the message box gets the usual merge message, once
            const QString mergeMsg = QStringLiteral("Merge branch '%1'").arg(QString::fromUtf8(m_session.merge.label));
            if (m_commitMsg->toPlainText().trimmed().isEmpty() && m_mergeMsgShown != mergeMsg) {
                m_commitMsg->setPlainText(mergeMsg);
                m_mergeMsgShown = mergeMsg;
            }
        } else {
            m_mergeMsgShown.clear();
        }
        m_mergeBanner->setVisible(m_session.merge.active);
        m_conflictBox->setVisible(m_session.merge.active);

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
        m_mergeBanner->hide();
        m_conflictBox->hide();
        setWindowTitle(tr("AMIVCS GUI"));
    }

    showPreview(nullptr);
    m_commitDiff->clear();
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
    m_actMerge->setEnabled(open && !m_session.merge.active);
    m_mergeBtn->setEnabled(open && !m_session.merge.active);
    m_actAbortMerge->setEnabled(open && m_session.merge.active);
    m_actPush->setEnabled(open);
    m_actPull->setEnabled(open);
    m_stageBtn->setEnabled(open);
    m_unstageBtn->setEnabled(open);
    m_stageAllBtn->setEnabled(open);
    m_commitBtn->setEnabled(open && vcsgui_conflicts_open(&m_session) == 0);
    m_rescanBtn->setEnabled(open);
    m_pushBtn->setEnabled(open);
    m_pullBtn->setEnabled(open);
    m_actSettings->setEnabled(open);
}

// The right-hand pane: what changed in the selected file, as a line
// diff (staged: against HEAD; unstaged: against what is staged).
void MainWindow::showPreview(QListWidgetItem *item)
{
    if (!item) {
        m_previewTitle->clear();
        m_preview->clear();
        return;
    }
    const QByteArray raw = item->data(Qt::UserRole).toByteArray();
    const QChar kind = item->data(Qt::UserRole + 1).toChar();
    const bool staged = item->listWidget() == m_stagedList;
    const QString shown = shownPath(raw.constData());
    m_previewTitle->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;(%2, %3)")
                                .arg(shown.toHtmlEscaped(), kindLabel(kind.toLatin1()),
                                     staged ? tr("staged") : tr("not staged")));
    DiffLines d;
    const int n = vcsgui_diff_file(&m_session, raw.constData(), staged ? 1 : 0, &kDiffSink, &d);
    if (n < 0) {
        m_preview->setPlainText(tr("(cannot read the file)"));
        return;
    }
    if (d.lines.size() <= 1) {   // only the file head: no line changes
        m_preview->setPlainText(d.lines.isEmpty() || d.lines.first().first != 'F'
                                    ? tr("(no changes)") : tr("(no line changes)"));
        if (d.lines.size() == 1 && d.lines.first().first == '!') renderDiff(m_preview, d, false);
        return;
    }
    renderDiff(m_preview, d, false);
}

// A conflicting file as it is now in the working tree, with the
// conflict markers highlighted.
void MainWindow::showConflictPreview(QListWidgetItem *item)
{
    if (!item) return;
    const QByteArray raw = item->data(Qt::UserRole).toByteArray();
    m_previewTitle->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;(%2)")
                                .arg(shownPath(raw.constData()).toHtmlEscaped(), tr("conflict")));
    QFile f(QDir(m_cwd).filePath(QString::fromUtf8(raw)));
    if (!f.open(QIODevice::ReadOnly)) {
        m_preview->setPlainText(tr("(the file is not in the working tree -- take a side, "
                                   "or mark it resolved to keep it deleted)"));
        return;
    }
    const qint64 limit = 512 * 1024;
    QByteArray data = f.read(limit);
    if (data.contains('\0')) {
        m_preview->setPlainText(tr("(binary file -- the other side is beside it as .theirs)"));
        return;
    }
    m_preview->clear();
    QTextCursor c(m_preview->document());
    QTextCharFormat plain, marker, ours, theirs;
    marker.setFontWeight(QFont::Bold);
    marker.setBackground(QColor(230, 160, 0, 90));
    ours.setBackground(QColor(60, 120, 220, 45));
    theirs.setBackground(QColor(40, 170, 60, 45));
    int zone = 0;   // 0 outside, 1 ours, 2 theirs
    const QList<QByteArray> lines = data.split('\n');
    for (int i = 0; i < lines.size(); i++) {
        QByteArray l = lines[i];
        if (l.endsWith('\r')) l.chop(1);
        if (i) c.insertText(QStringLiteral("\n"), plain);
        const QString t = QString::fromUtf8(l);
        if (l.startsWith("<<<<<<<")) { zone = 1; c.insertText(t, marker); }
        else if (l.startsWith("=======") && zone == 1) { zone = 2; c.insertText(t, marker); }
        else if (l.startsWith(">>>>>>>") && zone == 2) { zone = 0; c.insertText(t, marker); }
        else c.insertText(t, zone == 1 ? ours : zone == 2 ? theirs : plain);
    }
    m_preview->moveCursor(QTextCursor::Start);
}

void MainWindow::onHistorySelected()
{
    const QModelIndex idx = m_historyView->currentIndex();
    m_commitDiff->clear();
    if (!idx.isValid() || idx.row() >= m_session.log_all.n) return;
    const VcsLogEntry &e = m_session.log_all.items[idx.row()];
    DiffLines d;
    char hex[VCS_OID_HEX];
    vcs_oid_to_hex(&e.oid, hex);
    d.add('!', tr("commit %1%2").arg(QString::fromLatin1(hex),
                                     e.nparents > 1 ? tr("  (merge -- changes against the first parent)") : QString()));
    d.add('!', QString::fromUtf8(e.message ? e.message : "").trimmed());
    if (vcsgui_diff_commit(&m_session, &e.oid, &kDiffSink, &d) < 0) d.add('!', tr("(cannot read the commit)"));
    renderDiff(m_commitDiff, d, true);
}

void MainWindow::resolveSelected(int how)
{
    QList<QListWidgetItem *> items = m_conflictList->selectedItems();
    if (items.isEmpty() && m_conflictList->currentItem()) items << m_conflictList->currentItem();
    if (items.isEmpty()) {
        QMessageBox::information(this, tr("Conflicts"), tr("Select a conflict in the list first."));
        return;
    }
    for (QListWidgetItem *item : items) {
        const QByteArray path = item->data(Qt::UserRole).toByteArray();
        const int ok = how == 2 ? vcsgui_add(&m_session, path.constData())
                                : vcsgui_resolve(&m_session, path.constData(), how);
        if (!ok) {
            QMessageBox::warning(this, tr("Conflicts"), QString::fromUtf8(vcsgui_last_message()));
            break;
        }
    }
    refreshUI();
}

void MainWindow::onTakeOurs() { resolveSelected(0); }
void MainWindow::onTakeTheirs() { resolveSelected(1); }

void MainWindow::onMarkResolved()
{
    // a file still holding markers is most likely not done yet
    QList<QListWidgetItem *> items = m_conflictList->selectedItems();
    if (items.isEmpty() && m_conflictList->currentItem()) items << m_conflictList->currentItem();
    for (QListWidgetItem *item : items) {
        QFile f(QDir(m_cwd).filePath(QString::fromUtf8(item->data(Qt::UserRole).toByteArray())));
        QByteArray head;
        if (f.open(QIODevice::ReadOnly)) head = f.read(512 * 1024);
        if (head.startsWith("<<<<<<< ") || head.contains("\n<<<<<<< ")) {
            if (QMessageBox::question(this, tr("Mark resolved"),
                    tr("%1 still contains conflict markers (<<<<<<<).\n\nMark it resolved anyway?")
                        .arg(item->text())) != QMessageBox::Yes)
                return;
        }
    }
    resolveSelected(2);
}

void MainWindow::onAbortMerge()
{
    if (!m_session.merge.active) return;
    if (QMessageBox::question(this, tr("Abort Merge"),
            tr("Give up merging '%1'?\n\nEvery file the merge changed goes back to how it was before, "
               "including your edits to conflicting files. Files not under version control stay.")
                .arg(QString::fromUtf8(m_session.merge.label))) != QMessageBox::Yes)
        return;
    if (!vcsgui_merge_abort(&m_session)) {
        QMessageBox::warning(this, tr("Abort Merge"), QString::fromUtf8(vcsgui_last_message()));
    } else {
        if (m_commitMsg->toPlainText().trimmed() == m_mergeMsgShown) m_commitMsg->clear();
        statusBar()->showMessage(tr("Merge aborted."), 5000);
    }
    refreshUI();
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
    if (!m_session.merge.active && m_session.staged.n == 0
        && !(m_stageAllCheck->isChecked() && m_session.unstaged.n > 0)) {
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
    if (msg.isEmpty() && !m_session.merge.active) {
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
    if (m_session.merge.active) return;   // the buttons are off; finish or abort it first
    if (!vcsgui_merge(&m_session, branch.toUtf8().constData())) {
        vcsgui_refresh(&m_session);
        if (m_session.merge.active)
            QMessageBox::information(this, tr("Merge"),
                tr("The merge stopped on %n conflict(s). They are listed under \"Conflicts\": "
                   "edit each file and mark it resolved, or take one side as a whole. "
                   "Then commit -- or abort the merge.", nullptr, vcsgui_conflicts_open(&m_session)));
        else
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
