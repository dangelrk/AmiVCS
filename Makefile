# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# AmiVCS -- Top-Level-Makefile. "make help" zeigt alle Ziele; Details in
# docs/BUILD.md. Die eigentliche Logik liegt in core/mk/project.mk
# (AmiCloudCore-Submodul), jedes Programm hat zusaetzlich sein eigenes
# Makefile.pc / Makefile.amiga-cross / CMakeLists.txt.

PROJECT    = AmiVCS
PC_DIRS    = apps/amivcs
QT_DIRS    = apps/amivcs-gui-qt
AMIGA_DIRS = apps/amivcs apps/amivcs-gui-mui apps/amivcs-prefs

include core/mk/project.mk
