/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "mainwindow.h"

extern "C" {
#include "config.h"
#include "vcsconfig.h"
#include "session.h"
extern int ac_console_progress;
extern int ac_errors_to_stderr;
}

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <cstdlib>

int main(int argc, char *argv[])
{
    ac_console_progress = 0;   /* GUI never wants the CLI's \r status line */
    ac_errors_to_stderr = 0;   /* windowed GUI app -- never write to stderr (libcloudhttp) */

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AMIVCS GUI"));
    QApplication::setApplicationVersion(QString::fromUtf8(AC_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AMIVCS GUI -- git-gui-style AMIVCS front-end"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"),
        QStringLiteral("Repository to open (default: current directory)"), QStringLiteral("[path]"));
    parser.process(app);

    vcsgui_install_hooks();

    AcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.basepath[0] = '/';
    cfg.verify_ssl  = 1;
    /* program options from ~/.amivcsconfig; remote, login and identity
     * are per repository (vcsconfig.h) */
    vcs_global_load_options(&cfg);

    /* No repo picker -- opens the current working directory by default,
     * exactly like the CLI's "must be run from the repository root" rule
     * (libcloudvcs/include/cloudvcs/repo.h); an optional positional
     * argument overrides that with an explicit path instead. */
    const QStringList positional = parser.positionalArguments();
    QString cwd = positional.isEmpty() ? QDir::currentPath() : QFileInfo(positional.first()).absoluteFilePath();

    MainWindow window(cwd, cfg);
    window.show();

    return app.exec();
}
